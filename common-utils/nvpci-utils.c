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
 * 文件说明: nvpci-utils.c
 *
 * 本文件实现了 PCI 设备扫描相关的工具函数，用于查找系统中的 NVIDIA GPU 设备。
 * 基于 libpciaccess 库进行 PCI 设备枚举和匹配。
 *
 * 主要功能：
 *
 * 1. nvpci_find_gpu_by_vendor() - 按厂商 ID 查找 GPU 设备
 *    - 使用 libpciaccess 的 pci_id_match_iterator_create() 创建迭代器
 *    - 匹配 VGA 控制器 (0x0300) 和 3D 控制器 (0x0302) 两种设备类别
 *    - 调用者通常传入 NV_PCI_VENDOR_ID (0x10de) 来查找 NVIDIA GPU
 *
 * 2. nvpci_dev_is_vga() - 判断 PCI 设备是否为 VGA 控制器
 *    - 区分 VGA 控制器 (0x0300) 和 3D 控制器 (0x0302)
 *
 * PCI 设备类别编码说明：
 *   libpciaccess 中的 dev->device_class 字段编码如下：
 *   - 位 16-23 : 设备类别（class），如 0x03 = 显示控制器
 *   - 位 8-15  : 子类别（subclass），如 0x00 = VGA, 0x02 = 3D
 *   - 位 0-7   : 编程接口（interface）
 *
 * 使用前提：
 *   调用者必须在使用 nvpci_find_gpu_by_vendor() 之前调用 pci_system_init()，
 *   在不再需要时调用 pci_system_cleanup()。
 */

#include "nvpci-utils.h"

/*
 * PCI_CLASS_DISPLAY_VGA - VGA 显示控制器的设备类别值。
 *
 * 值为 0x30000，对应于：
 *   - 类别 (class) = 0x03（显示控制器）
 *   - 子类别 (subclass) = 0x00（VGA 兼容控制器）
 *   - 接口 (interface) = 0x00
 *
 * 注意：在 libpciaccess 中，设备类别存储在 device_class 的位 16-23，
 *       子类别在位 8-15，所以 class=0x03, subclass=0x00 编码为 0x30000。
 */
const uint32_t PCI_CLASS_DISPLAY_VGA = 0x30000;

/*
 * PCI_CLASS_SUBCLASS_MASK - 类别和子类别的掩码。
 *
 * 值为 0xffff00，用于屏蔽 device_class 中的接口位（位 0-7），
 * 只保留类别（位 16-23）和子类别（位 8-15）的信息。
 */
const uint32_t PCI_CLASS_SUBCLASS_MASK = 0xffff00;

/*
 * nvpci_find_gpu_by_vendor() - 查找指定厂商的 GPU PCI 设备。
 *
 * 功能：使用 libpciaccess 创建一个 PCI 设备迭代器，
 *       用于遍历所有匹配指定厂商 ID 的 VGA 和 3D 控制器设备。
 *
 * 设备匹配逻辑：
 *   - vendor_id     : 匹配传入的厂商 ID（可以为 PCI_MATCH_ANY）
 *   - device_id     : 匹配任意设备 ID
 *   - subvendor_id  : 匹配任意子厂商 ID
 *   - subdevice_id  : 匹配任意子设备 ID
 *   - device_class  : 0x30000（VGA 显示控制器）
 *   - device_class_mask : 0xfffd00
 *     通过将掩码的位 9 (0x200) 清零，同时匹配：
 *     - 0x30000 (VGA 控制器，subclass = 0x00)
 *     - 0x30200 (3D 控制器，subclass = 0x02)
 *     这是因为 0x200 正好是 VGA(0x00) 和 3D(0x02) 子类别之间的差异位
 *
 * 参数：
 *   vendor_id - 要匹配的 PCI 厂商 ID（如 0x10de 为 NVIDIA），
 *               可以传入 PCI_MATCH_ANY 匹配所有厂商
 *
 * 返回值：
 *   PCI 设备迭代器指针，调用者使用 pci_device_next() 遍历匹配的设备，
 *   使用完毕后需要调用 pci_iterator_destroy() 释放
 */
struct pci_device_iterator *nvpci_find_gpu_by_vendor(uint32_t vendor_id)
{
    const struct pci_id_match match = {
        .vendor_id = vendor_id,
        .device_id = PCI_MATCH_ANY,
        .subvendor_id = PCI_MATCH_ANY,
        .subdevice_id = PCI_MATCH_ANY,
        .device_class = PCI_CLASS_DISPLAY_VGA,
        /*
         * 忽略子类别的位 1（值 0x200），使得掩码同时匹配：
         *   0x30000（VGA 控制器）和 0x30200（3D 控制器）
         */
        .device_class_mask = PCI_CLASS_SUBCLASS_MASK & ~0x200,
    };

    return pci_id_match_iterator_create(&match);
}

/*
 * nvpci_dev_is_vga() - 检测 PCI 设备是否为 VGA 控制器。
 *
 * 功能：判断给定的 PCI 设备是否属于 VGA 设备类别 (0x0300)，
 *       而非 3D 控制器类别 (0x0302)。
 *
 * 判断方法：
 *   将 device_class 与 PCI_CLASS_SUBCLASS_MASK 进行按位与，
 *   屏蔽接口位后，与 PCI_CLASS_DISPLAY_VGA (0x30000) 比较。
 *
 * 参数：
 *   dev - 指向 pci_device 结构体的指针
 *
 * 返回值：
 *   如果设备是 VGA 控制器（0x0300）返回非零值（真）
 *   如果设备是 3D 控制器（0x0302）或其他类型返回 0（假）
 */
int nvpci_dev_is_vga(struct pci_device *dev)
{
    return (dev->device_class & PCI_CLASS_SUBCLASS_MASK) ==
           PCI_CLASS_DISPLAY_VGA;
}
