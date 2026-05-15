/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2015 NVIDIA Corporation
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
 * 【文件说明】冲突内核模块列表头文件。
 * 定义了与 NVIDIA 驱动冲突的内核模块名称数组。
 * 安装器在安装前会检查这些模块是否已加载，若已加载则尝试卸载。
 * 典型的冲突模块包括 nouveau（开源 NVIDIA 驱动）等。
 */

#ifndef __CONFLICTING_KERNEL_MODULES_H__
#define __CONFLICTING_KERNEL_MODULES_H__

/* 冲突内核模块名称数组（定义在 conflicting-kernel-modules.c 中） */
extern const char * const conflicting_kernel_modules[];
/* 冲突内核模块数量 */
extern const int num_conflicting_kernel_modules;

#endif
