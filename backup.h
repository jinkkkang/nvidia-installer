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
 * 【文件说明】备份与卸载子系统头文件。
 * 声明了安装器的备份/还原/卸载功能的所有函数。
 * 安装器使用 /var/lib/nvidia/log 文件记录所有安装操作，
 * 包括安装了哪些文件、创建了哪些符号链接、备份了哪些被替换的文件。
 * 卸载时根据此日志文件进行回滚。
 */

#ifndef __NVIDIA_INSTALLER_BACKUP_H__
#define __NVIDIA_INSTALLER_BACKUP_H__

#include "nvidia-installer.h"

/* 备份日志中的操作类型标识 */
#define INSTALLED_SYMLINK  0      /* 安装了一个符号链接 */
#define INSTALLED_FILE     1      /* 安装了一个文件 */
#define BACKED_UP_SYMLINK  2      /* 备份了一个被替换的符号链接 */
#define BACKED_UP_FILE_NUM 100    /* 备份文件的编号起始值（100+n 表示第 n 个备份文件） */

/* 初始化备份系统，创建备份目录和日志文件 */
int init_backup                 (Options*, Package*);
/* 备份指定文件（将其移动到备份目录） */
int do_backup                   (Options*, const char*);
/* 在备份日志中记录已安装的文件 */
int log_install_file            (Options*, const char*);
/* 在备份日志中记录已创建的符号链接 */
int log_create_symlink          (Options*, const char*, const char*);
/* 检查系统中是否已存在 NVIDIA 驱动安装 */
int check_for_existing_driver   (Options*, Package*);
/* 卸载现有驱动（根据备份日志回滚） */
int uninstall_existing_driver   (Options*, const int, const int);
/* 运行现有安装的卸载程序（nvidia-uninstall） */
int run_existing_uninstaller    (Options*);
/* 报告当前已安装的驱动信息 */
int report_driver_information   (Options*);

/* 获取已安装驱动的版本号和描述信息 */
int get_installed_driver_version_and_descr(Options *, char **, char **);
/* 测试所有已安装文件是否仍存在（完整性检查） */
int test_installed_files(Options *op);
/* 在已安装文件列表中查找指定文件 */
int find_installed_file(Options *op, char *filename);

/* 在备份日志中记录创建的目录 */
int log_mkdir(Options *op, const char *dirs);

#endif /* __NVIDIA_INSTALLER_BACKUP_H__ */
