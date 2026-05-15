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
 * 【文件说明】CRC32 校验和计算实现。
 * 基于《Numerical Recipes》第二版（pp 900-901）中的 16 位 CRC 算法扩展而来。
 *
 * 使用的生成多项式：
 *   x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 +
 *   x^5 + x^4 + x^2 + x^1 + 1
 *
 * CRC 用于验证安装包中文件的完整性以及已安装文件的一致性。
 *
 * crc.c
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>   /* mmap / munmap */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "nvidia-installer.h"
#include "user-interface.h"
#include "misc.h"
#include "crc.h"

/* 位掩码辅助宏 */
#define BIT(x) (1 << (x))

/* CRC32 生成多项式掩码（不含最高位 x^32） */
#define CRC_GEN_MASK (BIT(26) | BIT(23) | BIT(22) | BIT(16) | BIT(12) | \
                      BIT(11) | BIT(10) | BIT(8)  | BIT(7)  | BIT(5)  | \
                      BIT(4)  | BIT(2)  | BIT(1)  | BIT(0))


/*
 * crc_init() - 初始化 CRC 查找表中的单个条目。
 * 对输入的 crc 值执行 8 次移位-异或运算，模拟逐位处理一个字节。
 */
static uint32 crc_init(uint32 crc)
{
    int i;
    uint32 ans = crc;

    for (i=0; i < 8; i++) {
        if (ans & 0x80000000) {
            /* 最高位为 1：左移并异或生成多项式 */
            ans = (ans << 1) ^ CRC_GEN_MASK;
        } else {
            /* 最高位为 0：仅左移 */
            ans <<= 1;
        }
    }
    return ans;

} /* crc_init() */



/*
 * compute_crc_from_buffer() - 从内存缓冲区计算 CRC32 校验和。
 * 使用 256 项查找表加速计算。查找表在首次调用时初始化（静态变量）。
 * 初始值 cword = ~0（全 1），这是标准 CRC32 的做法。
 */
uint32 compute_crc_from_buffer(const uint8 *buf, int len)
{
    uint32 cword = ~0;                      /* 初始 CRC 值（全 1） */
    static uint32 *crctab = NULL;           /* 静态查找表（首次调用时初始化） */
    uint32 i;

    /* 首次调用时构建 256 项查找表 */
    if (!crctab) {
        crctab = (uint32 *) nvalloc(sizeof(uint32) * 256);
        for (i=0; i < 256; i++) {
            crctab[i] = crc_init(i << 24);  /* 将字节值放到高 8 位进行初始化 */
        }
    }

    /* 逐字节查表计算 CRC */
    for (i = 0; i < len; i++) {
        cword = crctab[buf[i] ^ (cword >> 24)] ^ (cword << 8);
    }

    return cword;
}



/*
 * compute_crc() - 计算指定文件的 CRC32 校验和。
 * 使用 mmap 将文件映射到内存，然后调用 compute_crc_from_buffer 计算。
 * 空文件返回 CRC 值 0。
 * 计算失败时打印警告并返回 ~0。
 */
uint32 compute_crc(Options *op, const char *filename)
{
    uint32 cword = ~0;             /* 默认返回值（计算失败时） */
    uint8 *buf = MAP_FAILED;
    int success = FALSE;
    int fd;
    struct stat stat_buf;
    size_t len = 0;

    /* 打开文件并获取文件大小 */
    if ((fd = open(filename, O_RDONLY)) == -1) goto done;
    if (fstat(fd, &stat_buf) == -1) goto done;

    /* 空文件特殊处理 */
    if (stat_buf.st_size == 0) {
        cword = 0;
        success = TRUE;
        goto done;
    }
    len = stat_buf.st_size;

    /* 将文件 mmap 到内存（只读、共享映射） */
    buf = mmap(0, len, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) goto done;

    /* 计算 CRC32 */
    cword = compute_crc_from_buffer(buf, len);

    success = TRUE;

 done:
    if (!success) {
        ui_warn(op, "Unable to compute CRC for file '%s' (%s).",
                filename, strerror(errno));
    }

    /* 清理资源 */
    if (buf != MAP_FAILED) {
        munmap(buf, len);
    }
    if (fd >= 0) {
        close(fd);
    }

    return cword;

} /* compute_crc() */
