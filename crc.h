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
 * 【文件说明】CRC32 校验和计算头文件。
 * 用于验证安装包文件和已安装文件的完整性。
 */

#ifndef __NVIDIA_INSTALLER_CRC_H__
#define __NVIDIA_INSTALLER_CRC_H__

/* 从内存缓冲区计算 CRC32 校验和 */
uint32 compute_crc_from_buffer(const uint8 *buf, int len);
/* 从文件计算 CRC32 校验和 */
uint32 compute_crc(Options *op, const char *filename);

#endif /* __NVIDIA_INSTALLER_CRC_H__ */
