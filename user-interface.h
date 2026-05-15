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
 * 【文件说明】UI 包装函数声明头文件。
 * 声明了安装器中所有 UI 操作的公共接口函数（ui_* 系列）。
 * 这些函数是对 InstallerUI 调度表的包装，由 user-interface.c 实现。
 * 所有需要与用户交互的模块都通过这些函数进行 UI 操作，
 * 不直接调用底层的 ncurses 或 printf。
 *
 * user_interface.h
 */

#ifndef __NVIDIA_INSTALLER_USER_INTERFACE_H__
#define __NVIDIA_INSTALLER_USER_INTERFACE_H__

#include "nvidia-installer.h"
#include "command-list.h"

/* "继续/中止" 选择枚举（用于错误后询问用户） */
enum {
    CONTINUE_CHOICE = 0,              /* 继续 */
    ABORT_CHOICE,                     /* 中止 */
    NUM_CONTINUE_ABORT_CHOICES        /* 选项数量（必须是最后一个） */
};
/* "Continue" / "Abort" 选项字符串数组 */
extern const char * const CONTINUE_ABORT_CHOICES[];

/* ===== UI 操作函数声明 ===== */

/* 初始化 UI 子系统（尝试 ncurses，失败则回退到 stream） */
int   ui_init                (Options*);
/* 设置 UI 标题 */
void  ui_set_title           (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 获取用户输入字符串 */
char *ui_get_input           (Options*, const char*, const char*, ...) NV_ATTRIBUTE_PRINTF(3, 4);
/* 显示错误消息 */
void  ui_error               (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 显示警告消息 */
void  ui_warn                (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 显示普通信息消息 */
void  ui_message             (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 仅写入日志（不显示给用户） */
void  ui_log                 (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 仅在专家模式下显示的消息 */
void  ui_expert              (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 显示命令执行输出 */
void  ui_command_output      (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 在专家模式下让用户审核命令列表 */
int   ui_approve_command_list(Options*, CommandList*,const char*, ...) NV_ATTRIBUTE_PRINTF(3, 4);
/* 向用户提问 yes/no 问题 */
int   ui_yes_no              (Options*, const int, const char*, ...)   NV_ATTRIBUTE_PRINTF(3, 4);
/* 向用户展示多选题 */
int   ui_multiple_choice     (Options *, const char * const*, int, int,
                              const char *, ...)                       NV_ATTRIBUTE_PRINTF(5, 6);
/* 带可滚动文本的多选题（如许可证确认） */
int   ui_paged_prompt        (Options *, const char *, const char *,
                              const char *, const char * const *, int, int);
/* 开始显示进度条 */
void  ui_status_begin        (Options*, const char*, const char*, ...) NV_ATTRIBUTE_PRINTF(3, 4);
/* 更新进度条百分比 */
void  ui_status_update       (Options*, const float, const char*, ...) NV_ATTRIBUTE_PRINTF(3, 4);
/* 开始不确定进度显示（如编译内核模块时） */
void  ui_indeterminate_begin (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 结束不确定进度显示 */
void  ui_indeterminate_end   (Options*);
/* 结束进度条显示 */
void  ui_status_end          (Options*, const char*, ...)              NV_ATTRIBUTE_PRINTF(2, 3);
/* 关闭 UI 子系统 */
void  ui_close               (Options*);

/* UI 消息函数类型定义，用于在不同上下文中传递不同级别的消息函数 */
typedef void ui_message_func (Options*, const char*, ...) NV_ATTRIBUTE_PRINTF(2, 3);

#endif /* __NVIDIA_INSTALLER_USER_INTERFACE_H__ */
