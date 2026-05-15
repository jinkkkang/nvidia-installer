/*
 * Copyright (C) 2010-2015 NVIDIA Corporation
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
 * 文件说明: common-utils.h
 *
 * 本头文件声明了 NVIDIA 安装程序中使用的通用工具函数和宏定义，包括：
 *
 * 1. 基本常量和宏：
 *    - TRUE / FALSE         : 布尔值常量
 *    - ARRAY_LEN()          : 计算数组元素个数
 *    - NV_MIN() / NV_MAX()  : 最小值/最大值宏
 *    - TAB / BIGTAB         : 用于文本格式化的缩进字符串
 *
 * 2. 内存管理函数声明：
 *    - nvalloc / nvrealloc / nvfree : 带错误检查的内存分配/释放
 *    - nvstrdup / nvstrndup         : 带错误检查的字符串复制
 *
 * 3. 字符串处理函数声明：
 *    - nvstrcat / nvvstrcat         : 可变参数字符串拼接
 *    - nvstrtolower / nvstrtoupper  : 大小写转换
 *    - nvasprintf / nv_append_sprintf : 格式化字符串构建
 *    - nv_trim_space / nv_trim_char : 字符串修剪
 *
 * 4. 文件和路径操作函数声明：
 *    - nv_open / nv_mmap / nv_get_file_length 等文件操作
 *    - nv_basename / nv_dirname / nvdircat 等路径处理
 *    - nv_mkdir_recursive / nv_string_to_file 等高级文件操作
 *
 * 5. 版本号编码工具：
 *    - nv_encode_version() : 将多段版本号编码为 64 位整数以便比较
 *    - NV_VERSION2/3/4     : 版本号编码的便捷宏
 *
 * 6. 可选布尔类型 NVOptionalBool：
 *    - 支持"未设置"状态的三值布尔枚举
 */

#ifndef __COMMON_UTILS_H__
#define __COMMON_UTILS_H__

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <stdint.h>
#include <version.h>

#include "msg.h"

/* 布尔值常量定义 */
#if !defined(TRUE)
#define TRUE 1
#endif

#if !defined(FALSE)
#define FALSE 0
#endif

/*
 * ARRAY_LEN() - 计算静态数组的元素个数。
 * 注意：此宏只能用于真正的数组，不能用于指针。
 */
#define ARRAY_LEN(_arr) (sizeof(_arr) / sizeof(_arr[0]))

/* NV_MIN / NV_MAX - 求最小值和最大值的宏 */
#ifndef NV_MIN
#define NV_MIN(x,y) ((x) < (y) ? (x) : (y))
#endif
#ifndef NV_MAX
#define NV_MAX(x,y) ((x) > (y) ? (x) : (y))
#endif

/*
 * TAB / BIGTAB - 用于帮助信息等文本输出中的缩进字符串。
 * TAB 为 2 个空格，BIGTAB 为 6 个空格。
 */
#define TAB "  "
#define BIGTAB "      "

/*
 * 内存分配和字符串操作函数声明
 * 所有分配函数在失败时都会打印错误并 exit(1)
 */
void *nvalloc(size_t size);                 /* 带错误检查的 calloc 封装 */
char *nvstrcat(const char *str, ...);       /* 可变参数字符串拼接（NULL 结尾） */
char *nvvstrcat(const char *str, va_list ap); /* nvstrcat 的 va_list 版本 */
void *nvrealloc(void *ptr, size_t size);    /* 带错误检查的 realloc 封装 */
char *nvstrdup(const char *s);              /* 带错误检查的 strdup 封装 */
char *nvstrndup(const char *s, size_t n);   /* 带错误检查的 strndup 实现 */
char *nvstrtolower(char *s);                /* 字符串原地转小写 */
char *nvstrtoupper(char *s);                /* 字符串原地转大写 */
char *nvstrchrnul(char *s, int c);          /* 类似 strchr，未找到时返回末尾指针而非 NULL */
char *nvasprintf(const char *fmt, ...) NV_ATTRIBUTE_PRINTF(1, 2); /* 格式化字符串分配 */
void nv_append_sprintf(char **buf, const char *fmt, ...) NV_ATTRIBUTE_PRINTF(2, 3); /* 追加格式化字符串 */
void nvfree(void *s);                       /* 安全的 free 封装（允许 NULL） */

/* 路径和字符串列表操作 */
char *tilde_expansion(const char *str);     /* 波浪号路径展开（~ -> 主目录） */
char *nv_prepend_to_string_list(char *list, const char *item, const char *delim); /* 列表前置插入 */

/* 文件流逐行读取 */
char *fget_next_line(FILE *fp, int *eof);   /* 从文件流读取一行 */

/* 文件操作函数 */
int nv_open(const char *pathname, int flags, mode_t mode); /* 带错误检查的 open 封装 */
int nv_get_file_length(const char *filename);              /* 获取文件大小 */
void nv_set_file_length(const char *filename, int fd, int len); /* 设置文件大小 */
void *nv_mmap(const char *filename, size_t len, int prot, int flags, int fd); /* 带错误检查的 mmap 封装 */
char *nv_basename(const char *path);        /* 提取文件名（跨平台 basename） */
int nv_mkdir_recursive(const char *path, const mode_t mode,
                       char **error_str, char **log_str); /* 递归创建目录 */
char *nvdircat(const char *str, ...);       /* 路径拼接（自动插入 /） */
char *nv_dirname(const char *path);         /* 提取目录路径（跨平台 dirname） */
int nv_string_to_file(const char *, const char *); /* 将字符串写入文件 */

/* 字符串修剪函数 */
char *nv_trim_space(char *string);          /* 去除首尾空白 */
char *nv_trim_char(char *string, char trim); /* 去除首尾指定字符 */
char *nv_trim_char_strict(char *string, char trim); /* 严格去除首尾字符（必须配对） */
void remove_trailing_slashes(char *string); /* 移除尾部斜杠 */
void collapse_multiple_slashes(char *s);    /* 合并连续斜杠 */

/* 目录存在性检查 */
int directory_exists(const char *dir);

/*
 * NV_INLINE 宏 - 为不同编译器提供内联函数支持。
 * GCC 使用 __inline__，其他编译器可能不支持内联。
 */
#if defined(__GNUC__)
# define NV_INLINE __inline__
#else
# define NV_INLINE
#endif

/*
 * nv_encode_version() - 将多段版本号编码为 64 位无符号整数。
 *
 * 功能：将 major.minor.micro.nano 格式的版本号编码到一个 uint64_t 中，
 *       使得编码后的版本号可以直接用 < > == 等运算符比较。
 *
 * 编码布局（从低位到高位）：
 *   - 位 0-15  : nano  (最低 16 位)
 *   - 位 16-31 : micro
 *   - 位 32-47 : minor
 *   - 位 48-63 : major (最高 16 位)
 *
 * 参数：
 *   major - 主版本号（0-65535）
 *   minor - 次版本号（0-65535）
 *   micro - 微版本号（0-65535）
 *   nano  - 纳版本号（0-65535）
 *
 * 返回值：
 *   编码后的 64 位版本号
 *
 * 注意：定义为内联函数而非宏，便于调试器检查
 */
static NV_INLINE uint64_t nv_encode_version(unsigned int major,
                                            unsigned int minor,
                                            unsigned int micro,
                                            unsigned int nano)
{
    return (((uint64_t)(nano  & 0xFFFF)) |
           (((uint64_t)(micro & 0xFFFF)) << 16) |
           (((uint64_t)(minor & 0xFFFF)) << 32) |
           (((uint64_t)(major & 0xFFFF)) << 48));
}

/*
 * nv_encode_version() 的便捷封装宏。
 * NV_VERSIONK() 接受 K 段版本号（缺少的部分补零）。
 */
#define NV_VERSION2(major, minor)              \
    nv_encode_version(major, minor, 0, 0)
#define NV_VERSION3(major, minor, micro)       \
    nv_encode_version(major, minor, micro, 0)
#define NV_VERSION4(major, minor, micro, nano) \
    nv_encode_version(major, minor, micro, nano)

/*
 * NVOptionalBool - 可选布尔枚举类型
 *
 * 用途：表示一个可能被设置也可能未被设置的布尔值（三值逻辑）。
 *
 * 枚举值：
 *   NV_OPTIONAL_BOOL_DEFAULT (-1) : 默认/未设置状态
 *   NV_OPTIONAL_BOOL_FALSE   (0)  : 明确设置为假
 *   NV_OPTIONAL_BOOL_TRUE    (1)  : 明确设置为真
 *
 * 重要注意事项：
 *   - 不能简单地用 if(value) 来测试，因为 DEFAULT(-1) 也会被视为真
 *   - 使用者必须在任何可能设置值的代码路径执行之前，
 *     将变量无条件初始化为 NV_OPTIONAL_BOOL_DEFAULT
 */

typedef enum {
    NV_OPTIONAL_BOOL_DEFAULT = -1,
    NV_OPTIONAL_BOOL_FALSE   = FALSE,
    NV_OPTIONAL_BOOL_TRUE    = TRUE
} NVOptionalBool;

/*
 * NV_ID_STRING - 程序标识字符串宏。
 * 格式为 "程序名:  version 版本号"，例如 "nvidia-installer:  version 580.105.08"
 * PROGRAM_NAME 和 NVIDIA_VERSION 在编译时通过 Makefile 定义。
 */
#define NV_ID_STRING PROGRAM_NAME ":  version " NVIDIA_VERSION

#endif /* __COMMON_UTILS_H__ */
