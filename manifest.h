/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2013 NVIDIA Corporation
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
 * 【文件说明】清单文件（.manifest）类型管理头文件。
 * 提供文件类型与其能力标志之间的映射、解析和查询功能。
 * .manifest 文件是安装包的核心描述文件，列出所有待安装的文件及其类型。
 */

#ifndef __NVIDIA_INSTALLER_MANIFEST_H__
#define __NVIDIA_INSTALLER_MANIFEST_H__

#include "nvidia-installer.h"

/* 获取指定文件类型的能力标志（是否可安装、是否为链接等） */
PackageEntryFileCapabilities get_file_type_capabilities(
    PackageEntryFileType type);

/* 将 .manifest 中的文件类型字符串解析为枚举值，并输出对应的能力标志 */
PackageEntryFileType parse_manifest_file_type(
    const char *str,
    PackageEntryFileCapabilities *caps);

/* 根据当前选项，获取需要安装的文件类型列表（排除用户禁用的类型） */
void get_installable_file_type_list(
    Options *op,
    PackageEntryFileTypeList *installable_file_types);

/* 将符号链接类型添加到文件类型列表中（每个库类型对应的符号链接类型） */
void add_symlinks_to_file_type_list(
    PackageEntryFileTypeList *file_type_list);

/* 从文件类型列表中移除指定的文件类型 */
void remove_file_type_from_file_type_list(
    PackageEntryFileTypeList *list, PackageEntryFileType type);
#endif /* __NVIDIA_INSTALLER_MANIFEST_H__ */
