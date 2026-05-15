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
 * 【文件说明】命令列表子系统头文件。
 * 安装器使用"命令列表"模式组织安装操作：
 *   1. build_command_list() 根据 Package 中的文件条目生成一组有序命令
 *      （安装文件、创建链接、备份旧文件、运行后处理命令等）
 *   2. 可选地通过 UI 让用户审核命令列表
 *   3. execute_command_list() 按顺序执行所有命令，并显示进度
 */

#ifndef __NVIDIA_INSTALLER_COMMAND_LIST_H__
#define __NVIDIA_INSTALLER_COMMAND_LIST_H__

/*
 * CommandList：安装命令列表。
 * 封装了一组待执行的安装命令及其人类可读的描述。
 * 命令的实际结构 (struct __command) 对外部是不透明的。
 */

typedef struct {
    int num;                    /* 命令数量 */
    char **descriptions;        /* 每条命令的文字描述数组（用于 UI 审核显示） */
    struct __command *cmds;     /* 命令数组（内部结构，定义在 command-list.c 中） */
} CommandList;


/*
 * FileList：文件名列表。
 * 用于存储一组文件路径（如冲突文件列表）。
 */

typedef struct {
    int num;            /* 文件数量 */
    char **filename;    /* 文件名数组 */
} FileList;


/* 根据 Package 文件条目构建安装命令列表 */
CommandList *build_command_list(Options*, Package *);
/* 释放命令列表占用的内存 */
void free_command_list(Options*, CommandList*);
/* 执行命令列表中的所有命令（带进度显示） */
int execute_command_list(Options*, CommandList*, const char*, const char*);

#endif /* __NVIDIA_INSTALLER_COMMAND_LIST_H__ */
