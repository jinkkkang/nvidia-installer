/*
 * Copyright (C) 2021 NVIDIA Corporation
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * 文件说明: nvpci-utils.h
 *
 * 本头文件定义了 PCI 设备扫描工具的接口，用于在系统中查找 NVIDIA GPU 设备。
 * 基于 libpciaccess 库提供 PCI 设备枚举功能。
 *
 * 内容包括：
 *   - NV_PCI_VENDOR_ID   : NVIDIA 的 PCI 厂商 ID (0x10de)
 *   - nvpci_find_gpu_by_vendor() : 按厂商 ID 查找 GPU 设备迭代器
 *   - nvpci_dev_is_vga()         : 判断设备是否为 VGA 控制器
 *
 * 依赖：
 *   - libpciaccess 库（通过 pciaccess.h 头文件引入）
 */

#ifndef __NVPCI_UTILS_H__
#define __NVPCI_UTILS_H__

#include <pciaccess.h>

/*
 * NV_PCI_VENDOR_ID - NVIDIA 公司的 PCI 厂商标识符。
 *
 * PCI SIG 分配给 NVIDIA 的厂商 ID 为 0x10de。
 * 用于在 PCI 总线上识别 NVIDIA 制造的设备。
 */
#define NV_PCI_VENDOR_ID 0x10de

/*
 * nvpci_find_gpu_by_vendor() - 查找指定厂商的 GPU PCI 设备。
 *
 * 参数：
 *   vendor_id - PCI 厂商 ID（如 NV_PCI_VENDOR_ID），
 *               传入 PCI_MATCH_ANY 可匹配所有厂商
 *
 * 返回值：
 *   PCI 设备迭代器，用于遍历匹配的设备
 *
 * 前置条件：调用者必须先调用 pci_system_init()
 * 后置条件：使用完毕后需调用 pci_iterator_destroy() 释放
 */
struct pci_device_iterator *nvpci_find_gpu_by_vendor(uint32_t vendor_id);

/*
 * nvpci_dev_is_vga() - 检测 PCI 设备是否为 VGA 控制器。
 *
 * 参数：
 *   dev - 指向 pci_device 结构体的指针
 *
 * 返回值：
 *   VGA 控制器 (0x0300) 返回非零值，其他（如 3D 控制器 0x0302）返回 0
 */
int nvpci_dev_is_vga(struct pci_device *dev);

#endif /* __NVPCI_UTILS_H__ */
