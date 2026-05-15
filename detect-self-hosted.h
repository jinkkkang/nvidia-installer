/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * 【文件说明】自托管（Self-Hosted）GPU 检测头文件。
 * 自托管 GPU 是指 CPU 和 GPU 集成在同一芯片上的 NVIDIA 产品
 * （如 Grace Hopper GH100、Blackwell GB100/GB110 超级芯片）。
 * 这些产品使用开源内核模块，安装器需要据此做出不同的安装决策。
 */
#ifndef __DETECT_SELF_HOSTED_H__
#define __DETECT_SELF_HOSTED_H__

/* 检测 PCI 设备 ID 是否为 Hopper 架构的自托管 GPU（GH100） */
static inline int pci_devid_is_self_hosted_hopper(unsigned short devid)
{
    return devid >= 0x2340 && devid <= 0x237f;       // GH100 自托管设备 ID 范围
}

/* 检测 PCI 设备 ID 是否为 Blackwell 架构的自托管 GPU（GB100/GB110） */
static inline int pci_devid_is_self_hosted_blackwell(unsigned short devid)
{
    return (devid >= 0x2940 && devid <= 0x297f)      // GB100 自托管设备 ID 范围
           || (devid >= 0x31c0 && devid <= 0x31ff)   // GB110 自托管设备 ID 范围
           || (devid == 0x31a1);                     // 特殊设备 ID
}

/* 检测 PCI 设备 ID 是否为任意架构的自托管 GPU */
static inline int pci_devid_is_self_hosted(unsigned short devid)
{
    return pci_devid_is_self_hosted_hopper(devid) ||
           pci_devid_is_self_hosted_blackwell(devid);
}

#endif
