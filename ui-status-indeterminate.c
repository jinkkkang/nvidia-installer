/*
 * Copyright (C) 2023 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses>.
 *
 * 【文件说明】不确定进度指示器实现。
 *
 * 当安装器执行无法预估完成时间的操作（如 initramfs 重建、内核模块编译等）时，
 * 需要向用户显示一个"正在处理中"的动态指示（类似旋转图标或来回滚动的进度条），
 * 表明程序仍在正常运行，而不是挂起。
 *
 * 实现原理：
 *   1. 主线程调用 indeterminate_begin() 创建一个后台 pthread 工作线程
 *   2. 工作线程定期调用 UI 层的更新回调函数来刷新进度显示
 *   3. 主线程操作完成后调用 indeterminate_end()，将状态设为 INACTIVE
 *   4. 工作线程检测到状态变为 INACTIVE 后退出
 *   5. 主线程通过 pthread_join 等待工作线程结束
 *
 * 线程安全：
 *   状态变量的读写通过 pthread_mutex 互斥锁保护，
 *   确保主线程和工作线程之间的状态同步。
 *   如果互斥锁操作失败，状态将被标记为 INVALID，
 *   后续所有操作变为空操作（fail-safe 设计）。
 */

#include <stdlib.h>      /* 标准库：calloc, free */
#include <pthread.h>     /* POSIX 线程：互斥锁和线程管理 */

#include "ui-status-indeterminate.h"  /* 本模块头文件：状态枚举和接口声明 */

/*
 * __indeterminate_data - 不确定进度指示器的内部数据结构。
 *
 * 在头文件中声明为不透明类型（opaque type），外部代码只能通过
 * 接口函数访问，不能直接操作内部成员，实现封装。
 */
struct __indeterminate_data {
    pthread_mutex_t mutex;     /* 互斥锁：保护 state 字段的并发访问 */
    pthread_t thread;          /* 后台工作线程句柄 */
    IndeterminateState state;  /* 当前状态：INVALID / INACTIVE / ACTIVE */
};

/*
 * indeterminate_init() - 创建并初始化一个不确定进度指示器。
 *
 * 分配 IndeterminateData 结构体并初始化其互斥锁。
 * 初始状态为 INACTIVE（非活跃），表示指示器已准备就绪但未开始运行。
 * 如果互斥锁初始化失败，状态被设为 INVALID，后续操作将安全地变为空操作。
 *
 * 返回值：
 *   成功 - 指向新创建的 IndeterminateData 的指针
 *   失败 - NULL（内存分配失败时）
 */

IndeterminateData *indeterminate_init(void)
{
    /* calloc 将所有字段初始化为 0 */
    IndeterminateData *ret = calloc(1, sizeof(*ret));

    if (ret) {
        /* 初始化互斥锁（使用默认属性） */
        if (pthread_mutex_init(&ret->mutex, NULL) == 0) {
            ret->state = INDETERMINATE_INACTIVE;
        } else {
            /* 互斥锁初始化失败，标记为无效状态 */
            ret->state = INDETERMINATE_INVALID;
        }
    }

    return ret;
}

/*
 * indeterminate_destroy() - 销毁不确定进度指示器，释放所有资源。
 *
 * 注意：调用此函数前应确保已调用 indeterminate_end() 停止工作线程。
 * 如果在工作线程仍在运行时销毁，可能导致未定义行为。
 *
 * 参数：
 *   d - 要销毁的指示器指针（可为 NULL，此时函数为空操作）
 */

void indeterminate_destroy(IndeterminateData *d)
{
    if (d) {
        /* 只有状态不是 INVALID 时互斥锁才被成功初始化过，需要销毁 */
        if (d->state != INDETERMINATE_INVALID) {
            pthread_mutex_destroy(&d->mutex);
        }
        free(d);
    }
}

/*
 * indeterminate_get() - 线程安全地获取指示器的当前状态。
 *
 * 通过加锁-读取-解锁的方式安全地读取状态值。
 * 如果任何互斥锁操作失败，返回 INVALID。
 *
 * 此函数被工作线程和主线程同时使用：
 *   - 工作线程定期调用此函数检查是否应该退出（状态变为 INACTIVE）
 *   - 主线程调用此函数检查工作线程是否仍在运行
 *
 * 参数：
 *   d - 指示器指针
 *
 * 返回值：当前状态（INVALID / INACTIVE / ACTIVE）。
 */

IndeterminateState indeterminate_get(IndeterminateData *d)
{
    IndeterminateState state = INDETERMINATE_INVALID;

    if (d && d->state != INDETERMINATE_INVALID) {
        /* 加锁以安全读取状态 */
        if (!pthread_mutex_lock(&d->mutex)) {
            state = d->state;
            /* 解锁失败则认为状态不可信 */
            if (pthread_mutex_unlock(&d->mutex)) {
                state = INDETERMINATE_INVALID;
            }
        }
    }

    return state;
}

/*
 * indeterminate_set() - 线程安全地设置指示器状态（内部函数）。
 *
 * 通过加锁-写入-解锁的方式安全地更新状态值。
 * 如果互斥锁操作失败，将状态强制设为 INVALID（fail-safe）。
 *
 * 参数：
 *   d     - 指示器指针
 *   state - 要设置的新状态
 */
static void indeterminate_set(IndeterminateData *d, IndeterminateState state)
{
    if (!d || d->state == INDETERMINATE_INVALID) {
        return;
    } else if (pthread_mutex_lock(&d->mutex)) {
        /* 加锁失败，标记为无效状态 */
        d->state = INDETERMINATE_INVALID;
        return;
    }

    d->state = state;

    if (pthread_mutex_unlock(&d->mutex)) {
        /* 解锁失败，标记为无效状态 */
        d->state = INDETERMINATE_INVALID;
    }
}

/*
 * indeterminate_begin() - 启动一个新的不确定进度指示器。
 *
 * 如果已有一个正在运行的指示器，先停止它（调用 indeterminate_end），
 * 然后创建新的后台工作线程来显示进度动画。
 *
 * 工作线程的职责：
 *   - 工作线程应定期检查状态（通过 indeterminate_get），
 *     当状态变为 INACTIVE 时应主动退出。
 *   - 工作线程负责实际的 UI 更新（例如刷新进度动画）。
 *
 * 参数：
 *   d      - 指示器指针
 *   worker - 工作线程入口函数指针
 *   args   - 传递给工作线程的参数
 */

void indeterminate_begin(IndeterminateData *d, void *(*worker)(void *),
                         void *args)
{
    if (!d || indeterminate_get(d) == INDETERMINATE_INVALID) {
        return;
    }

    /* 如果已有活跃的指示器，先停止它 */
    indeterminate_end(d);

    /* 创建新的后台工作线程 */
    if (pthread_create(&d->thread, NULL, worker, args)) {
        /* 线程创建失败，标记为无效状态 */
        indeterminate_set(d, INDETERMINATE_INVALID);
    } else {
        /* 线程创建成功，标记为活跃状态 */
        indeterminate_set(d, INDETERMINATE_ACTIVE);
    }
}

/*
 * indeterminate_end() - 停止不确定进度指示器并等待工作线程结束。
 *
 * 处理流程：
 *   1. 检查指示器是否处于 ACTIVE 状态（如果不是则直接返回）
 *   2. 将状态设为 INACTIVE，通知工作线程退出
 *   3. 调用 pthread_join 等待工作线程完成清理并退出
 *
 * 此函数是阻塞的：在工作线程退出前不会返回。
 * 工作线程应在检测到状态变为 INACTIVE 后尽快退出，
 * 以避免长时间阻塞主线程。
 *
 * 参数：
 *   d - 指示器指针（可为 NULL，此时函数为空操作）
 */

void indeterminate_end(IndeterminateData *d)
{
    void *join_ret;

    /* 只有在 ACTIVE 状态下才需要停止 */
    if (!d || indeterminate_get(d) != INDETERMINATE_ACTIVE) {
        return;
    }

    /* 将状态设为 INACTIVE，通知工作线程退出 */
    indeterminate_set(d, INDETERMINATE_INACTIVE);

    /* 阻塞等待工作线程退出 */
    if (pthread_join(d->thread, &join_ret)) {
        /* pthread_join 失败，标记为无效状态 */
        indeterminate_set(d, INDETERMINATE_INVALID);
    }
}
