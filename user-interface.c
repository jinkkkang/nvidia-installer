/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003 NVIDIA Corporation
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
 *
 * user_interface.c - this source file contains an abstraction to the
 * nvidia-installer user interface.
 *
 * 【文件说明】nvidia-installer UI 抽象层实现文件。
 *
 * 本文件是 nvidia-installer 用户界面子系统的核心，提供了一组 ui_* 前缀的
 * 包装函数（wrapper functions），通过全局的 InstallerUI 调度表（__ui）间接
 * 调用具体的 UI 后端实现。
 *
 * 支持的 UI 后端：
 *   1. ncurses UI  -- 以共享库（.so）形式嵌入安装器二进制中，运行时提取到
 *                     临时文件后通过 dlopen() 动态加载。提供交互式终端界面。
 *   2. stream UI   -- 内置的基于 printf/stream 的简单 UI，始终可用，作为
 *                     回退方案（fallback）。
 *
 * UI 初始化流程（ui_init）：
 *   - 依次尝试加载 ncurses6 -> ncurses -> ncursesw6 共享库
 *   - 将嵌入二进制的 .so 数据写入临时文件，再 dlopen() 加载
 *   - 若所有 ncurses 后端均不可用，则回退到内置的 stream UI
 *   - 初始化不确定进度指示器（indeterminate indicator）
 *   - 注册常见信号处理器，确保异常退出时能清理 UI
 *
 * UI 消息机制：
 *   - 在 UI 初始化之前到达的消息会被延迟存储（defer），UI 初始化后再回放
 *   - 静默模式（--silent）下仅显示错误和警告，普通消息被抑制
 *   - 非交互模式（--no-questions）下自动使用默认答案
 *
 * 进度显示：
 *   - 确定性进度：ui_status_begin/update/end，显示百分比进度条
 *   - 不确定性进度：ui_indeterminate_begin/end，用于无法预估完成时间的操作
 */


/* ======================== 标准库头文件 ======================== */
#include <stdio.h>       /* 标准输入输出（printf 等） */
#include <stdlib.h>      /* 标准库函数（exit, free 等） */
#include <sys/types.h>   /* 系统数据类型定义 */
#include <sys/stat.h>    /* 文件状态相关 */
#include <unistd.h>      /* POSIX 系统调用（write, close, unlink, lseek 等） */
#include <sys/mman.h>    /* 内存映射（mmap, munmap），用于提取嵌入的 UI .so 文件 */
#include <fcntl.h>       /* 文件控制（open 等） */
#include <string.h>      /* 字符串操作（memcpy, strcmp, strlen 等） */
#include <dlfcn.h>       /* 动态链接库加载（dlopen, dlsym, dlclose） */
#include <errno.h>       /* 错误码及 strerror() */
#include <signal.h>      /* 信号处理（signal, SIGTERM 等） */

/* ======================== 项目内部头文件 ======================== */
#include "nvidia-installer.h"        /* 项目核心定义（Options 结构体、NV_VSNPRINTF 宏等） */
#include "nvidia-installer-ui.h"     /* InstallerUI 调度表结构体定义及消息级别常量 */
#include "misc.h"                    /* 杂项工具函数（nvstrdup, nvstrcat, nvfree, log_printf 等） */
#include "files.h"                   /* 文件操作工具函数 */
#include "user-interface.h"          /* 本文件对应的头文件，声明所有 ui_* 公共接口 */
#include "ui-status-indeterminate.h" /* 不确定进度指示器接口 */

/*
 * 以下头文件由构建系统自动生成，包含将 ncurses UI 共享库以静态数组形式
 * 嵌入到安装器二进制中的数据。每个头文件定义了：
 *   _binary_nvidia_installer_ncursesX_ui_so_start  -- 数据起始指针
 *   _binary_nvidia_installer_ncursesX_ui_so_end    -- 数据结束指针
 * 这样做是为了将 UI 共享库打包进单一的安装器可执行文件中。
 */
#include "nvidia-installer-ncurses-ui.so.h"
#if defined(NV_INSTALLER_NCURSES6)
#include "nvidia-installer-ncurses6-ui.so.h"
#endif
#if defined(NV_INSTALLER_NCURSESW6)
#include "nvidia-installer-ncursesw6-ui.so.h"
#endif

/*
 * 全局 UI 调度表指针。
 * 指向当前激活的 UI 后端（ncurses 或 stream）的 InstallerUI 实例。
 * 所有 ui_* 包装函数都通过此指针间接调用 UI 操作。
 * 在 ui_init() 中初始化，ui_close() 中置 NULL。
 */

InstallerUI *__ui = NULL;

/*
 * 已提取的 UI 共享库的临时文件路径。
 * UI 共享库从二进制中提取到临时文件后，记录其路径以便在 ui_close() 时删除。
 */

char *__extracted_user_interface_filename = NULL;

/*
 * 引入 stream UI（基于 printf 的简单文本 UI）的调度表。
 * 定义在 stream-ui.c 中，作为所有图形/交互式 UI 都不可用时的最终回退方案。
 * stream UI 始终编译在安装器内部，无需动态加载。
 */

extern InstallerUI stream_ui_dispatch_table;

/*
 * user_interface_attribute_t：描述一个可用 UI 后端的属性结构体。
 *
 * 成员说明：
 *   name            -- UI 后端的短名称标识（如 "ncurses6"），用于命令行 --ui 选项匹配
 *   descr           -- UI 后端的人类可读描述，用于日志输出
 *   filename        -- 提取后的临时 .so 文件路径（由 extract_user_interface 填充）
 *   data_array      -- 指向嵌入在二进制中的 .so 文件数据的起始地址
 *   data_array_size -- 嵌入的 .so 文件数据的字节大小
 */

typedef struct {
    char *name;
    char *descr;
    char *filename;
    const char *data_array;
    const int data_array_size;
} user_interface_attribute_t;

/* 前向声明：将嵌入的 UI 共享库数据提取到临时文件 */
static int extract_user_interface(Options *op, user_interface_attribute_t *ui);
/* 前向声明：信号处理器，在收到致命信号时清理 UI 并退出 */
static void ui_signal_handler(int n);

/*
 * 常用选项定义：用于 ui_multiple_choice() 或 ui_paged_prompt() 的预定义答案。
 * 安装过程中许多对话框都会使用"继续安装/中止安装"这一对选项。
 * 枚举值 CONTINUE_CHOICE 和 ABORT_CHOICE 定义在 user-interface.h 中。
 */

const char * const CONTINUE_ABORT_CHOICES[] = {
    [CONTINUE_CHOICE] = "Continue installation",
    [ABORT_CHOICE]    = "Abort installation"
};

/*
 * level_str() - 根据消息级别返回对应的日志前缀字符串。
 *
 * @param level: 消息级别（NV_MSG_LEVEL_ERROR / NV_MSG_LEVEL_WARNING / 其他）
 * @return: 日志前缀字符串，用于 log_printf() 输出时标识消息类型
 *          - 错误级别返回 "ERROR: "
 *          - 警告级别返回 "WARNING: "
 *          - 其他级别返回 NV_BULLET_STR（通常是类似 " -> " 的项目符号前缀）
 */
static const char *level_str(int level)
{
    switch (level) {
        case NV_MSG_LEVEL_ERROR: return "ERROR: ";
        case NV_MSG_LEVEL_WARNING: return "WARNING: ";
        default: return NV_BULLET_STR;
    }
}

/*
 * do_print_message() - 通过 UI 后端显示消息的内部辅助函数。
 *
 * @param op:    全局选项结构体
 * @param level: 消息级别
 * @param msg:   要显示的消息文本
 *
 * 处理逻辑：
 *   - 错误和警告消息（ERROR / WARNING）始终显示
 *   - 普通消息（MESSAGE）仅在非静默模式下显示
 *   - 静默模式下普通消息被抑制，但错误/警告不受影响
 */
static void do_print_message(Options *op, int level, const char *msg)
{
    /* 打印所有警告/错误；普通消息仅在非静默模式下打印 */
    if (level != NV_MSG_LEVEL_MESSAGE || !op->silent) {
        __ui->message(op, level, msg);
    }
}

/*
 * ui_init() - 初始化用户界面。
 *
 * @param op: 全局选项结构体，包含命令行参数解析结果
 * @return:   成功返回 TRUE，失败返回 FALSE
 *
 * 初始化流程：
 *   1. 构建可用 UI 后端列表（按优先级排列）
 *   2. 如果用户通过 --ui 指定了特定后端，则跳转到该后端
 *   3. 依次尝试提取并加载每个 UI 共享库（extract + dlopen）
 *   4. 找到第一个可用的后端后停止搜索
 *   5. 如果所有后端都不可用，回退到内置的 stream UI
 *   6. 调用选定后端的 init() 函数进行初始化
 *   7. 初始化不确定进度指示器
 *   8. 注册信号处理器
 *   9. 回放 UI 初始化前被延迟的消息
 */

int ui_init(Options *op)
{
    void *handle;  /* dlopen() 返回的共享库句柄 */
    int i;         /* 循环索引 */

    /*
     * 可用 UI 后端列表，按优先级从高到低排列。
     * 每个条目包含：名称、描述、临时文件路径（待填充）、嵌入数据指针和大小。
     * 最后一个 "none" 条目作为哨兵（sentinel），descr 为 NULL 表示列表结束。
     *
     * 注意：GTK+ UI 已被注释掉（可能已废弃或从未实现）。
     */
    user_interface_attribute_t ui_list[] = {
        /* { "nvidia-installer GTK+ user interface", NULL, NULL, 0 }, */
#if defined(NV_INSTALLER_NCURSES6)
        { "ncurses6", "nvidia-installer ncurses v6 user interface", NULL,
          _binary_nvidia_installer_ncurses6_ui_so_start,
          _binary_nvidia_installer_ncurses6_ui_so_end - _binary_nvidia_installer_ncurses6_ui_so_start
        },
#endif
        { "ncurses", "nvidia-installer ncurses user interface", NULL,
          _binary_nvidia_installer_ncurses_ui_so_start,
          _binary_nvidia_installer_ncurses_ui_so_end - _binary_nvidia_installer_ncurses_ui_so_start
        },
#if defined(NV_INSTALLER_NCURSESW6)
        { "ncursesw6", "nvidia-installer ncurses v6 user interface (widechar)",
          NULL,
          _binary_nvidia_installer_ncursesw6_ui_so_start,
          _binary_nvidia_installer_ncursesw6_ui_so_end - _binary_nvidia_installer_ncursesw6_ui_so_start
        },
#endif
        { "none", NULL, NULL, NULL, 0 }
    };

    /* 尝试 dlopen() 加载合适的 UI 共享库 */

    __ui = NULL;

    /*
     * 静默模式下跳过所有交互式 UI 的加载，直接使用 stream UI。
     * 非静默模式下才尝试加载 ncurses 等交互式 UI。
     */
    if (!op->silent) {
        /*
         * 如果用户通过 --ui 选项指定了 UI 后端名称，
         * 在列表中查找匹配项，从该位置开始尝试。
         */
        if (op->ui.name) {
            for (i = 0; i < ARRAY_LEN(ui_list); i++) {
                if (strcmp(op->ui.name, ui_list[i].name) == 0) {
                    break;
                }
            }

            /* 如果指定的 UI 名称无效（未找到），记录日志并从头开始尝试 */
            if (i == ARRAY_LEN(ui_list)) {
                log_printf(op, NULL, "Invalid \"ui\" option: %s", op->ui.name);
                i = 0;
            }
        } else {
            /* 未指定 --ui 选项，从列表第一个开始依次尝试 */
            i = 0;
        }

        /*
         * 遍历 UI 列表，依次尝试加载每个后端。
         * 循环条件：
         *   - i < ARRAY_LEN(ui_list)    -- 未越界
         *   - ui_list[i].descr          -- 未到达哨兵条目（descr 为 NULL 即结束）
         *   - !__ui                     -- 尚未成功加载任何 UI
         */
        for (; i < ARRAY_LEN(ui_list) && ui_list[i].descr && !__ui; i++) {

            /* 步骤 1：将嵌入的 .so 数据提取到临时文件 */
            if (!extract_user_interface(op, &ui_list[i])) continue;

            /* 步骤 2：dlopen() 加载提取出的共享库，RTLD_NOW 表示立即解析所有符号 */
            handle = dlopen(ui_list[i].filename, RTLD_NOW);

            if (handle) {
                /*
                 * 步骤 3：从共享库中查找 "ui_dispatch_table" 符号，
                 * 这是每个 UI 后端导出的 InstallerUI 结构体实例。
                 */
                __ui = dlsym(handle, "ui_dispatch_table");
                /*
                 * 步骤 4：调用 detect() 检测 UI 后端是否可用。
                 * 例如 ncurses 需要检测终端是否支持。
                 */
                if (__ui && __ui->detect(op)) {
                    log_printf(op, NULL, "Using: %s", ui_list[i].descr);
                    __extracted_user_interface_filename = ui_list[i].filename;
                    break;  /* 成功，跳出循环 */
                } else {
                    /* detect() 失败，关闭共享库并继续尝试下一个 */
                    log_printf(op, NULL, "Unable to initialize: %s",
                               ui_list[i].descr);
                    dlclose(handle);
                    __ui = NULL;
                }
            } else {
                /* dlopen() 失败（如缺少 ncurses 库依赖），记录日志并继续 */
                log_printf(op, NULL, "Unable to load: %s", ui_list[i].descr);
                log_printf(op, NULL, "");
            }
        }
    }

    /* 所有交互式 UI 后端都不可用，回退到始终内置的 stream UI */

    if (!__ui) {
        __ui = &stream_ui_dispatch_table;
        log_printf(op, NULL, "Using built-in stream user interface");
    }

    /*
     * 调用选定 UI 后端的 init() 函数进行初始化。
     * 传入 nv_format_text_rows 回调函数，用于文本自动换行排版。
     *
     * XXX（待改进）：如果 init() 失败，应尝试回退到内置的 stream UI，
     * 但目前未实现此逻辑。
     */

    if (!__ui->init(op, nv_format_text_rows)) return FALSE;

    /* 初始化不确定进度指示器（用于无法预估完成时间的操作） */
    op->ui.indeterminate_data = indeterminate_init();

    /*
     * 注册常见信号的处理器。
     * 当进程收到这些致命信号时，ui_signal_handler() 会先清理 UI 状态
     * （如恢复终端设置），再安全退出。否则终端可能留在异常状态。
     */

    signal(SIGHUP,  ui_signal_handler);  /* 终端挂断 */
    signal(SIGALRM, ui_signal_handler);  /* 定时器到期 */
    signal(SIGABRT, ui_signal_handler);  /* 程序中止（abort） */
    signal(SIGSEGV, ui_signal_handler);  /* 段错误 */
    signal(SIGTERM, ui_signal_handler);  /* 终止信号 */
    signal(SIGINT,  ui_signal_handler);  /* 中断信号（Ctrl+C） */
    signal(SIGILL,  ui_signal_handler);  /* 非法指令 */
    signal(SIGBUS,  ui_signal_handler);  /* 总线错误 */

    /*
     * 回放在 UI 初始化之前被延迟存储的消息。
     *
     * 在 ui_init() 被调用之前，如果代码路径已经调用了 ui_error/ui_warn/ui_message，
     * 这些消息会被暂存在 op->ui_deferred_messages 数组中（参见 defer_message()）。
     * 现在 UI 已初始化，将它们全部显示出来。
     */
    for (i = 0; i < op->num_ui_deferred_messages; i++) {
        /*
         * 延迟消息在 defer_message() 中已经通过 nv_error_msg/nv_warning_msg/nv_info_msg
         * 输出到了 stdout/stderr。如果当前使用的是 stream UI（同样输出到 stdout/stderr），
         * 则不再重复显示，避免消息出现两次。
         */
        if (__ui != &stream_ui_dispatch_table) {
            do_print_message(op, op->ui_deferred_messages[i].level,
                             op->ui_deferred_messages[i].message);
        }

        /*
         * 延迟消息产生时日志系统尚未初始化，现在将它们写入日志文件。
         */
        log_printf(op, level_str(op->ui_deferred_messages[i].level), "%s",
                   op->ui_deferred_messages[i].message);

        nvfree(op->ui_deferred_messages[i].message);
    }

    /* 清理延迟消息数组 */
    nvfree(op->ui_deferred_messages);
    op->ui_deferred_messages = NULL;
    op->num_ui_deferred_messages = 0;

    /* 初始化成功 */

    return TRUE;

} /* init_ui () */



/*
 * ui_set_title() - 设置 UI 界面的标题。
 *
 * @param op:  全局选项结构体
 * @param fmt: printf 风格的格式化字符串
 * @param ...: 格式化参数
 *
 * 在 ncurses UI 中，标题通常显示在窗口顶部；在 stream UI 中可能无效果。
 * 静默模式下直接返回，不设置标题。
 */

void ui_set_title(Options *op, const char *fmt, ...)
{
    char *title;

    if (op->silent) return;

    /* NV_VSNPRINTF：项目自定义的宏，执行 va_start + vsnprintf + va_end，分配并填充 title */
    NV_VSNPRINTF(title, fmt);

    __ui->set_title(op, title);
    free(title);

} /* ui_set_title() */



/*
 * ui_get_input() - 向用户显示提示信息并获取一行文本输入。
 *
 * @param op:  全局选项结构体
 * @param def: 默认值字符串，当用户直接回车或处于非交互模式时使用
 * @param fmt: printf 风格的格式化提示信息
 * @param ...: 格式化参数
 * @return:    用户输入的字符串（调用者负责释放），非交互模式下返回默认值的副本
 *
 * 处理逻辑：
 *   - 非交互模式（--no-questions）：直接使用默认值，不询问用户
 *   - 交互模式：调用 UI 后端的 get_input() 获取用户输入
 *   - 无论哪种模式，都将问题和答案记录到日志
 */

char *ui_get_input(Options *op, const char *def, const char *fmt, ...)
{
    char *msg, *tmp = NULL, *ret;

    NV_VSNPRINTF(msg, fmt);

    if (op->no_questions) {
        /* 非交互模式：复制默认值作为答案（def 为 NULL 时使用空字符串） */
        ret = nvstrdup(def ? def : "");
        tmp = nvstrcat(msg, " (Answer: '", ret, "')", NULL);
        if (!op->silent) {
            __ui->message(op, NV_MSG_LEVEL_LOG, tmp);
        }
    } else {
        /* 交互模式：调用 UI 后端获取用户输入 */
        ret = __ui->get_input(op, def, msg);
        tmp = nvstrcat(msg, " (Answer: '", ret, "')", NULL);
    }
    /* 将问题和答案写入日志文件 */
    log_printf(op, NV_BULLET_STR, "%s", tmp);
    nvfree(msg);
    nvfree(tmp);

    return ret;

} /* ui_get_input() */

/*
 * defer_message() - 在 UI 尚未初始化时延迟存储消息。
 *
 * @param op:      全局选项结构体
 * @param level:   消息级别（ERROR / WARNING / MESSAGE）
 * @param message: 消息文本
 *
 * 当 ui_init() 尚未被调用时（__ui 为 NULL），ui_error/ui_warn/ui_message
 * 无法通过 UI 后端显示消息。此函数将消息暂存到 op->ui_deferred_messages
 * 数组中，等 ui_init() 完成后再回放显示。
 *
 * 同时，为了确保即使 ui_init() 从未被调用（如启动早期就出错退出），
 * 消息也能被用户看到，这里会立即通过 nv_error_msg / nv_warning_msg /
 * nv_info_msg 输出到 stdout/stderr。
 */
static void defer_message(Options *op, int level, const char *message)
{
    int i = op->num_ui_deferred_messages;

    /* 扩展延迟消息数组（动态增长，每次增加一个元素） */
    op->num_ui_deferred_messages++;

    op->ui_deferred_messages = nvrealloc(op->ui_deferred_messages,
                                         op->num_ui_deferred_messages *
                                         sizeof(op->ui_deferred_messages[0]));

    /* 保存消息的级别和内容副本 */
    op->ui_deferred_messages[i].level = level;
    op->ui_deferred_messages[i].message = nvstrdup(message);

    /*
     * 立即将消息输出到 stdout/stderr，确保即使 ui_init() 永远不会被调用，
     * 用户也能看到错误/警告/信息。
     */
    switch (level) {
        case NV_MSG_LEVEL_ERROR:
            nv_error_msg("%s", message);
            break;
        case NV_MSG_LEVEL_WARNING:
            nv_warning_msg("%s", message);
            break;
        default:
            if (!op->silent) {
                nv_info_msg(NULL, "%s", message);
            }
            break;
    }
}

/*
 * message_helper() - 消息显示的核心辅助函数。
 *
 * @param op:    全局选项结构体
 * @param level: 消息级别
 * @param msg:   消息文本
 *
 * 所有 ui_error / ui_warn / ui_message 最终都汇聚到此函数。
 * 根据 UI 是否已初始化决定处理方式：
 *   - __ui 不为 NULL（已初始化）：通过 UI 后端显示并写入日志
 *   - __ui 为 NULL（未初始化）：延迟存储，等待 ui_init() 后回放
 */
static void message_helper(Options *op, int level, const char *msg)
{
    if (__ui) {
        do_print_message(op, level, msg);
        log_printf(op, level_str(level), "%s", msg);
    } else {
        defer_message(op, level, msg);
    }
}


/*
 * ui_error() / ui_warn() / ui_message() - 三个消息显示函数。
 *
 * 功能相同，区别仅在于消息级别：
 *   ui_error   -- NV_MSG_LEVEL_ERROR   错误级别，UI 后端会以醒目方式显示
 *   ui_warn    -- NV_MSG_LEVEL_WARNING 警告级别
 *   ui_message -- NV_MSG_LEVEL_MESSAGE 普通信息级别，静默模式下不显示
 *
 * 参数：
 *   @param op:  全局选项结构体
 *   @param fmt: printf 风格的格式化字符串
 *   @param ...: 格式化参数
 *
 * 所有函数通过 NV_VSNPRINTF 宏构造格式化消息字符串，
 * 然后委托给 message_helper() 处理显示和日志记录。
 */

void ui_error(Options *op, const char *fmt, ...)
{
    char *msg;

    NV_VSNPRINTF(msg, fmt);
    message_helper(op, NV_MSG_LEVEL_ERROR, msg);
    free(msg);
}

void ui_warn(Options *op, const char *fmt, ...)
{
    char *msg;

    NV_VSNPRINTF(msg, fmt);
    message_helper(op, NV_MSG_LEVEL_WARNING, msg);
    free(msg);
}

void ui_message(Options *op, const char *fmt, ...)
{
    char *msg;

    NV_VSNPRINTF(msg, fmt);
    message_helper(op, NV_MSG_LEVEL_MESSAGE, msg);
    free(msg);
}


/*
 * ui_log() - 显示日志级别消息。
 *
 * @param op:  全局选项结构体
 * @param fmt: printf 风格的格式化字符串
 * @param ...: 格式化参数
 *
 * 与 ui_message() 的区别：
 *   - 使用 NV_MSG_LEVEL_LOG 级别，消息通常只出现在 ncurses UI 的日志区域，
 *     不会弹出对话框要求用户确认
 *   - 不经过 message_helper()，不支持延迟机制（要求 __ui 已初始化）
 *   - 静默模式下不通过 UI 显示，但仍写入日志文件
 *
 * 注意：原注释末尾标注为 ui_message()，疑为复制粘贴遗留，实际是 ui_log()。
 */
void ui_log(Options *op, const char *fmt, ...)
{
    char *msg;

    NV_VSNPRINTF(msg, fmt);

    if (__ui && !op->silent) __ui->message(op, NV_MSG_LEVEL_LOG, msg);
    log_printf(op, NV_BULLET_STR, "%s", msg);

    free(msg);

} /* ui_message() */


/*
 * ui_expert() - 仅在专家模式下显示的日志消息。
 *
 * @param op:  全局选项结构体
 * @param fmt: printf 风格的格式化字符串
 * @param ...: 格式化参数
 *
 * 与 ui_log() 基本相同，但增加了专家模式检查：
 *   - 非专家模式（--expert 未指定）下直接返回，不做任何操作
 *   - 专家模式下，通过 UI 后端以 LOG 级别显示消息，并写入日志
 *   - 用于输出安装过程中的详细技术信息，普通用户通常不需要看到
 */

void ui_expert(Options *op, const char *fmt, ...)
{
    char *msg;

    /* 非专家模式下直接返回 */
    if (!op->expert) return;

    NV_VSNPRINTF(msg, fmt);

    if (!op->silent) __ui->message(op, NV_MSG_LEVEL_LOG, msg);
    log_printf(op, NV_BULLET_STR, "%s", msg);

    free (msg);

} /* ui_expert() */



/*
 * ui_command_output() - 显示外部命令的输出文本。
 *
 * @param op:  全局选项结构体
 * @param fmt: printf 风格的格式化字符串（通常是命令的一行输出）
 * @param ...: 格式化参数
 *
 * 用于显示安装过程中执行的外部命令（如 make、gcc 等）的标准输出/错误。
 * 在 ncurses UI 中，这些输出通常显示在专门的命令输出区域。
 * 日志中使用 NV_CMD_OUT_PREFIX 前缀标识，以区分安装器自身消息和命令输出。
 * 静默模式下不通过 UI 显示，但仍写入日志。
 */
void ui_command_output(Options *op, const char *fmt, ...)
{
    char *msg;

    NV_VSNPRINTF(msg, fmt);

    if (!op->silent) __ui->command_output(op, msg);

    log_printf(op, NV_CMD_OUT_PREFIX, "%s", msg);

    free(msg);

} /* ui_command_output() */



/*
 * ui_approve_command_list() - 在专家模式下向用户展示即将执行的命令列表，
 *                             并请求批准。
 *
 * @param op:  全局选项结构体
 * @param c:   要执行的命令列表结构体（CommandList）
 * @param fmt: printf 风格的描述信息
 * @param ...: 格式化参数
 * @return:    TRUE 表示用户批准执行，FALSE 表示用户拒绝
 *
 * 处理逻辑：
 *   - 非专家模式或非交互模式下直接返回 TRUE（自动批准）
 *   - 专家模式 + 交互模式下，通过 UI 后端展示命令列表并等待用户确认
 *   - 将批准/拒绝结果记录到日志
 *
 * 这是安装器的安全特性之一，允许高级用户在安装前审查所有将要执行的操作。
 */

int ui_approve_command_list(Options *op, CommandList *c, const char *fmt, ...)
{
    char *msg;
    int ret;

    /* 非专家模式或非交互模式下自动批准 */
    if (!op->expert || op->no_questions) return TRUE;

    NV_VSNPRINTF(msg, fmt);

    ret = __ui->approve_command_list(op, c, msg);
    free(msg);

    /* 记录用户的选择 */
    if (ret) __ui->message(op, NV_MSG_LEVEL_LOG, "Commandlist approved.");
    else __ui->message(op, NV_MSG_LEVEL_LOG, "Commandlist rejected.");

    return ret;

} /* ui_approve_command_list() */


/*
 * ui_yes_no() - 向用户提问一个 Yes/No 问题。
 *
 * @param op:  全局选项结构体
 * @param def: 默认答案（TRUE = Yes, FALSE = No），非交互模式下直接使用
 * @param fmt: printf 风格的问题文本
 * @param ...: 格式化参数
 * @return:    用户的选择（TRUE = Yes, FALSE = No）
 *
 * 处理逻辑：
 *   - 非交互模式（--no-questions）：自动使用默认答案 def
 *   - 交互模式：通过 UI 后端显示对话框等待用户选择
 *   - 无论哪种模式，都将问题和答案记录到日志
 */

int ui_yes_no (Options *op, const int def, const char *fmt, ...)
{
    char *msg, *tmp = NULL;
    int ret;

    NV_VSNPRINTF(msg, fmt);

    if (op->no_questions) {
        /* 非交互模式：使用默认答案 */
        ret = def;
        tmp = nvstrcat(msg, " (Answer: ", (ret ? "Yes" : "No"), ")", NULL);
        if (!op->silent) {
            __ui->message(op, NV_MSG_LEVEL_LOG, tmp);
        }
    } else {
        /* 交互模式：调用 UI 后端询问用户 */
        ret = __ui->yes_no(op, def, msg);
        tmp = nvstrcat(msg, " (Answer: ", (ret ? "Yes" : "No"), ")", NULL);
    }

    /* 将问题和答案写入日志 */
    log_printf(op, NV_BULLET_STR, "%s", tmp);
    nvfree(msg);
    nvfree(tmp);

    return ret;

} /* ui_yes_no() */


/*
 * ui_multiple_choice() - 向用户展示多选题并获取选择。
 *
 * @param op:             全局选项结构体
 * @param answers:        选项字符串数组（如 {"Continue", "Abort"}）
 * @param num_answers:    选项数量
 * @param default_answer: 默认选项索引
 * @param fmt:            printf 风格的问题文本
 * @param ...:            格式化参数
 * @return:               用户选择的选项索引（从 0 开始）
 *
 * 处理逻辑：
 *   - 非交互模式：直接返回默认选项索引
 *   - 交互模式：通过 UI 后端显示多选对话框
 *   - 将问题和选择记录到日志
 */

int ui_multiple_choice (Options *op, const char * const *answers,
                        int num_answers, int default_answer,
                        const char *fmt, ...)
{
    char *question, *tmp = NULL;
    int ret;

    NV_VSNPRINTF(question, fmt);

    if (op->no_questions) {
        ret = default_answer;
    } else {
        ret = __ui->multiple_choice(op, question, answers, num_answers,
                                    default_answer);
    }

    /* 拼接问题和所选答案，用于 UI 日志显示和日志文件记录 */
    tmp = nvstrcat(question, " (Answer: ", answers[ret], ")", NULL);

    if (!op->silent) {
        __ui->message(op, NV_MSG_LEVEL_LOG, tmp);
    }

    log_printf(op, NV_BULLET_STR, "%s", tmp);
    nvfree(question);
    nvfree(tmp);

    return ret;

} /* ui_multiple_choice() */


/*
 * ui_paged_prompt() - 带可滚动长文本的多选题对话框。
 *
 * @param op:             全局选项结构体
 * @param question:       问题文本（显示在文本框上方或下方）
 * @param pager_title:    可滚动文本区域的标题
 * @param pager_text:     可滚动的长文本内容（如许可证协议全文）
 * @param answers:        选项字符串数组
 * @param num_answers:    选项数量
 * @param default_answer: 默认选项索引
 * @return:               用户选择的选项索引
 *
 * 典型使用场景：显示 NVIDIA 驱动许可证协议全文，让用户阅读后选择
 * "接受"或"拒绝"。与 ui_multiple_choice() 类似，但额外提供了
 * 可滚动的长文本显示区域。
 */

int ui_paged_prompt (Options *op, const char *question, const char *pager_title,
                     const char *pager_text, const char * const *answers,
                     int num_answers, int default_answer)
{
    char *tmp;
    int ret;

    if (op->no_questions) {
        ret = default_answer;
    } else {
        ret = __ui->paged_prompt(op, question, pager_title, pager_text, answers,
                                 num_answers, default_answer);
    }

    /* 将完整的问题、长文本和答案拼接后写入日志 */
    tmp = nvstrcat(question, "\n\n", pager_text,
                   "\n(Answer: ", answers[ret], ")", NULL);

    if (!op->silent) {
        __ui->message(op, NV_MSG_LEVEL_LOG, tmp);
    }

    log_printf(op, NV_BULLET_STR, "%s", tmp);
    nvfree(tmp);

    return ret;
}


/*
 * ui_status_begin() - 创建并立即显示一个新的进度指示器。
 *
 * @param op:    全局选项结构体
 * @param title: 进度指示器的标题（在整个进度周期内保持显示）
 * @param fmt:   printf 风格的初始状态消息（可选，可传 NULL）
 *               此消息可能在进度更新过程中被替换
 * @param ...:   格式化参数
 *
 * 配合 ui_status_update() 和 ui_status_end() 构成完整的进度显示流程：
 *   1. ui_status_begin()  -- 开始进度（设置标题和初始消息）
 *   2. ui_status_update() -- 反复调用更新进度百分比（0.0 ~ 1.0）
 *   3. ui_status_end()    -- 结束进度并显示完成消息
 *
 * 注意：op->ui.status_active 标志用于跟踪进度是否处于活跃状态。
 */

void ui_status_begin(Options *op, const char *title, const char *fmt, ...)
{
    char *msg;

    /* 标题始终写入日志，无论是否静默模式 */
    log_printf(op, NV_BULLET_STR, "%s", title);

    if (op->silent) return;

    NV_VSNPRINTF(msg, fmt);

    /* 标记进度为活跃状态 */
    op->ui.status_active = TRUE;

    __ui->status_begin(op, title, msg);
    free(msg);
}

/*
 * ui_status_update() - 更新进度指示器的位置和消息。
 *
 * @param op:      全局选项结构体
 * @param percent: 完成度百分比，范围 0.0 到 1.0（如 0.5 表示 50%）
 * @param fmt:     printf 风格的状态消息（替换之前显示的消息）
 * @param ...:     格式化参数
 *
 * 注意：
 *   - 某些 UI 实现（如 stream UI）可能只显示 ui_status_begin() 中的初始消息，
 *     忽略后续更新的消息文本（但仍会更新百分比）
 *   - 静默模式下直接返回，不更新显示
 *   - 此函数不写入日志，因为进度更新通常非常频繁
 */

void ui_status_update(Options *op, const float percent, const char *fmt, ...)
{
    char *msg;

    if (op->silent) return;

    NV_VSNPRINTF(msg, fmt);

    __ui->status_update(op, percent, msg);
    free(msg);
}

/*
 * indeterminate_args：不确定进度工作线程的参数结构体。
 * 用于将 Options 和消息文本传递给后台线程。
 */
struct indeterminate_args {
    Options *op;   /* 全局选项结构体 */
    char *msg;     /* 要显示的状态消息 */
};

/*
 * indeterminate_worker() - 不确定进度指示器的后台工作线程函数。
 *
 * @param p:  指向 indeterminate_args 结构体的 void 指针
 * @return:   始终返回 NULL（pthread 要求的返回值）
 *
 * 此函数在独立线程中运行，持续循环调用 UI 后端的 update_indeterminate()
 * 回调来刷新动画效果（如旋转的进度符号），直到主线程调用
 * ui_indeterminate_end() 将状态设置为非活跃。
 *
 * 注意：消息文本被复制一份（nvstrdup），因为调用者的缓冲区可能随时被释放。
 */
static void *indeterminate_worker(void *p)
{
    struct indeterminate_args *args = p;
    Options *op = args->op;
    char *msg = nvstrdup(args->msg);       /* 复制消息文本，确保线程安全 */
    IndeterminateData *id = op->ui.indeterminate_data;

    /* 持续循环更新动画，直到状态变为非活跃（由 ui_indeterminate_end 触发） */
    while (indeterminate_get(id) == INDETERMINATE_ACTIVE) {
        __ui->update_indeterminate(op, msg);
    }

    nvfree(msg);
    return NULL;
}


/*
 * ui_indeterminate_begin() - 开始显示不确定进度指示器。
 *
 * @param op:  全局选项结构体
 * @param fmt: printf 风格的状态消息
 * @param ...: 格式化参数
 *
 * 用于无法预估完成时间的操作（如编译内核模块、下载文件等）。
 * 显示一个动画效果（如旋转的字符），直到调用 ui_indeterminate_end() 结束。
 *
 * 实现方式：
 *   - 创建一个后台线程（indeterminate_worker）循环调用 UI 的动画更新回调
 *   - args 使用 static 变量是因为后台线程需要在此函数返回后仍能访问参数
 *     （注意：这意味着不能同时运行多个不确定进度指示器）
 *   - 静默模式或 fmt 为 NULL 时不启动
 */

void ui_indeterminate_begin(Options *op, const char *fmt, ...)
{
    IndeterminateData *id = op->ui.indeterminate_data;
    /*
     * 使用 static 变量存储线程参数，因为线程生命周期超过本函数栈帧。
     * 这也意味着同一时刻只能有一个活跃的不确定进度指示器。
     */
    static struct indeterminate_args args;
    char *msg;

    if (!op->silent && fmt != NULL) {
        NV_VSNPRINTF(msg, fmt);
        args.op = op;
        args.msg = msg;

        /* 启动后台线程运行 indeterminate_worker */
        indeterminate_begin(id, indeterminate_worker, &args);
    }
}

/*
 * ui_indeterminate_end() - 终止不确定进度指示器。
 *
 * @param op: 全局选项结构体
 *
 * 通知后台工作线程停止循环，并等待线程结束（pthread_join）。
 * 必须与 ui_indeterminate_begin() 配对使用。
 */

void ui_indeterminate_end(Options *op)
{
    indeterminate_end(op->ui.indeterminate_data);
}

/*
 * ui_status_end() - 结束由 ui_status_begin() 创建的进度指示器。
 *
 * @param op:  全局选项结构体
 * @param fmt: printf 风格的完成消息（如 "Installation complete"）
 * @param ...: 格式化参数
 *
 * 显示完成消息并将进度标记为非活跃状态。
 * 与 ui_status_begin() 必须配对使用。
 */
void ui_status_end(Options *op, const char *fmt, ...)
{
    char *msg;

    NV_VSNPRINTF(msg, fmt);

    if (!op->silent) __ui->status_end(op, msg);
    log_printf(op, NV_BULLET_STR, "%s", msg);
    free(msg);

    /* 标记进度为非活跃状态 */
    op->ui.status_active = FALSE;
}



/*
 * ui_close() - 关闭并清理用户界面。
 *
 * @param op: 全局选项结构体（可以为 NULL，从信号处理器调用时即是如此）
 *
 * 清理流程：
 *   1. 调用 UI 后端的 close() 函数（如恢复终端设置）
 *   2. 删除从二进制中提取出的 UI 共享库临时文件
 *   3. 将全局 __ui 指针置 NULL，防止后续误用
 *   4. 如果 op 不为 NULL，销毁不确定进度指示器资源（互斥锁等）
 *
 * 注意：此函数可能从信号处理器中被调用（此时 op 为 NULL），
 * 因此对 op 的访问必须检查 NULL。
 */
void ui_close (Options *op)
{
    /* 调用 UI 后端的清理函数（如 ncurses 的 endwin()） */
    if (__ui) __ui->close(op);

    /* 删除提取的 UI 共享库临时文件 */
    if (__extracted_user_interface_filename) {
        unlink(__extracted_user_interface_filename);
    }

    /* 清空全局 UI 指针 */
    __ui = NULL;

    if (op) {
        /* 信号处理器调用时 op 为 NULL，此时跳过以下清理 */
        indeterminate_destroy(op->ui.indeterminate_data);
        op->ui.indeterminate_data = NULL;
    }
} /* ui_close() */



/*
 * extract_user_interface() - 将嵌入在安装器二进制中的 UI 共享库数据
 *                            提取到临时文件，以便后续通过 dlopen() 加载。
 *
 * @param op: 全局选项结构体
 * @param ui: UI 后端属性结构体，包含嵌入数据的指针和大小
 * @return:   成功返回 TRUE，失败返回 FALSE
 *
 * 设计背景：
 *   UI 共享库（如 ncurses-ui.so）被编译为独立的 .so 文件，然后在构建时
 *   通过 objcopy 或类似工具将其二进制内容转换为 C 数组，嵌入到安装器
 *   的可执行文件中。这样做有两个好处：
 *     1. 安装器是单一文件，无需额外安装 UI 库
 *     2. UI 库的依赖（如 libncurses）不会在链接时影响安装器主体
 *
 *   运行时，此函数将嵌入的数据写入临时文件，再由 dlopen() 加载。
 *   如果目标系统缺少 ncurses 运行时库，dlopen() 会失败，安装器
 *   可以优雅地回退到 stream UI。
 *
 * 提取流程：
 *   1. 检查嵌入数据是否存在
 *   2. 创建临时文件（mkstemp）
 *   3. 通过 lseek + write 设置文件大小
 *   4. mmap 临时文件
 *   5. memcpy 将嵌入数据复制到 mmap 区域（即写入文件）
 *   6. munmap + close 完成
 */

static int extract_user_interface(Options *op, user_interface_attribute_t *ui)
{
    unsigned char *dst = (void *) -1;  /* mmap 返回的映射地址，初始化为无效值 */
    int fd = -1;                       /* 临时文件描述符 */

    /* 检查此 UI 后端的数据是否存在于二进制中 */

    if ((ui->data_array == NULL) || (ui->data_array_size == 0)) {
        log_printf(op, NULL, "%s: not present.", ui->descr);
        return FALSE;
    }

    /* 在临时目录下创建临时文件（XXXXXX 会被 mkstemp 替换为随机字符） */

    ui->filename = nvstrcat(op->tmpdir, "/nv-XXXXXX", NULL);

    fd = mkstemp(ui->filename);
    if (fd == -1) {
        log_printf(op, NULL, "unable to create temporary file (%s)",
                   strerror(errno));
        goto failed;
    }

    /*
     * 设置临时文件的大小。
     * 通过 lseek 到文件末尾位置 - 1，然后写入一个字节，
     * 使文件系统为文件分配所需大小的空间。
     * 这是一种常用的创建稀疏文件或预分配文件大小的技巧。
     */

    if (lseek(fd, ui->data_array_size - 1, SEEK_SET) == -1) {
        log_printf(op, NULL, "Unable to set file size for '%s' (%s)",
                   ui->filename, strerror(errno));
        goto failed;
    }
    if (write(fd, "", 1) != 1) {
        log_printf(op, NULL, "Unable to write file size for '%s' (%s)",
                   ui->filename, strerror(errno));
        goto failed;
    }

    /*
     * 将临时文件映射到内存中。
     * MAP_SHARED 表示对映射区域的写入会反映到文件中。
     * PROT_READ | PROT_WRITE 表示映射区域可读可写。
     */

    if ((dst = mmap(0, ui->data_array_size, PROT_READ | PROT_WRITE,
                    MAP_FILE | MAP_SHARED, fd, 0)) == (void *) -1) {
        log_printf(op, NULL, "Unable to map destination file '%s' "
                   "for copying (%s)", ui->filename, strerror(errno));
        goto failed;
    }

    /* 将嵌入在二进制中的 .so 数据复制到映射区域（即写入临时文件） */

    memcpy(dst, ui->data_array, ui->data_array_size);

    /* 解除内存映射，此时数据已写入文件 */

    if (munmap(dst, ui->data_array_size) == -1) {
        log_printf(op, NULL, "Unable to unmap destination file '%s' "
                   "(%s)", ui->filename, strerror(errno));
        goto failed;
    }

    /* 关闭文件描述符，提取完成 */

    close(fd);

    return TRUE;

 failed:
    /* 错误清理：解除映射、关闭并删除临时文件、释放文件名 */
    if (dst != (void *) -1) munmap(dst, ui->data_array_size);
    if (fd != -1) { close(fd); unlink(ui->filename); }
    free(ui->filename);

    return FALSE;

} /* extract_user_interface() */


/*
 * ui_signal_handler() - 致命信号处理器。
 *
 * @param n: 信号编号（如 SIGTERM = 15, SIGSEGV = 11 等）
 *
 * 当进程收到致命信号（如段错误、中断、终止等）时，此函数被调用。
 * 处理流程：
 *   1. 调用 ui_close(NULL) 清理 UI 状态（如恢复终端 ncurses 设置）
 *   2. 向 stderr 输出信号名称信息
 *   3. 以 128 + 信号编号 作为退出码退出（Unix 惯例）
 *
 * 注意事项：
 *   - 此函数运行在信号处理上下文中，不能使用非异步信号安全函数
 *   - 使用 write(2) 而非 fprintf(3) 输出消息，因为 fprintf 不保证可重入
 *   - 传递 NULL 给 ui_close() 是因为信号处理器中无法获取 Options 结构体
 *     （这是一个已知的局限性，标记为 XXX）
 */

static void ui_signal_handler(int n)
{
    /*
     * 信号编号到名称的映射表。
     * 索引对应 Linux 上的标准信号编号（0-31）。
     * 注意：信号编号 31 同时映射了 SIGSYS 和 SIGUNUSED（历史原因）。
     */
    const char *sig_names[] = {
        "UNKNOWN",   /* 0 */
        "SIGHUP",    /* 1  - 终端挂断 */
        "SIGINT",    /* 2  - 中断（Ctrl+C） */
        "SIGQUIT",   /* 3  - 退出（Ctrl+\） */
        "SIGILL",    /* 4  - 非法指令 */
        "SIGTRAP",   /* 5  - 调试陷阱 */
        "SIGABRT",   /* 6  - 程序中止 */
        "SIGBUS",    /* 7  - 总线错误 */
        "SIGFPE",    /* 8  - 浮点异常 */
        "SIGKILL",   /* 9  - 强制终止（不可捕获） */
        "SIGUSR1",   /* 10 - 用户自定义信号 1 */
        "SIGSEGV",   /* 11 - 段错误（非法内存访问） */
        "SIGUSR2",   /* 12 - 用户自定义信号 2 */
        "SIGPIPE",   /* 13 - 管道破裂 */
        "SIGALRM",   /* 14 - 定时器到期 */
        "SIGTERM",   /* 15 - 终止信号 */
        "SIGSTKFLT", /* 16 - 协处理器栈错误（已废弃） */
        "SIGCHLD",   /* 17 - 子进程状态改变 */
        "SIGCONT",   /* 18 - 继续执行 */
        "SIGSTOP",   /* 19 - 暂停执行（不可捕获） */
        "SIGTSTP",   /* 20 - 终端暂停（Ctrl+Z） */
        "SIGTTIN",   /* 21 - 后台进程读终端 */
        "SIGTTOU",   /* 22 - 后台进程写终端 */
        "SIGURG",    /* 23 - 紧急套接字数据 */
        "SIGXCPU",   /* 24 - CPU 时间限制超出 */
        "SIGXFSZ",   /* 25 - 文件大小限制超出 */
        "SIGVTALRM", /* 26 - 虚拟定时器到期 */
        "SIGPROF",   /* 27 - 性能分析定时器到期 */
        "SIGWINCH",  /* 28 - 终端窗口大小改变 */
        "SIGIO",     /* 29 - I/O 可用 */
        "SIGPWR",    /* 30 - 电源故障 */
        "SIGSYS",    /* 31 - 非法系统调用 */
        "SIGUNUSED", /* 31 - 未使用信号（与 SIGSYS 同编号） */
    };

    const char *s;

    /*
     * 清理 UI 状态。传入 NULL 是因为在信号处理上下文中
     * 无法访问 Options 结构体（这是已知的设计局限，标记为 XXX）。
     */
    ui_close(NULL);

    /*
     * 使用系统调用 write(2) 直接向 stderr（文件描述符 2）写入消息，
     * 而非使用 fprintf(3)，因为 fprintf 涉及缓冲区管理，
     * 在信号处理器中不保证可重入（async-signal-safe）。
     */

    s = (n < 32) ? sig_names[n] : "UNKNOWN";

    write(2, "Received signal ", 16);
    write(2, s, strlen(s));
    write(2, "; aborting.\n", 12);

    /*
     * 以 128 + 信号编号退出。
     * 这是 Unix/Linux 的惯例：当进程因信号终止时，
     * 退出码 = 128 + 信号编号，便于调用者判断退出原因。
     */
    exit(128 + n);

} /* ui_signal_handler() */
