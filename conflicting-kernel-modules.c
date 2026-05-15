/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2015-2021 NVIDIA Corporation
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
 * 【文件说明】冲突内核模块列表定义。
 * 列出了所有与当前驱动安装冲突的内核模块名称。
 * 安装新驱动前，安装器会按此列表的顺序尝试 rmmod 卸载这些模块。
 *
 * 【重要】列表按反向依赖顺序排列！
 * 即：被依赖的模块排在后面，依赖者排在前面。
 * 这样可以逐个卸载，不会因为依赖关系导致卸载失败。
 * 例如：nvidia-uvm 依赖 nvidia-modeset，而 nvidia-modeset 依赖 nvidia，
 * 所以 nvidia-uvm 排在 nvidia-modeset 前面，nvidia-modeset 排在 nvidia 前面。
 */

#include "common-utils.h"

const char * const conflicting_kernel_modules[] = {
    "nv_peer_mem",        /* 旧版 GPUDirect RDMA 对等内存模块（第三方） */
    "nvidia-peermem",     /* 新版 GPUDirect RDMA 对等内存模块 */
    "nvidia-vgpu-vfio",   /* vGPU（虚拟 GPU）VFIO 模块 */
    "nvidia-uvm",         /* 统一虚拟内存（Unified Virtual Memory）模块，CUDA 需要 */
    "nvidia-drm",         /* DRM/KMS（内核模式设置）支持模块 */
    "nvidia-modeset",     /* 显示模式设置模块 */
    "nvidia",             /* 主驱动内核模块 */
    "nvidia0", "nvidia1", "nvidia2", "nvidia3",  /* 旧式多 GPU 实例模块（已弃用） */
    "nvidia4", "nvidia5", "nvidia6", "nvidia7",
    "nvidia-frontend",    /* 旧式前端模块（已弃用） */
    "nvidia-kutils",      /* 旧式内核工具模块（已弃用） */
#ifdef BULLSEYE_BUILD
    "libcov-lkm",         /* 代码覆盖率内核模块（仅 Bullseye 测试构建中使用） */
#endif
};

/* 冲突模块总数 */
const int num_conflicting_kernel_modules = ARRAY_LEN(conflicting_kernel_modules);
