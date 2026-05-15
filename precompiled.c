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
 *
 * precompiled.c - this source file contains functions for dealing
 * with precompiled kernel interfaces.
 *
 * ===========================================================================
 * 文件说明：precompiled.c
 * ===========================================================================
 *
 * 本文件实现了 nvidia-installer 中预编译内核接口包（Precompiled Kernel Interface
 * Package）的核心处理逻辑。预编译包是一种自定义的二进制文件格式，用于分发已针对
 * 特定内核版本编译好的 NVIDIA 内核接口文件（.o）或完整内核模块（.ko），从而避免
 * 用户在安装驱动时需要从源码重新编译。
 *
 * 主要功能包括：
 *
 * 1. 内核版本读取（read_proc_version）：
 *    从 /proc/version 读取当前运行内核的版本字符串，用于与预编译包中记录的
 *    内核版本进行匹配，确保预编译的二进制文件与当前内核兼容。
 *
 * 2. 预编译包解析（get_precompiled_info）：
 *    打开并解析一个预编译包文件，验证其魔数、版本号、内核版本匹配性，
 *    读取包中所有文件的元数据和数据内容，构建 PrecompiledInfo 结构体。
 *
 * 3. 预编译包解压（precompiled_file_unpack / precompiled_unpack）：
 *    将预编译包中的文件解压（写出）到指定的输出目录。
 *
 * 4. 预编译包打包（precompiled_pack）：
 *    将 PrecompiledInfo 结构体中的所有信息按照预编译包二进制格式序列化，
 *    写入到指定的输出文件中。
 *
 * 5. 文件读取辅助（precompiled_read_interface / precompiled_read_module）：
 *    从磁盘读取单个预编译接口文件或模块文件，填充 PrecompiledFileInfo 结构体。
 *
 * 6. 文件查找（precompiled_find_file）：
 *    在已解析的预编译包中按文件名查找特定文件。
 *
 * 7. 内存释放（free_precompiled / free_precompiled_file_data）：
 *    释放预编译包相关数据结构占用的内存。
 *
 * 8. 辅助工具（byte_tail）：
 *    从文件的指定字节偏移位置读取到文件末尾，返回新分配的缓冲区。
 *
 * 预编译包的二进制格式在 precompiled.h 中有详细描述，格式概要：
 *   - 包头：魔数 "\aNVIDIA\a" + 格式版本 + 驱动版本 + 描述 + proc version + 文件数量
 *   - 文件条目：每个文件以 "FILE" 开头、"END." 结尾，中间包含类型、属性、
 *     文件名、链接模块名、核心目标文件名、目标目录、CRC、数据、签名等字段
 *
 * 本文件中使用的内存分配函数（nvalloc/nvrealloc/nvfree/nvstrdup/nvstrcat 等）
 * 是 nvidia-installer 项目自定义的包装函数，定义在 misc.h/misc.c 中，
 * 通常在内存分配失败时会直接终止程序。
 */


#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>

#include "nvidia-installer.h"
#include "user-interface.h"
#include "precompiled.h"
#include "misc.h"
#include "crc.h"



/*
 * precompiled_read_fileinfo() 的前向声明。
 *
 * 该函数用于从预编译包的二进制缓冲区中解析单个文件条目（PrecompiledFileInfo），
 * 在 get_precompiled_info() 中被调用。具体实现在本文件后面。
 */
static int precompiled_read_fileinfo(Options *op, PrecompiledFileInfo *fileInfos,
                                     int index, char *buf, int offset, int size);

/*
 * read_uint32() - 从缓冲区的指定偏移位置读取一个 32 位无符号整数（小端序）
 *
 * 参数：
 *   buf    - 指向数据缓冲区的指针
 *   offset - 指向当前读取位置偏移量的指针，读取完成后会自动向前推进 4 字节
 *
 * 返回值：
 *   读取到的 32 位无符号整数
 *
 * 处理流程：
 *   按照小端序（Little-Endian）从 buf[offset] 开始依次读取 4 个字节，
 *   组装成一个 uint32 值。读取顺序为 buf[offset+3] 在最高位，buf[offset] 在最低位。
 *   注意：代码中先读取高字节再左移的写法等价于小端序读取。
 *   读取完成后，*offset 增加 sizeof(uint32)（即 4 字节）。
 */

static uint32 read_uint32(const char *buf, int *offset)
{
    uint32 ret = 0;

    /* 读取第 4 个字节（最高有效字节），放入 ret 的低 8 位 */
    ret += (((uint32) buf[*offset + 3]) & 0xff);
    ret <<= 8;  /* 左移 8 位，为下一个字节腾出空间 */

    /* 读取第 3 个字节 */
    ret += (((uint32) buf[*offset + 2]) & 0xff);
    ret <<= 8;

    /* 读取第 2 个字节 */
    ret += (((uint32) buf[*offset + 1]) & 0xff);
    ret <<= 8;

    /* 读取第 1 个字节（最低有效字节） */
    ret += (((uint32) buf[*offset]) & 0xff);
    ret <<= 0;  /* 左移 0 位，实际上无操作，保持代码对称性 */

    /* 将偏移量向前推进 4 字节 */
    *offset += sizeof(uint32);

    return ret;

}



/*
 * read_proc_version() - 读取 /proc/version 文件的内容，返回内核版本字符串
 *
 * 参数：
 *   op               - 选项结构体指针，用于调用 ui_warn() 等界面函数
 *   proc_mount_point - proc 文件系统的挂载点路径（通常为 "/proc"）
 *
 * 返回值：
 *   成功时返回新分配的字符串，包含 /proc/version 的内容（去掉末尾换行符）；
 *   失败时返回 NULL。
 *   调用者负责释放返回的字符串。
 *
 * 处理流程：
 *   1. 拼接完整路径 "<proc_mount_point>/version"
 *   2. 以只读方式打开该文件
 *   3. 循环读取文件内容到动态增长的缓冲区（因为 /proc 文件不支持 mmap）
 *   4. 将第一个换行符替换为 '\0'，确保字符串只包含第一行内容
 *   5. 清理临时资源并返回结果字符串
 *
 * XXX 注意：这里硬编码了路径拼接方式。理论上可以使用 getmntent() 来动态
 * 确定 proc 文件系统的挂载位置，但目前未实现。
 */

char *read_proc_version(Options *op, const char *proc_mount_point)
{
    int fd, len, version_len;
    char *version, *c = NULL;
    char *proc_verson_filename;

    /*
     * 计算完整路径所需的缓冲区长度：
     * proc_mount_point 的长度 + "/version" 的长度（8 字符）+ 1（'\0' 终止符）
     * 注意：原注释中 9 = strlen("/version") + 1，其中 "/version" 是 8 个字符
     */
    len = strlen(proc_mount_point) + 9; /* strlen("/version") + 1 */
    proc_verson_filename = (char *) nvalloc(len);
    snprintf(proc_verson_filename, len, "%s/version", proc_mount_point);

    /* 以只读方式打开 /proc/version 文件 */
    if ((fd = open(proc_verson_filename, O_RDONLY)) == -1) {
        ui_warn(op, "Unable to open the file '%s' (%s).",
                proc_verson_filename, strerror(errno));
        return NULL;
    }

    /*
     * /proc 文件系统下的文件不支持 mmap(2)，所以只能通过 read(2)
     * 来逐块读取文件内容。使用动态增长的缓冲区来容纳内容。
     */

    len = version_len = 0;  /* len: 已读取的总字节数; version_len: 缓冲区总容量 */
    version = NULL;

    while (1) {
        int ret;

        /*
         * 如果已读取的数据长度已经达到缓冲区容量，则扩展缓冲区。
         * 每次扩展 NV_LINE_LEN 字节（定义在 nvidia-installer.h 中）。
         */
        if (version_len == len) {
            version_len += NV_LINE_LEN;
            version = nvrealloc(version, version_len);
            c = version + len;  /* 更新写入位置指针 */
        }

        /* 读取数据到缓冲区剩余空间 */
        ret = read(fd, c, version_len - len);
        if (ret == -1) {
            /* 读取失败，记录警告并释放缓冲区 */
            ui_warn(op, "Error reading %s (%s).",
                    proc_verson_filename, strerror(errno));
            nvfree(version);
            version = NULL;
            goto done;
        }
        if (ret == 0) {
            /* 已读取到文件末尾，添加 '\0' 终止符并退出循环 */
            *c = '\0';
            break;
        }
        /* 更新已读取长度和写入位置 */
        len += ret;
        c += ret;
    }

    /*
     * 将第一个换行符替换为 '\0'，确保返回的字符串只包含第一行内容。
     * /proc/version 通常只有一行，但末尾可能有换行符。
     */

    c = version;
    while ((*c != '\0') && (*c != '\n')) c++;
    *c = '\0';

 done:
    /* 释放文件路径字符串并关闭文件描述符 */
    nvfree(proc_verson_filename);
    close(fd);

    return version;
}



/*
 * get_precompiled_info() - 从指定的预编译包文件中加载并解析预编译信息
 *
 * 参数：
 *   op                       - 选项结构体指针，用于调用 ui_expert()/ui_log() 等界面函数
 *   filename                 - 预编译包文件的路径
 *   real_proc_version_string - 当前运行内核的 /proc/version 字符串。
 *                              如果非 NULL，将与包中记录的 proc version 进行比较，
 *                              不匹配则返回 NULL。如果为 NULL 则跳过此检查。
 *   package_version          - 期望的驱动版本字符串。如果非 NULL，将与包中记录的
 *                              驱动版本进行比较，不匹配则返回 NULL。
 *                              如果为 NULL 则跳过此检查。
 *   search_filelist          - 以 NULL 结尾的文件名列表。如果非 NULL，包中必须
 *                              包含列表中的所有文件，否则视为包无效并返回 NULL。
 *
 * 返回值：
 *   成功时返回新分配的 PrecompiledInfo 结构体指针，包含包的完整信息；
 *   失败时返回 NULL。
 *   调用者负责通过 free_precompiled() 释放返回的结构体。
 *
 * 处理流程：
 *   1. 打开文件并获取文件大小
 *   2. 使用 mmap 将文件映射到内存
 *   3. 验证包头魔数（"\aNVIDIA\a"）
 *   4. 验证包格式版本号
 *   5. 读取驱动版本字符串并与期望版本对比
 *   6. 读取描述字符串
 *   7. 读取 /proc/version 字符串并与当前内核版本对比
 *   8. 读取所有文件条目（调用 precompiled_read_fileinfo）
 *   9. 验证 search_filelist 中的所有文件都存在于包中
 *  10. 构建并返回 PrecompiledInfo 结构体
 *
 * 注意：打开文件失败或格式不匹配并不算严重错误（可能只是该包不适用于当前内核），
 * 因此只输出 expert 级别的日志，不输出 error 级别的错误。
 */

PrecompiledInfo *get_precompiled_info(Options *op,
                                      const char *filename,
                                      const char *real_proc_version_string,
                                      const char *package_version,
                                      char *const *search_filelist)
{
    int fd, offset, num_files, i;
    char *buf;
    uint32 val, size;
    char *version, *description, *proc_version_string;
    struct stat stat_buf;
    PrecompiledInfo *info = NULL;
    PrecompiledFileInfo *fileInfos = NULL;

    /* 初始化局部变量，确保 done 标签处的清理代码能安全执行 */
    fd = size = 0;
    buf = description = proc_version_string = version = NULL;

    /* 打开预编译包文件（只读模式） */

    if ((fd = open(filename, O_RDONLY)) == -1) {
        ui_expert(op, "Unable to open precompiled kernel interface file "
                  "'%s' (%s)", filename, strerror(errno));
        goto done;
    }

    /* 通过 fstat 获取文件大小 */

    if (fstat(fd, &stat_buf) == -1) {
        ui_expert(op, "Unable to determine '%s' file length (%s).",
                  filename, strerror(errno));
        goto done;
    }
    size = stat_buf.st_size;

    /*
     * 检查文件最小长度：文件大小必须至少为包头固定部分的长度
     * （PRECOMPILED_PKG_CONSTANT_LENGTH），否则不可能是有效的预编译包
     */

    if (size < PRECOMPILED_PKG_CONSTANT_LENGTH) {
        ui_expert(op, "File '%s' appears to be too short.", filename);
        goto done;
    }

    /* 使用 mmap(2) 将整个文件映射到内存，以只读共享方式映射 */

    buf = mmap(0, size, PROT_READ, MAP_FILE|MAP_SHARED, fd, 0);
    if (buf == (void *) -1) {
        ui_expert(op, "Unable to mmap file %s (%s).",
                  filename, strerror(errno));
        goto done;
    }
    offset = 0;  /* 初始化读取偏移量为 0 */

    /*
     * 验证包头魔数：前 8 字节必须是 "\aNVIDIA\a"
     * （PRECOMPILED_PKG_HEADER 定义在 precompiled.h 中）
     */

    if (strncmp(buf + offset, PRECOMPILED_PKG_HEADER, 8) != 0) {
        ui_expert(op, "File '%s': unrecognized file format.", filename);
        goto done;
    }
    offset += 8;

    /*
     * 读取并验证包格式版本号：必须与当前支持的版本
     * （PRECOMPILED_PKG_VERSION，值为 2）匹配
     */

    val = read_uint32(buf, &offset);
    if (val != PRECOMPILED_PKG_VERSION) {
        ui_expert(op, "Incompatible package format version %d: expected %d.",
                  val, PRECOMPILED_PKG_VERSION);
        goto done;
    }

    /*
     * 读取驱动版本字符串：
     * 先读取 4 字节的长度值，然后读取对应长度的字符串内容
     */

    val = read_uint32(buf, &offset);
    /* 边界检查：版本字符串长度加上固定部分不应超过文件总大小 */
    if ((val + PRECOMPILED_PKG_CONSTANT_LENGTH) > size) {
        ui_expert(op, "Invalid file '%s' (bad version string length %d).",
                  filename, val);
        goto done;
    }
    if (val > 0) {
        /* 分配内存并复制版本字符串，手动添加 '\0' 终止符 */
        version = nvalloc(val+1);
        memcpy(version, buf + offset, val);
        version[val] = '\0';
    } else {
        version = NULL;
    }
    offset += val;

    /*
     * 版本验证：
     * - 如果版本字符串为空（读取失败），则包无效
     * - 如果调用者指定了期望的 package_version，则必须完全匹配
     */

    if (!version ||
        (package_version && strcmp(version, package_version) != 0)) {
        goto done;
    }


    /*
     * 读取描述字符串（description）：
     * 格式与版本字符串相同 - 4 字节长度 + 对应内容
     */

    val = read_uint32(buf, &offset);
    if ((val + PRECOMPILED_PKG_CONSTANT_LENGTH) > size) {
        ui_expert(op, "Invalid file '%s' (bad description string length %d).",
                  filename, val);
        goto done;
    }
    description = nvalloc(val+1);
    memcpy(description, buf + offset, val);
    description[val] = '\0';
    offset += val;

    /*
     * 读取 /proc/version 字符串：
     * 该字符串记录了编译预编译包时目标内核的 /proc/version 内容，
     * 用于在安装时匹配当前运行的内核
     */

    val = read_uint32(buf, &offset);
    if ((val + PRECOMPILED_PKG_CONSTANT_LENGTH) > size) {
        ui_expert(op, "Invalid file '%s' (bad version string length %d).",
                  filename, val);
        goto done;
    }
    proc_version_string = nvalloc(val+1);
    memcpy(proc_version_string, buf + offset, val);
    offset += val;
    proc_version_string[val] = '\0';

    /*
     * 内核版本匹配检查：
     * 如果调用者提供了当前内核的 proc version 字符串（real_proc_version_string），
     * 则将其与包中记录的 proc version 进行比较。
     * 不匹配则说明该预编译包不适用于当前内核，静默返回 NULL。
     */

    if (real_proc_version_string &&
        (strcmp(real_proc_version_string, proc_version_string) != 0)) {
        goto done;
    }

    /* 内核版本匹配成功，记录日志 */
    ui_log(op, "A precompiled kernel interface for kernel '%s' has been "
           "found here: %s.", description, filename);

    /*
     * 读取包中的文件数量，然后逐一解析每个文件条目。
     * 为所有文件条目分配 PrecompiledFileInfo 数组。
     */
    num_files = read_uint32(buf, &offset);
    fileInfos = nvalloc(num_files * sizeof(PrecompiledFileInfo));
    for (i = 0; i < num_files; i++) {
        int ret;
        /* 解析第 i 个文件条目，ret 返回该条目占用的字节数 */
        ret = precompiled_read_fileinfo(op, fileInfos, i, buf, offset, size);

        if (ret > 0) {
            /* 成功，推进偏移量 */
            offset += ret;
        } else {
            /* 解析失败，记录日志并放弃整个包 */
            ui_log(op, "An error occurred while trying to parse '%s'.",
                   filename);
            goto done;
        }
    }

    /*
     * 包完整性验证：
     *
     * 如果调用者提供了 search_filelist（以 NULL 结尾的文件名列表），
     * 则验证列表中的每个文件都存在于预编译包中。
     * 这确保预编译包包含所有必需的文件。
     */

    if (search_filelist != NULL) {
        int index;

        /* 遍历搜索文件列表中的每个文件名 */
        for (index = 0; search_filelist[index]; index++) {
            int found = FALSE;

            /* 在包的文件列表中查找该文件名 */
            for (i = 0; i < num_files; i++) {
                if (!strcmp(search_filelist[index], fileInfos[i].name)) {
                    found = TRUE;
                    break;
                }
            }

            /* 如果某个必需文件不在包中，则包无效 */
            if (!found) {
                ui_log(op, "Required file '%s' not found in package '%s'",
                       search_filelist[index], filename);
                goto done;
            }
        }
    }


    /*
     * 至此所有验证都已通过，不会再失败。
     * 分配并初始化 PrecompiledInfo 结构体，将解析结果填入。
     */

    info = (PrecompiledInfo *) nvalloc(sizeof(PrecompiledInfo));
    info->package_size = size;
    info->version = version;
    info->proc_version_string = proc_version_string;
    info->description = description;
    info->num_files = num_files;
    info->files = fileInfos;

    /*
     * 将已转移所有权的指针置为 NULL，防止在下面的 done 标签处
     * 被错误释放。这些字符串和数组的生命周期现在由 PrecompiledInfo 管理。
     */

    proc_version_string = description = version = NULL;
    fileInfos = NULL;

done:

    /* 清理资源：解除 mmap 映射、关闭文件、释放未被转移所有权的字符串 */

    if (buf) munmap(buf, size);
    if (fd >= 0) close(fd);
    nvfree(description);
    nvfree(proc_version_string);
    nvfree(fileInfos);
    nvfree(version);

    return info;

}


/*
 * precompiled_file_unpack() - 将预编译包中的单个文件解压到指定输出目录
 *
 * 参数：
 *   op               - 选项结构体指针，用于调用 ui_error() 等界面函数
 *   fileInfo         - 指向要解压的文件信息结构体的指针，包含文件名、
 *                      目标子目录、数据内容和大小等信息
 *   output_directory - 输出的基础目录路径
 *
 * 返回值：
 *   成功返回 TRUE，失败返回 FALSE
 *
 * 处理流程：
 *   1. 拼接完整的输出路径：output_directory/target_directory/name
 *   2. 创建（或截断）输出文件
 *   3. 使用 lseek + write 设置文件长度
 *   4. 使用 mmap 映射输出文件到内存
 *   5. 通过 memcpy 将文件数据复制到映射区域
 *   6. 清理资源
 */

int precompiled_file_unpack(Options *op, const PrecompiledFileInfo *fileInfo,
                            const char *output_directory)
{
    int ret = FALSE, dst_fd = 0;
    char *dst_path, *dst = NULL;

    /*
     * 拼接完整的输出文件路径：
     * <output_directory>/<target_directory>/<name>
     * nvstrcat 会自动分配足够的内存并拼接所有参数，以 NULL 结束参数列表
     */
    dst_path = nvstrcat(output_directory, "/", fileInfo->target_directory, "/",
                        fileInfo->name, NULL);

    /*
     * 创建输出文件：
     * O_CREAT - 如果文件不存在则创建
     * O_RDWR  - 以读写方式打开（mmap 需要读写权限）
     * O_TRUNC - 如果文件已存在则截断为 0 长度
     * 权限设置为 644（所有者可读写，组和其他用户可读）
     */

    if ((dst_fd = open(dst_path, O_CREAT | O_RDWR | O_TRUNC,
                       S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
        ui_error(op, "Unable to open output file '%s' (%s).", dst_path,
                 strerror(errno));
        goto done;
    }

    /*
     * 设置输出文件的长度：
     * 通过 lseek 定位到文件末尾前一字节的位置，然后写入一个空字节，
     * 这样文件系统就会为整个文件分配空间。这是 UNIX 系统中设置文件大小
     * 并确保后续 mmap 能够成功的常用技巧。
     */

    if ((lseek(dst_fd, fileInfo->size - 1, SEEK_SET) == -1) ||
        (write(dst_fd, "", 1) == -1)) {
        ui_error(op, "Unable to set output file '%s' length %d (%s).\n",
                 dst_path, fileInfo->size, strerror(errno));
        goto done;
    }

    /*
     * 将输出文件以读写共享方式映射到内存。
     * 之后可以通过 memcpy 直接将数据写入文件。
     */

    dst = mmap(0, fileInfo->size, PROT_READ|PROT_WRITE, MAP_FILE|MAP_SHARED, dst_fd, 0);
    if (dst == (void *) -1) {
        ui_error(op, "Unable to mmap output file %s (%s).\n",
                 dst_path, strerror(errno));
        goto done;
    }

    /* 将预编译文件数据复制到映射的输出文件中 */

    memcpy(dst, fileInfo->data, fileInfo->size);

    ret = TRUE;

done:

    /* 清理：释放路径字符串、解除 mmap 映射、关闭文件描述符 */

    nvfree(dst_path);
    if (dst) munmap(dst, fileInfo->size);
    if (dst_fd > 0) close(dst_fd);

    return ret;
}


/*
 * precompiled_unpack() - 将预编译包中的所有文件解压到指定目录
 *
 * 参数：
 *   op               - 选项结构体指针
 *   info             - 已解析的预编译包信息结构体指针
 *   output_directory - 输出目录路径
 *
 * 返回值：
 *   所有文件都成功解压返回 TRUE，任一文件解压失败返回 FALSE
 *
 * 处理流程：
 *   遍历包中的所有文件，逐一调用 precompiled_file_unpack() 解压。
 *   任何一个文件解压失败则立即返回 FALSE，不继续处理后续文件。
 */
int precompiled_unpack(Options *op, const PrecompiledInfo *info,
                       const char *output_directory)
{
    int i;

    /* 空指针保护 */
    if (!info) {
        return FALSE;
    }

    for (i = 0; i < info->num_files; i++) {
        if (!precompiled_file_unpack(op, &(info->files[i]), output_directory)) {
            return FALSE;
        }
    }

    return TRUE;
}




/*
 * encode_uint32() - 将一个 32 位无符号整数以小端序写入数据缓冲区
 *
 * 参数：
 *   val    - 要写入的 32 位无符号整数
 *   data   - 目标数据缓冲区
 *   offset - 指向当前写入位置偏移量的指针，写入完成后自动向前推进 4 字节
 *
 * 处理流程：
 *   按小端序将 val 的 4 个字节依次写入 data 的 offset 位置。
 *   data[offset+0] = 最低有效字节，data[offset+3] = 最高有效字节。
 *   这是 read_uint32() 的逆操作。
 */

static void encode_uint32(uint32 val, uint8 *data, int *offset)
{
    data[*offset + 0] = ((val >> 0)  & 0xff);  /* 最低有效字节 */
    data[*offset + 1] = ((val >> 8)  & 0xff);
    data[*offset + 2] = ((val >> 16) & 0xff);
    data[*offset + 3] = ((val >> 24) & 0xff);  /* 最高有效字节 */

    *offset += sizeof(uint32);
}


/*
 * precompiled_pack() - 将 PrecompiledInfo 结构体中的所有信息按照预编译包
 * 的二进制格式打包，写入指定的输出文件
 *
 * 参数：
 *   info             - 指向要打包的 PrecompiledInfo 结构体的指针，
 *                      包含版本信息、描述、proc version、所有文件等
 *   package_filename - 输出文件的路径
 *
 * 返回值：
 *   始终返回 TRUE（注意：如果 nv_open/nv_set_file_length/nv_mmap 失败，
 *   这些函数内部会直接终止程序，不会返回错误码）
 *
 * 处理流程：
 *   1. 计算所有可变长度字段的总大小
 *   2. 计算输出文件总大小 = 固定头部 + 可变字段
 *   3. 创建并 mmap 输出文件
 *   4. 按照二进制格式依次写入：
 *      a. 包头魔数 "\aNVIDIA\a"
 *      b. 包格式版本号
 *      c. 驱动版本字符串（长度 + 内容）
 *      d. 描述字符串（长度 + 内容）
 *      e. proc version 字符串（长度 + 内容）
 *      f. 文件数量
 *      g. 逐个写入每个文件条目
 *   5. 解除 mmap 映射并关闭文件
 *
 * 二进制格式详见 precompiled.h 中的注释。
 */

int precompiled_pack(const PrecompiledInfo *info, const char *package_filename)
{
    int fd, offset;
    uint8 *out;
    int version_len, description_len, proc_version_len;
    int total_len, files_len, i;

    /*
     * 计算各可变长度字段的长度：
     * - 驱动版本字符串长度
     * - 描述字符串长度
     * - /proc/version 字符串长度
     */

    version_len = strlen(info->version);
    description_len = strlen(info->description);
    proc_version_len = strlen(info->proc_version_string);

    /*
     * 计算所有文件条目的总长度：
     * 每个文件的总长度 = 固定部分（PRECOMPILED_FILE_CONSTANT_LENGTH）
     *                  + 文件名长度
     *                  + 链接模块名长度
     *                  + 核心目标文件名长度
     *                  + 目标目录名长度
     *                  + 文件数据大小
     *                  + 签名数据大小
     */
    for (files_len = i = 0; i < info->num_files; i++) {
        files_len += PRECOMPILED_FILE_CONSTANT_LENGTH +
                     strlen(info->files[i].name) +
                     strlen(info->files[i].linked_module_name) +
                     strlen(info->files[i].core_object_name) +
                     strlen(info->files[i].target_directory) +
                     info->files[i].size +
                     info->files[i].signature_size;
    }

    /* 总长度 = 包头固定部分 + 版本字符串 + 描述字符串 + proc version + 文件条目 */
    total_len = PRECOMPILED_PKG_CONSTANT_LENGTH +
        version_len + description_len + proc_version_len + files_len;

    /*
     * 创建输出文件：
     * nv_open 是 nvidia-installer 的包装函数，失败时会直接终止程序
     */

    fd = nv_open(package_filename, O_CREAT|O_RDWR|O_TRUNC,
                 S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);

    /* 设置输出文件的大小为计算出的总长度 */

    nv_set_file_length(package_filename, fd, total_len);

    /* 以读写共享方式 mmap 输出文件 */

    out = nv_mmap(package_filename, total_len, PROT_READ|PROT_WRITE,
                  MAP_FILE|MAP_SHARED, fd);
    offset = 0;

    /* ===== 写入包头部 ===== */

    /* 写入 8 字节的包头魔数 "\aNVIDIA\a" */

    memcpy(&(out[0]), PRECOMPILED_PKG_HEADER, 8);
    offset += 8;

    /* 写入 4 字节的包格式版本号（PRECOMPILED_PKG_VERSION = 2） */

    encode_uint32(PRECOMPILED_PKG_VERSION, out, &offset);

    /* 写入驱动版本字符串：4 字节长度 + 字符串内容（不含 '\0'） */

    encode_uint32(version_len, out, &offset);

    if (version_len) {
        memcpy(&(out[offset]), info->version, version_len);
        offset += version_len;
    }

    /* 写入描述字符串：4 字节长度 + 字符串内容 */

    encode_uint32(description_len, out, &offset);

    if (description_len) {
        memcpy(&(out[offset]), info->description, description_len);
        offset += description_len;
    }

    /* 写入 /proc/version 字符串：4 字节长度 + 字符串内容 */

    encode_uint32(proc_version_len, out, &offset);

    memcpy(&(out[offset]), info->proc_version_string, proc_version_len);
    offset += proc_version_len;

    /* 写入包中的文件数量 */

    encode_uint32(info->num_files, out, &offset);

    /* ===== 逐个写入文件条目 ===== */
    for (i = 0; i < info->num_files; i++) {
        PrecompiledFileInfo *file = &(info->files[i]);
        uint32 name_len = strlen(file->name);
        uint32 linked_module_name_len = strlen(file->linked_module_name);
        uint32 core_object_name_len = strlen(file->core_object_name);
        uint32 target_directory_len = strlen(file->target_directory);

        /* 写入 4 字节的文件条目头标记 "FILE" */
        memcpy(&(out[offset]), PRECOMPILED_FILE_HEADER, 4);
        offset += 4;

        /* 写入文件序号（从 0 开始，与数组索引一致） */
        encode_uint32(i, out, &offset);

        /* 写入文件类型（PRECOMPILED_FILE_TYPE_INTERFACE 或 PRECOMPILED_FILE_TYPE_MODULE） */
        encode_uint32(file->type, out, &offset);
        /* 写入属性位掩码（标记该文件是否有分离签名、链接模块 CRC、嵌入签名等） */
        encode_uint32(file->attributes, out, &offset);

        /* 写入文件名：4 字节长度 + 文件名字符串 */
        encode_uint32(name_len, out, &offset);
        memcpy(&(out[offset]), file->name, name_len);
        offset += name_len;

        /* 写入链接模块名：4 字节长度 + 名称字符串（模块类型时为空字符串） */
        encode_uint32(linked_module_name_len, out, &offset);
        memcpy(&(out[offset]), file->linked_module_name, linked_module_name_len);
        offset += linked_module_name_len;

        /* 写入核心目标文件名：4 字节长度 + 名称字符串（模块类型时为空字符串） */
        encode_uint32(core_object_name_len, out, &offset);
        memcpy(&(out[offset]), file->core_object_name, core_object_name_len);
        offset += core_object_name_len;

        /* 写入目标安装目录名：4 字节长度 + 目录名字符串 */
        encode_uint32(target_directory_len, out, &offset);
        memcpy(&(out[offset]), file->target_directory, target_directory_len);
        offset += target_directory_len;

        /* 写入文件数据的 CRC32 校验和 */
        encode_uint32(file->crc, out, &offset);

        /* 写入文件数据：4 字节大小 + 实际文件数据 */
        encode_uint32(file->size, out, &offset);
        memcpy(&(out[offset]), file->data, file->size);
        offset += file->size;

        /*
         * 写入冗余的 CRC32 校验和（与上面的 crc 值相同）。
         * 冗余存储用于在解析时进行一致性校验，检测包是否损坏。
         */
        encode_uint32(file->crc, out, &offset);

        /*
         * 写入链接模块的 CRC32（仅当属性包含 PRECOMPILED_FILE_HAS_LINKED_MODULE_CRC
         * 时有实际意义，否则值可能为 0 或无意义）
         */
        encode_uint32(file->linked_module_crc, out, &offset);

        /*
         * 写入分离签名：4 字节签名大小 + 签名数据。
         * 如果没有分离签名，signature_size 为 0，不写入签名数据。
         */
        encode_uint32(file->signature_size, out, &offset);
        if (file->signature_size) {
            memcpy(&(out[offset]), file->signature, file->signature_size);
            offset += file->signature_size;
        }

        /*
         * 写入冗余的文件序号（与文件条目开头的序号相同）。
         * 用于在解析时验证文件条目的完整性。
         */
        encode_uint32(i, out, &offset);

        /* 写入 4 字节的文件条目尾标记 "END." */
        memcpy(&(out[offset]), PRECOMPILED_FILE_FOOTER, 4);
        offset += 4;
    }

    /* 解除 mmap 映射（数据将被刷新到磁盘） */

    munmap(out, total_len);

    /* 关闭文件描述符 */
    close(fd);

    return TRUE;

}



/*
 * free_precompiled() - 释放 PrecompiledInfo 结构体及其所有子资源
 *
 * 参数：
 *   info - 指向要释放的 PrecompiledInfo 结构体的指针，可以为 NULL
 *
 * 处理流程：
 *   1. 空指针检查（如果 info 为 NULL 则直接返回）
 *   2. 释放描述字符串、proc version 字符串和版本字符串
 *   3. 遍历所有文件条目，逐一释放每个文件的数据
 *   4. 释放文件信息数组本身
 *   5. 最后释放 PrecompiledInfo 结构体本身
 */
void free_precompiled(PrecompiledInfo *info)
{
    int i;

    if (!info) {
        return;
    }

    nvfree(info->description);
    nvfree(info->proc_version_string);
    nvfree(info->version);

    /* 释放每个文件条目中动态分配的数据 */
    for (i = 0; i < info->num_files; i++) {
        free_precompiled_file_data(info->files[i]);
    }
    /* 释放文件信息数组 */
    nvfree(info->files);

    /* 释放 PrecompiledInfo 结构体本身 */
    nvfree(info);
}



/*
 * free_precompiled_file_data() - 释放单个 PrecompiledFileInfo 中动态分配的字段
 *
 * 参数：
 *   fileInfo - PrecompiledFileInfo 结构体（注意是值传递，不是指针）
 *
 * 处理流程：
 *   释放以下动态分配的字段：
 *   - name: 文件名
 *   - linked_module_name: 链接模块名
 *   - data: 文件数据
 *   - signature: 分离签名数据
 *   - target_directory: 目标安装目录
 *
 * 注意：这里没有释放 core_object_name，这可能是有意为之（该字段可能在某些情况下
 * 不是动态分配的），也可能是一个遗漏。[不确定]
 */

void free_precompiled_file_data(PrecompiledFileInfo fileInfo)
{
    nvfree(fileInfo.name);
    nvfree(fileInfo.linked_module_name);
    nvfree(fileInfo.data);
    nvfree(fileInfo.signature);
    nvfree(fileInfo.target_directory);
}



/*
 * precompiled_read_file() - 从磁盘读取一个文件，将其内容和元数据填充到
 * PrecompiledFileInfo 结构体中（内部辅助函数）
 *
 * 参数：
 *   fileInfo           - 指向要填充的 PrecompiledFileInfo 结构体的指针
 *   filename           - 要读取的文件路径
 *   linked_module_name - 链接模块名（对于接口类型文件，这是对应的内核模块名；
 *                        对于模块类型文件，传入空字符串 ""）
 *   core_object_name   - 核心目标文件名（对于接口类型文件，这是需要链接的
 *                        核心对象文件名；对于模块类型文件，传入空字符串 ""）
 *   target_directory   - 目标安装目录路径
 *   type               - 文件类型（PRECOMPILED_FILE_TYPE_INTERFACE 或
 *                        PRECOMPILED_FILE_TYPE_MODULE）
 *
 * 返回值：
 *   成功返回 TRUE（实际为 1），失败返回 FALSE（实际为 0）
 *
 * 处理流程：
 *   1. 以只读方式打开文件
 *   2. 通过 fstat 获取文件大小
 *   3. 分配缓冲区并读取整个文件内容
 *   4. 验证实际读取的字节数与文件大小一致
 *   5. 设置文件类型、名称、关联模块信息等元数据
 *   6. 计算文件的 CRC32 校验和
 */

static int precompiled_read_file(PrecompiledFileInfo *fileInfo,
                                 const char *filename,
                                 const char *linked_module_name,
                                 const char *core_object_name,
                                 const char *target_directory,
                                 uint32 type)
{
    int fd;
    struct stat st;
    int success = FALSE, ret;

    /* 以只读方式打开文件 */
    fd = open(filename, O_RDONLY);
    if (fd == -1) {
        goto done;
    }

    /* 获取文件状态信息，主要是获取文件大小 */
    if (fstat(fd, &st) != 0) {
        goto done;
    }

    /* 分配与文件大小相同的缓冲区，读取整个文件内容 */
    fileInfo->size = st.st_size;
    fileInfo->data = nvalloc(fileInfo->size);

    ret = read(fd, fileInfo->data, fileInfo->size);

    /* 验证实际读取的字节数是否与文件大小一致 */
    if (ret != fileInfo->size) {
        goto done;
    }

    /* 设置文件元数据 */
    fileInfo->type = type;
    fileInfo->name = nv_basename(filename);  /* 提取文件名部分（去掉路径） */
    fileInfo->linked_module_name = nvstrdup(linked_module_name);  /* 复制链接模块名 */
    fileInfo->core_object_name = nvstrdup(core_object_name);      /* 复制核心目标文件名 */
    fileInfo->target_directory = nvstrdup(target_directory);       /* 复制目标目录 */
    fileInfo->crc = compute_crc(NULL, filename);  /* 计算文件的 CRC32 校验和 */

    success = TRUE;

done:
    close(fd);
    return success;
}


/*
 * precompiled_read_interface() - 读取一个预编译内核接口文件（.o 文件）
 *
 * 参数：
 *   fileInfo           - 指向要填充的 PrecompiledFileInfo 结构体的指针
 *   filename           - 接口文件路径
 *   linked_module_name - 该接口需要链接到的内核模块名称
 *   core_object_name   - 需要与该接口链接的核心目标文件名
 *   target_directory   - 目标安装目录
 *
 * 返回值：
 *   成功返回 TRUE，失败返回 FALSE
 *
 * 本函数是 precompiled_read_file() 的封装，将文件类型设置为
 * PRECOMPILED_FILE_TYPE_INTERFACE。
 */
int precompiled_read_interface(PrecompiledFileInfo *fileInfo,
                               const char *filename,
                               const char *linked_module_name,
                               const char *core_object_name,
                               const char *target_directory)
{
    return precompiled_read_file(fileInfo, filename, linked_module_name,
                                 core_object_name, target_directory,
                                 PRECOMPILED_FILE_TYPE_INTERFACE);
}

/*
 * precompiled_read_module() - 读取一个预编译内核模块文件（.ko 文件）
 *
 * 参数：
 *   fileInfo         - 指向要填充的 PrecompiledFileInfo 结构体的指针
 *   filename         - 模块文件路径
 *   target_directory - 目标安装目录
 *
 * 返回值：
 *   成功返回 TRUE，失败返回 FALSE
 *
 * 本函数是 precompiled_read_file() 的封装，将文件类型设置为
 * PRECOMPILED_FILE_TYPE_MODULE。由于模块类型不需要链接模块名和核心目标文件名，
 * 这两个参数传入空字符串 ""。
 */
int precompiled_read_module(PrecompiledFileInfo *fileInfo, const char *filename,
                            const char *target_directory)
{
    return precompiled_read_file(fileInfo, filename, "", "", target_directory,
                                 PRECOMPILED_FILE_TYPE_MODULE);
}



/*
 * precompiled_read_fileinfo() - 从预编译包的二进制缓冲区中解析单个文件条目
 *
 * 这是解析预编译包文件格式的核心函数，负责从原始字节流中提取一个完整的
 * 文件条目（包括头标记、元数据、文件数据、签名、尾标记等）。
 *
 * 参数：
 *   op        - 选项结构体指针，用于调用 ui_log() 等界面函数
 *   fileInfos - PrecompiledFileInfo 数组，解析结果将存储在 fileInfos[index] 中
 *   index     - 当前要解析的文件在数组中的索引（从 0 开始），
 *               同时用于验证文件条目中记录的序号是否一致
 *   buf       - 指向整个预编译包文件内容的缓冲区（mmap 得到的）
 *   offset    - 当前文件条目在 buf 中的起始偏移量
 *   size      - 整个缓冲区的大小（用于边界检查，防止越界读取）
 *
 * 返回值：
 *   成功时返回该文件条目占用的总字节数（>0），调用者用这个值推进偏移量；
 *   失败时返回 -1。
 *
 * 处理流程（按照二进制格式顺序）：
 *   1. 检查剩余空间是否足够容纳一个文件条目的固定长度部分
 *   2. 验证文件条目头标记 "FILE"
 *   3. 读取并验证文件序号
 *   4. 读取文件类型和属性掩码
 *   5. 读取文件名（长度 + 内容）
 *   6. 读取链接模块名（长度 + 内容）
 *   7. 读取核心目标文件名（长度 + 内容）
 *   8. 读取目标安装目录（长度 + 内容）
 *   9. 读取 CRC 校验和
 *  10. 读取文件数据（大小 + 内容）
 *  11. 读取并验证冗余 CRC（必须与第一个 CRC 一致）
 *  12. 计算数据的实际 CRC 并与记录值比较（仅记录警告，不返回失败）
 *  13. 读取链接模块 CRC
 *  14. 读取分离签名（大小 + 内容）
 *  15. 读取并验证冗余文件序号
 *  16. 验证文件条目尾标记 "END."
 */

static int precompiled_read_fileinfo(Options *op, PrecompiledFileInfo *fileInfos,
                                     int index, char *buf, int offset, int size)
{
    PrecompiledFileInfo *fileInfo = fileInfos + index;  /* 指向目标数组元素 */
    uint32 val;
    int oldoffset = offset;  /* 记录初始偏移量，用于计算返回值（读取的总字节数） */

    /*
     * 基本长度检查：缓冲区剩余空间必须至少能容纳一个文件条目的固定长度部分。
     * 注意这只是最低要求，实际条目可能更长（包含可变长度的文件名、数据等）。
     */
    if (size - offset < PRECOMPILED_FILE_CONSTANT_LENGTH) {
        return -1;
    }

    /* 验证文件条目头标记："FILE"（4 字节） */
    if (strncmp(buf + offset, PRECOMPILED_FILE_HEADER, 4) != 0) {
        ui_log(op, "Unrecognized header for packaged file.");
        return -1;
    }
    offset += 4;

    /*
     * 读取文件序号并验证其与期望的索引 index 一致。
     * 这是包完整性检查的一部分。
     */
    val = read_uint32(buf, &offset);
    if (val != index) {
        ui_log(op, "Invalid file index %d; expected %d.", val, index);
        return -1;
    }

    /* 读取文件类型（0=接口，1=模块）和属性位掩码 */
    fileInfo->type = read_uint32(buf, &offset);
    fileInfo->attributes = read_uint32(buf, &offset);

    /* 读取文件名：4 字节长度 + 文件名字符串 */
    val = read_uint32(buf, &offset);
    if (offset + val > size) {
        ui_log(op, "Bad filename length.");
        return -1;
    }

    fileInfo->name = nvalloc(val + 1);  /* +1 用于 '\0' 终止符 */
    memcpy(fileInfo->name, buf + offset, val);
    offset += val;

    /* 读取链接模块名：4 字节长度 + 字符串 */
    val = read_uint32(buf, &offset);
    if (offset + val > size) {
        ui_log(op, "Bad linked module name length.");
        return -1;
    }

    fileInfo->linked_module_name = nvalloc(val + 1);
    memcpy(fileInfo->linked_module_name, buf + offset, val);
    offset += val;

    /* 读取核心目标文件名：4 字节长度 + 字符串 */
    val = read_uint32(buf, &offset);
    if (offset + val > size) {
        ui_log(op, "Bad core object file name length.");
        return -1;
    }

    fileInfo->core_object_name = nvalloc(val + 1);
    memcpy(fileInfo->core_object_name, buf + offset, val);
    offset += val;

    /* 读取目标安装目录名：4 字节长度 + 字符串 */
    val = read_uint32(buf, &offset);
    if (offset + val > size) {
        ui_log(op, "Bad target directory name length.");
        return -1;
    }

    fileInfo->target_directory = nvalloc(val + 1);
    memcpy(fileInfo->target_directory, buf + offset, val);
    offset += val;

    /* 读取文件数据的 CRC32 校验和 */
    fileInfo->crc = read_uint32(buf, &offset);

    /* 读取文件数据：4 字节大小 + 实际数据内容 */
    fileInfo->size = read_uint32(buf, &offset);
    if (offset + fileInfo->size > size) {
        ui_log(op, "Bad file length.");
        return -1;
    }

    /* 分配缓冲区并复制文件数据 */
    fileInfo->data = nvalloc(fileInfo->size);
    memcpy(fileInfo->data, buf + offset, fileInfo->size);
    offset += fileInfo->size;

    /*
     * 读取冗余的 CRC32 校验和，与前面读取的 CRC 进行比较。
     * 如果两个 CRC 值不一致，说明文件可能已损坏。
     */
    val = read_uint32(buf, &offset);
    if (val != fileInfo->crc) {
        ui_log(op, "The redundant stored CRC values %" PRIu32 " and %" PRIu32
               " disagree with each other; the file may be corrupted.",
               fileInfo->crc, val);
        return -1;
    }

    /*
     * 使用实际文件数据重新计算 CRC32，与包中记录的 CRC 进行比较。
     * 注意：即使 CRC 不匹配，这里也只是记录警告，不返回失败。
     * 这与上面冗余 CRC 不匹配（返回 -1）的处理方式不同。[不确定：是否有意为之]
     */
    val = compute_crc_from_buffer(fileInfo->data, fileInfo->size);
    if (val != fileInfo->crc) {
        ui_log(op, "The CRC for the file '%s' (%" PRIu32 ") does not match the "
               "expected value (%" PRIu32 ").", fileInfo->name, val,
               fileInfo->crc);
    }

    /* 读取链接模块的 CRC32 */
    fileInfo->linked_module_crc = read_uint32(buf, &offset);

    /* 读取分离签名：4 字节大小 + 签名数据（如果大小 > 0） */
    fileInfo->signature_size = read_uint32(buf, &offset);
    if (fileInfo->signature_size) {
        if (offset + fileInfo->signature_size > size) {
            ui_log(op, "Bad signature size");
            return -1;
        }
        fileInfo->signature = nvalloc(fileInfo->signature_size);
        memcpy(fileInfo->signature, buf + offset, fileInfo->signature_size);
        offset += fileInfo->signature_size;
    }

    /*
     * 读取冗余的文件序号并验证其与期望的 index 一致。
     * 这是文件条目完整性检查的第二个冗余校验点。
     */
    val = read_uint32(buf, &offset);
    if (val != index) {
        ui_log(op, "Invalid file index %d; expected %d.", val, index);
        return -1;
    }

    /* 验证文件条目尾标记："END."（4 字节） */
    if (strncmp(buf + offset, PRECOMPILED_FILE_FOOTER, 4) != 0) {
        ui_log(op, "Unrecognized footer for packaged file.");
        return -1;
    }
    offset += 4;

    /* 返回本次读取的总字节数 */
    return offset - oldoffset;
}


/*
 * precompiled_find_file() - 在已解析的预编译包中按文件名查找文件
 *
 * 参数：
 *   info - 指向已解析的 PrecompiledInfo 结构体的指针
 *   file - 要查找的文件名
 *
 * 返回值：
 *   如果找到匹配的文件，返回指向对应 PrecompiledFileInfo 的指针；
 *   如果未找到，返回 NULL。
 *   返回的指针指向 info->files 数组中的元素，调用者不应释放它。
 *
 * 处理流程：
 *   线性遍历 info->files 数组，使用 strcmp 进行精确文件名匹配。
 */

PrecompiledFileInfo *precompiled_find_file(const PrecompiledInfo *info,
                                           const char *file)
{
    int i;

    for (i = 0; i < info->num_files; i++) {
        if (strcmp(file, info->files[i].name) == 0) {
            return info->files + i;
        }
    }

    return NULL;
}

/*
 * precompiled_append_files() - 向已有的 PrecompiledInfo 追加额外的文件条目
 *
 * 参数：
 *   info      - 指向要追加文件的 PrecompiledInfo 结构体的指针
 *   files     - 要追加的 PrecompiledFileInfo 数组
 *   num_files - 要追加的文件数量
 *
 * 处理流程：
 *   1. 使用 nvrealloc 扩展 info->files 数组，使其能容纳原有文件 + 新文件
 *   2. 使用 memcpy 将新文件条目复制到数组末尾
 *   3. 更新 info->num_files 为新的总文件数
 *
 * 注意：此函数进行的是浅拷贝（memcpy），追加后 files 数组中的指针
 * （如 name、data 等）的所有权转移到 info->files 中。
 * 调用者不应再释放 files 中各元素的指针字段。
 */

void precompiled_append_files(PrecompiledInfo *info, PrecompiledFileInfo *files,
                              int num_files)
{
    /* 扩展数组以容纳新增的文件条目 */
    info->files = nvrealloc(info->files, (info->num_files + num_files) *
                            sizeof(PrecompiledFileInfo));
    /* 将新文件条目复制到数组末尾 */
    memcpy(info->files + info->num_files, files,
           num_files * sizeof(PrecompiledFileInfo));
    /* 更新文件总数 */
    info->num_files += num_files;
}

/*
 * precompiled_file_type_name() - 返回文件类型的人类可读名称字符串
 *
 * 参数：
 *   file_type - 文件类型值（PRECOMPILED_FILE_TYPE_INTERFACE 或
 *               PRECOMPILED_FILE_TYPE_MODULE）
 *
 * 返回值：
 *   指向静态字符串的指针（调用者不应释放）：
 *   - 类型 0 -> "precompiled kernel interface"
 *   - 类型 1 -> "precompiled kernel module"
 *   - 其他值 -> "unknown file type"
 */
const char *precompiled_file_type_name(uint32 file_type)
{
    /* 文件类型名称数组，索引对应文件类型枚举值 */
    static const char *file_type_names[] = {
                                               "precompiled kernel interface",
                                               "precompiled kernel module",
                                           };

    /* 如果类型值超出已知范围，返回 "unknown file type" */
    if (file_type >= ARRAY_LEN(file_type_names)) {
        return "unknown file type";
    }

    return file_type_names[file_type];
}

/*
 * precompiled_file_attribute_names() - 将属性位掩码转换为人类可读的属性名称列表
 *
 * 参数：
 *   attribute_mask - 属性位掩码（每一位代表一个属性）
 *                    位 0: 有分离签名（detached signature）
 *                    位 1: 有链接模块 CRC（linked module crc）
 *                    位 2: 有嵌入签名（embedded signature）
 *
 * 返回值：
 *   以 NULL 结尾的字符串指针数组。数组本身是新分配的（调用者应释放），
 *   但数组中的各个字符串指针指向静态存储，不应被释放。
 *
 * 处理流程：
 *   遍历 attribute_mask 的每一位（最多 32 位，因为 uint32 是 32 位），
 *   对于每个被设置的位，将对应的属性名称添加到结果数组中。
 *   如果某一位被设置但超出了已知属性名称的范围，则添加 "unknown attribute"。
 */
const char **precompiled_file_attribute_names(uint32 attribute_mask)
{
    const char **ret;
    int i, attr = 0;

    /* 已知的属性名称数组，索引对应位位置 */
    static const char *file_attribute_names[] = {
                                                    "detached signature",
                                                    "linked module crc",
                                                    "embedded signature",
                                                };
    static const char *unknown_attribute = "unknown attribute";

    /* attribute_mask 是 uint32，最多有 32 位 */
    const int max_file_attribute_names = sizeof(attribute_mask) * 8;

    /* 分配结果数组，+1 为 NULL 终止符留出空间 */
    ret = nvalloc((max_file_attribute_names + 1) * sizeof(char *));

    /* 遍历每一位，收集被设置的属性名称 */
    for (i = 0; i < max_file_attribute_names; i++) {
        if (attribute_mask & (1 << i)) {
            if (i >= ARRAY_LEN(file_attribute_names)) {
                /* 未知属性位 */
                ret[attr++] = unknown_attribute;
            } else {
                ret[attr++] = file_attribute_names[i];
            }
        }
    }
    ret[attr] = NULL;  /* NULL 终止 */

    return ret;
}



/*
 * byte_tail() - 从文件的指定字节偏移位置读取到文件末尾
 *
 * 参数：
 *   infile - 输入文件的路径
 *   start  - 起始字节偏移位置（从文件开头计算）
 *   buf    - 指向输出缓冲区指针的指针。成功时 *buf 指向新分配的缓冲区，
 *            失败时 *buf 被设置为 NULL。调用者负责释放 *buf。
 *
 * 返回值：
 *   成功时返回读取的字节数（即从 start 到文件末尾的长度）；
 *   失败时返回 0（注意：如果文件从 start 开始恰好为空也会返回 0，
 *   但此时 *buf 不为 NULL）。[不确定：size 为 0 时的行为可能存在歧义]
 *
 * 处理流程：
 *   1. 以读取方式打开文件
 *   2. 使用 fseek(SEEK_END) 定位到文件末尾，获取文件总长度
 *   3. 使用 fseek(SEEK_SET) 定位到 start 位置
 *   4. 计算需要读取的字节数（文件总长度 - start）
 *   5. 分配缓冲区并使用 fread 读取数据
 *   6. 验证实际读取的字节数正确，且已到达文件末尾
 *
 * 注意：这个函数的存在是因为 `tail -c` 命令在某些系统实现中不可靠，
 * 所以需要自行实现类似功能。
 */
int byte_tail(const char *infile, int start, char **buf)
{
    FILE *in = NULL;
    int ret, end, size = 0;

    /* 以只读方式打开输入文件 */
    in = fopen(infile, "r");

    if (!in) {
	goto done;
    }

    /* 定位到文件末尾，获取文件总长度 */
    ret = fseek(in, 0, SEEK_END);
    if (ret != 0) {
	goto done;
    }
    end = ftell(in);  /* 获取当前位置（即文件总字节数） */

    /* 定位到指定的起始偏移位置 */
    ret = fseek(in, start, SEEK_SET);
    if (ret != 0) {
	goto done;
    }

    /* 计算需要读取的字节数 */
    size = end - start;
    *buf = nvalloc(size);

    /*
     * 读取数据：请求读取 size + 1 字节。
     * 因为我们已经知道文件从 start 到末尾只有 size 字节，
     * 所以多请求 1 字节是为了确认 fread 确实到达了文件末尾（EOF）。
     * 如果实际读取的字节数不等于 size，或者发生了错误，或者没有到达 EOF，
     * 则认为读取失败。
     */
    ret = (fread(*buf, 1, size + 1, in));
    if (ret != size || ferror(in) || !feof(in)) {
	nvfree(*buf);
	*buf = NULL;
	goto done;
    }

done:
    if (in) {
        fclose(in);
    }
    return size;
}
