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
 * 【文件说明】UI 抽象层接口定义头文件。
 * 定义了 InstallerUI 调度表结构，这是安装器 UI 子系统的核心抽象。
 * 每种 UI 实现（ncurses、stream/printf）都必须提供一个 InstallerUI 实例。
 * 安装器通过函数指针调用 UI 操作，实现 UI 后端的可替换。
 *
 * nv_installer_ui.h
 */

#ifndef __NVIDIA_INSTALLER_UI_H__
#define __NVIDIA_INSTALLER_UI_H__

#include "nvidia-installer.h"
#include "command-list.h"

/* 消息级别常量 */
#define NV_MSG_LEVEL_LOG     0  /* 仅写入日志，不显示给用户 */
#define NV_MSG_LEVEL_MESSAGE 1  /* 普通信息，显示给用户 */
#define NV_MSG_LEVEL_WARNING 2  /* 警告信息，以醒目方式显示 */
#define NV_MSG_LEVEL_ERROR   3  /* 错误信息，以醒目方式显示 */

/*
 * InstallerUI：UI 调度表结构。
 * 每种 UI 后端（ncurses-ui.c、stream-ui.c）必须提供此结构的一个实例，
 * 包含所有 UI 操作的函数指针。安装器通过 user-interface.c 中的包装函数
 * 间接调用这些指针。
 */

typedef struct __nv_installer_ui {

    /* detect：检测 UI 后端是否可用（如 ncurses 需要终端支持） */
    int (*detect)(Options *op);

    /* init：初始化 UI 并显示欢迎信息 */
    int (*init)(Options *op, FormatTextRows format_text_rows);

    /* set_title：设置 UI 窗口/界面标题 */
    void (*set_title)(Options *op, const char *title);

    /* get_input：显示提示消息并获取用户输入字符串，def 为默认值 */
    char *(*get_input)(Options *op, const char *def, const char *msg);

    /*
     * message：按指定级别显示消息。
     * 级别说明：
     *   NV_MSG_LEVEL_LOG     - 仅写入消息日志
     *   NV_MSG_LEVEL_MESSAGE - 显示给用户，可能需要确认（如点击 OK）
     *   NV_MSG_LEVEL_WARNING - 同上，但 UI 应标明这是一条警告
     *   NV_MSG_LEVEL_ERROR   - 同上，但 UI 应标明这是一条错误
     */
    void (*message)(Options *op, int level, const char *msg);

    /* command_output：显示命令执行的输出（如内核模块编译日志） */
    void (*command_output)(Options *op, const char *msg);

    /* approve_command_list：在专家模式下向用户展示命令列表，返回是否批准执行 */
    int (*approve_command_list)(Options *op, CommandList *c,const char *descr);

    /* yes_no：向用户提问 yes/no 问题，def 为默认答案，返回 TRUE=yes FALSE=no */
    int (*yes_no)(Options *op, const int def, const char *msg);

    /* multiple_choice：向用户展示多选题，返回用户选择的答案索引 */
    int (*multiple_choice)(Options *op, const char *question,
                           const char * const *answers, int num_answers,
                           int default_answer);

    /* paged_prompt：带可滚动文本的多选题（如显示许可证文本后询问） */
    int (*paged_prompt)(Options *op, const char *question,
                        const char *pager_title, const char *pager_text,
                        const char * const *answers, int num_answers,
                        int default_answer);

    /*
     * 进度显示三部曲：
     *   status_begin  - 开始显示进度（设置标题和初始消息）
     *   status_update - 更新进度百分比和消息
     *   status_end    - 结束进度显示
     * 在 begin 和 end 之间还可穿插 indeterminate_begin/end 调用。
     */
    void (*status_begin)(Options *op, const char *title, const char *msg);
    void (*status_update)(Options *op, const float percent, const char *msg);
    void (*status_end)(Options *op, const char *msg);

    /* update_indeterminate：不确定进度更新回调，由后台线程循环调用 */
    void (*update_indeterminate)(Options *, const char *msg);

    /* close：关闭并清理 UI 资源 */
    void (*close)(Options *op);


} InstallerUI;

#endif /* __NVIDIA_INSTALLER_UI_H__ */
