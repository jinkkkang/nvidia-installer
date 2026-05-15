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
 * 【文件说明】安装完整性检查（sanity check）头文件。
 * 声明 sanity() 函数，用于验证已安装的 NVIDIA 驱动是否完好。
 *
 * sanity.h
 */

#ifndef __NVIDIA_INSTALLER_SANITY_H__
#define __NVIDIA_INSTALLER_SANITY_H__

/* 执行已安装驱动的完整性检查，返回 TRUE 表示通过 */
int sanity(Options *);

#endif /* __NVIDIA_INSTALLER_SANITY_H__ */
