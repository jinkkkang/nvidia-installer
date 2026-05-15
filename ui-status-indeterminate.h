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
 * 【文件说明】不确定进度指示器头文件。
 * 用于在无法预估完成时间的操作（如内核模块编译）期间显示动态进度指示。
 * 通过后台线程定期调用 UI 的 update_indeterminate 回调来更新显示。
 */

#ifndef __UI_STATUS_INDETERMINATE_H__
#define __UI_STATUS_INDETERMINATE_H__

/*
 * 不确定进度指示器状态枚举。
 */

typedef enum {
    INDETERMINATE_INVALID,   /* 指示器初始化失败，所有操作应为空操作 */
    INDETERMINATE_INACTIVE,  /* 指示器处于非活跃状态（任务未开始或已完成） */
    INDETERMINATE_ACTIVE,    /* 指示器处于活跃状态（任务正在运行） */
} IndeterminateState;

/* 不透明数据结构，内部包含互斥锁和线程状态（定义在 ui-status-indeterminate.c） */
typedef struct __indeterminate_data IndeterminateData;

/* 初始化不确定进度指示器，创建互斥锁 */
IndeterminateData *indeterminate_init(void);
/* 销毁不确定进度指示器，释放资源 */
void indeterminate_destroy(IndeterminateData *d);
/* 获取当前指示器状态 */
IndeterminateState indeterminate_get(IndeterminateData *d);
/* 启动不确定进度指示（创建后台工作线程 w） */
void indeterminate_begin(IndeterminateData *d, void *(*w)(void *), void *args);
/* 结束不确定进度指示（通知后台线程退出并等待其完成） */
void indeterminate_end(IndeterminateData *d);
#endif
