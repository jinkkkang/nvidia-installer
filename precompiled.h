/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003-2013 NVIDIA Corporation
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
 * precompiled.h: mkprecompiled 工具和 nvidia-installer 共用的头文件。
 * 定义了预编译内核接口/模块包的二进制格式。
 *
 * 【预编译包二进制格式详解】
 *
 * 预编译包用于分发已编译好的内核接口文件，避免用户端重新编译。
 * 包通过 /proc/version 字符串和 CRC 匹配当前运行的内核。
 *
 * 包头部格式：
 *   前 8 字节：魔数 "\aNVIDIA\a"（用于识别文件类型）
 *   接下来 4 字节（无符号）：包格式版本号
 *   接下来 4 字节（无符号）：驱动版本字符串长度 (v)
 *   接下来 v 字节：驱动版本字符串
 *   接下来 4 字节（无符号）：描述字符串长度 (d)
 *   接下来 d 字节：描述字符串
 *   接下来 4 字节（无符号）：/proc/version 字符串长度 (p)
 *   接下来 p 字节：/proc/version 字符串（用于内核匹配）
 *   接下来 4 字节（无符号）：包中的文件数量 (f)
 *
 * 每个文件条目格式：
 *   4 字节：文件头标记 "FILE"
 *   4 字节（无符号）：文件序号（从 0 开始）
 *   4 字节（无符号）：文件类型（0=预编译接口，1=预编译内核模块）
 *   4 字节：属性掩码（1=有分离签名，2=有链接模块 CRC，4=有嵌入签名）
 *   4 字节（无符号）：文件名长度 (n)
 *   n 字节：文件名
 *   4 字节（无符号）：链接模块名长度 (m)
 *   m 字节：链接模块名（仅限内核接口）
 *   4 字节（无符号）：核心目标文件名长度 (o)
 *   o 字节：核心目标文件名（仅限内核接口）
 *   4 字节（无符号）：文件 CRC 校验和
 *   4 字节（无符号）：文件大小 (l)
 *   l 字节：文件数据
 *   4 字节（无符号）：文件 CRC 校验和（冗余校验）
 *   4 字节（无符号）：链接模块 CRC（仅当属性包含"有链接模块 CRC"时有意义）
 *   4 字节（无符号）：分离签名长度 (s)（无签名时为 0）
 *   s 字节：分离签名数据
 *   4 字节（无符号）：文件序号（冗余校验）
 *   4 字节：文件尾标记 "END."
 */

#ifndef __NVIDIA_INSTALLER_PRECOMPILED_H__
#define __NVIDIA_INSTALLER_PRECOMPILED_H__

/* 包头部固定长度（不含可变长度字段） */
#define PRECOMPILED_PKG_CONSTANT_LENGTH (8 + /* 包头魔数 "\aNVIDIA\a" */ \
                                         4 + /* 包格式版本号 */ \
                                         4 + /* 驱动版本字符串长度 */ \
                                         4 + /* 描述字符串长度 */ \
                                         4 + /* proc version 字符串长度 */ \
                                         4)  /* 文件数量 */

#define PRECOMPILED_PKG_HEADER "\aNVIDIA\a"  /* 包头魔数标识 */

#define PRECOMPILED_PKG_VERSION 2  /* 当前包格式版本 */

/* 每个文件条目的固定长度（不含可变长度字段的文件名、数据等） */
#define PRECOMPILED_FILE_CONSTANT_LENGTH (4 + /* 文件头标记 "FILE" */ \
                                          4 + /* 文件序号 */ \
                                          4 + /* 文件类型 */ \
                                          4 + /* 属性掩码 */ \
                                          4 + /* 文件名长度 */ \
                                          4 + /* 链接模块名长度 */ \
                                          4 + /* 核心目标文件名长度 */ \
                                          4 + /* 目标目录名长度 */ \
                                          4 + /* 文件 CRC */ \
                                          4 + /* 文件大小 */ \
                                          4 + /* 冗余文件 CRC */ \
                                          4 + /* 链接模块 CRC */ \
                                          4 + /* 分离签名长度 */ \
                                          4 + /* 冗余文件序号 */ \
                                          4)  /* 文件尾标记 "END." */

#define PRECOMPILED_FILE_HEADER "FILE"  /* 文件条目头标记 */
#define PRECOMPILED_FILE_FOOTER "END."  /* 文件条目尾标记 */

/* 预编译文件类型 */
enum {
    PRECOMPILED_FILE_TYPE_INTERFACE = 0,  /* 预编译内核接口（.o 文件，需与核心目标文件链接） */
    PRECOMPILED_FILE_TYPE_MODULE,         /* 预编译完整内核模块（.ko 文件，可直接安装） */
};

/* 预编译文件属性位索引（用于 PRECOMPILED_ATTR 宏） */
enum {
    PRECOMPILED_FILE_HAS_DETACHED_SIGNATURE = 0,  /* 包含分离的数字签名 */
    PRECOMPILED_FILE_HAS_LINKED_MODULE_CRC,       /* 包含链接模块的 CRC 校验和 */
    PRECOMPILED_FILE_HAS_EMBEDDED_SIGNATURE,       /* 包含嵌入式数字签名 */
};

/* 将属性枚举值转换为位掩码 */
#define PRECOMPILED_ATTR(attr) (1 << PRECOMPILED_FILE_HAS_##attr)

/*
 * PrecompiledFileInfo：预编译包中单个文件的信息。
 */
typedef struct __precompiled_file_info {
    uint32 type;                /* 文件类型（接口 or 模块） */
    uint32 attributes;          /* 属性位掩码 */
    char *name;                 /* 文件名 */
    char *linked_module_name;   /* 链接模块名（仅接口类型使用） */
    char *core_object_name;     /* 核心目标文件名（仅接口类型使用） */
    char *target_directory;     /* 目标安装目录 */
    uint32 crc;                 /* 文件数据的 CRC32 校验和 */
    uint32 size;                /* 文件数据大小（字节） */
    uint8 *data;                /* 文件数据指针 */
    uint32 linked_module_crc;   /* 链接模块的 CRC32（用于匹配验证） */
    uint32 signature_size;      /* 分离签名的大小 */
    char *signature;            /* 分离签名数据 */
} PrecompiledFileInfo;

/*
 * PrecompiledInfo：整个预编译包的信息。
 */
typedef struct __precompiled_info {

    uint32 package_size;          /* 包总大小（字节） */
    char *version;                /* 驱动版本字符串 */
    char *proc_version_string;    /* 匹配的 /proc/version 字符串 */
    char *description;            /* 包描述 */
    int num_files;                /* 包中文件数量 */
    PrecompiledFileInfo *files;   /* 文件信息数组 */

} PrecompiledInfo;


/* ===== 预编译包操作函数 ===== */

/* 读取 /proc/version 内容 */
char *read_proc_version(Options *op, const char *proc_mount_point);

/* 从文件中读取预编译包信息，验证版本和 proc version 匹配 */
PrecompiledInfo *get_precompiled_info(Options *op,
                                      const char *filename,
                                      const char *real_proc_version_string,
                                      const char *package_version,
                                      char *const *search_filelist);

/* 在预编译包中查找指定名称的文件 */
PrecompiledFileInfo *precompiled_find_file(const PrecompiledInfo *info,
                                           const char *file);

/* 将预编译包中的单个文件解压到指定目录 */
int precompiled_file_unpack(Options *op, const PrecompiledFileInfo *fileInfo,
                            const char *output_directory);
/* 将整个预编译包解压到指定文件 */
int precompiled_unpack(Options *op, const PrecompiledInfo *info,
                       const char *output_filename);

/* 将预编译信息打包写入文件 */
int precompiled_pack(const PrecompiledInfo *info, const char *package_filename);

/* 释放 PrecompiledInfo 及其所有子资源 */
void free_precompiled(PrecompiledInfo *info);
/* 释放 PrecompiledFileInfo 中的数据内存 */
void free_precompiled_file_data(PrecompiledFileInfo fileInfo);
/* 读取预编译接口文件信息 */
int precompiled_read_interface(PrecompiledFileInfo *fileInfo,
                               const char *filename,
                               const char *linked_module_name,
                               const char *core_object_name,
                               const char *target_directory);
/* 读取预编译模块文件信息 */
int precompiled_read_module(PrecompiledFileInfo *fileInfo, const char *filename,
                            const char *target_directory);
/* 向预编译包追加文件 */
void precompiled_append_files(PrecompiledInfo *info, PrecompiledFileInfo *files,
                              int num_files);

/* 获取预编译文件类型的名称字符串 */
const char *precompiled_file_type_name(uint32 file_type);
/* 获取预编译文件属性掩码对应的名称字符串数组 */
const char **precompiled_file_attribute_names(uint32 attribute_mask);

/* 从文件指定偏移位置开始读取到末尾 */
int byte_tail(const char *infile, int start, char **buf);

#endif /* __NVIDIA_INSTALLER_PRECOMPILED_H__ */
