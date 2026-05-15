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
 * 【文件说明】initramfs 管理头文件。
 * 安装内核模块后，可能需要重建 initramfs（初始内存文件系统），
 * 以确保系统启动时能正确加载 NVIDIA 内核模块。
 * 不同发行版使用不同的 initramfs 工具（如 dracut、mkinitramfs、mkinitcpio）。
 */

#ifndef __INITRAMFS_H__
#define __INITRAMFS_H__

#include <nvidia-installer.h>

/* 扫描 initramfs 中是否包含 NVIDIA 模块（判断是否需要重建） */
int begin_initramfs_scan(Options *op);
/* 重建 initramfs（调用系统工具如 dracut/mkinitramfs） */
int update_initramfs(Options *op);

#endif
