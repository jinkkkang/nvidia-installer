/*
 * Copyright (C) 2004 NVIDIA Corporation.
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
 * 文件说明: msg.h
 *
 * 本头文件定义了消息格式化和文本换行处理的接口，包括：
 *
 * 1. NV_ATTRIBUTE_PRINTF 宏：
 *    - 为 GCC 提供 printf 格式字符串检查的属性标注
 *
 * 2. NV_VSNPRINTF 宏：
 *    - 处理不同 glibc 版本中 vsnprintf() 返回值语义差异的可移植封装
 *    - 自动分配足够大小的缓冲区来存放格式化后的字符串
 *
 * 3. NvVerbosity 冗余度枚举：
 *    - 控制消息输出级别（静默、仅错误、包含弃用、包含警告、全部）
 *
 * 4. 格式化输出函数声明：
 *    - nv_error_msg / nv_warning_msg / nv_deprecated_msg / nv_info_msg / nv_msg 等
 *
 * 5. TextRows 文本行结构和相关操作：
 *    - 用于将长文本按指定宽度分割为多行
 */

#ifndef __MSG_H__
#define __MSG_H__

#include <stdarg.h>
#include <stdio.h>


/*
 * NV_ATTRIBUTE_PRINTF 宏 - printf 格式检查属性。
 *
 * 当使用 GCC >= 2.3 编译时，此宏展开为 __attribute__((__format__(__printf__,x,y)))，
 * 使编译器检查格式字符串与参数类型是否匹配。
 *
 * 参数：
 *   x - 格式字符串参数在函数参数列表中的位置（从 1 开始）
 *   y - 可变参数（...）在函数参数列表中的起始位置
 *
 * 定义参考自 Xfuncproto.h（xproto 包）。
 */

#if defined(__GNUC__) && ((__GNUC__ * 100 + __GNUC_MINOR__) >= 203)
# define NV_ATTRIBUTE_PRINTF(x,y) __attribute__((__format__(__printf__,x,y)))
#else /* 非 gcc >= 2.3 */
# define NV_ATTRIBUTE_PRINTF(x,y)
#endif


/*
 * NV_VSNPRINTF() 宏 - 使用 vsnprintf() 将格式化字符串写入自动分配的缓冲区。
 *
 * 功能：处理 vsnprintf() 返回值在不同 glibc 版本中的差异：
 *   - glibc < 2.1：缓冲区不够大时返回 -1
 *   - glibc >= 2.1：返回格式化后字符串本应有的长度
 *
 * 处理流程：
 *   1. 分配初始大小为 NV_FMT_BUF_LEN (256) 字节的缓冲区
 *   2. 尝试使用 vsnprintf 格式化
 *   3. 如果缓冲区足够大（len < current_len），成功退出
 *   4. 如果返回值 > -1 但 >= current_len，按需扩大缓冲区（len + 1）
 *   5. 如果返回值 == -1（旧版 glibc），增加 NV_FMT_BUF_LEN 并重试
 *   6. 每次重试前释放之前的缓冲区
 *
 * 参数：
 *   buf - 输出变量，格式化后的字符串（由此宏分配，调用者负责释放）
 *   fmt - 格式字符串变量名（必须是调用函数中可变参数前的最后一个固定参数）
 *
 * 注意：此宏内部使用 nvalloc() 和 nvfree()，因此需要包含 common-utils.h
 */

#define NV_FMT_BUF_LEN 256

#define NV_VSNPRINTF(buf, fmt)                                  \
do {                                                            \
    if (!fmt) {                                                 \
        (buf) = NULL;                                           \
    } else {                                                    \
        va_list ap;                                             \
        int len, current_len = NV_FMT_BUF_LEN;                  \
                                                                \
        while (1) {                                             \
            (buf) = nvalloc(current_len);                       \
                                                                \
            va_start(ap, fmt);                                  \
            len = vsnprintf((buf), current_len, (fmt), ap);     \
            va_end(ap);                                         \
                                                                \
            if ((len > -1) && (len < current_len)) {            \
                break;                                          \
            } else if (len > -1) {                              \
                current_len = len + 1;                          \
            } else {                                            \
                current_len += NV_FMT_BUF_LEN;                  \
            }                                                   \
                                                                \
            nvfree(buf);                                        \
        }                                                       \
    }                                                           \
} while (0)


/*
 * NvVerbosity - 冗余度（详细程度）枚举类型。
 *
 * 控制程序消息输出的详细程度级别：
 *   NV_VERBOSITY_NONE       (0) : 不输出任何消息（完全静默）
 *   NV_VERBOSITY_ERROR      (1) : 仅输出错误
 *   NV_VERBOSITY_DEPRECATED (2) : 输出错误和弃用消息
 *   NV_VERBOSITY_WARNING    (3) : 输出错误和所有警告
 *   NV_VERBOSITY_ALL        (4) : 输出所有消息（错误、警告和信息）
 *   NV_VERBOSITY_DEFAULT    (=ALL) : 默认级别，等同于 ALL
 *
 * 每个消息输出函数会检查当前冗余度是否达到其要求的最低级别。
 */

typedef enum {
    NV_VERBOSITY_NONE = 0,                    /* 无错误、无警告、无信息 */
    NV_VERBOSITY_ERROR,                       /* 仅错误 */
    NV_VERBOSITY_DEPRECATED,                  /* 错误和弃用消息 */
    NV_VERBOSITY_WARNING,                     /* 错误和所有警告 */
    NV_VERBOSITY_ALL,                         /* 错误、所有警告和其他信息 */
    NV_VERBOSITY_DEFAULT = NV_VERBOSITY_ALL
} NvVerbosity;

/* 冗余度控制函数 */
NvVerbosity nv_get_verbosity(void);            /* 获取当前冗余度级别 */
void        nv_set_verbosity(NvVerbosity level); /* 设置冗余度级别 */


/*
 * 格式化 I/O 函数声明
 *
 * 所有函数在输出到终端时会自动按终端宽度换行，
 * 输出到非终端（管道/文件）时直接输出原始文本。
 */

void reset_current_terminal_width(unsigned short new_val);  /* 检测或设置终端宽度 */

void nv_error_msg(const char *fmt, ...)                NV_ATTRIBUTE_PRINTF(1, 2); /* 错误消息 */
void nv_deprecated_msg(const char *fmt, ...)           NV_ATTRIBUTE_PRINTF(1, 2); /* 弃用消息 */
void nv_warning_msg(const char *fmt, ...)              NV_ATTRIBUTE_PRINTF(1, 2); /* 警告消息 */
void nv_info_msg(const char *prefix,
                 const char *fmt, ...)                 NV_ATTRIBUTE_PRINTF(2, 3); /* 信息消息 */
void nv_info_msg_to_file(FILE *stream,
                         const char *prefix,
                         const char *fmt, ...)         NV_ATTRIBUTE_PRINTF(3, 4); /* 输出到指定流的信息消息 */
void nv_msg(const char *prefix, const char *fmt, ...)  NV_ATTRIBUTE_PRINTF(2, 3); /* 无条件消息 */
void nv_msg_preserve_whitespace(const char *prefix,
                                const char *fmt, ...)  NV_ATTRIBUTE_PRINTF(2, 3); /* 保留空白的消息 */


/*
 * TextRows 结构体 - 用于存储按宽度分行后的文本行集合。
 *
 * 成员：
 *   t - 字符串数组，每个元素是一行文本（char* 指针数组）
 *   n - 总行数
 *   m - 所有行中最大的行长度
 *
 * 使用流程：
 *   1. 使用 nv_format_text_rows() 创建
 *   2. 遍历 t[0..n-1] 获取各行文本
 *   3. 使用 nv_free_text_rows() 释放
 */

typedef struct {
    char **t; /* 文本行数组 */
    int n;    /* 行数 */
    int m;    /* 最大行长 */
} TextRows;

/* TextRows 操作函数 */
TextRows *nv_format_text_rows(const char *prefix, const char *str, int width,
                              int word_boundary);         /* 按宽度将文本分行 */
void nv_text_rows_append(TextRows *t, const char *msg);   /* 追加一行 */
void nv_concat_text_rows(TextRows *t0, TextRows *t1);     /* 合并两个 TextRows */
void nv_free_text_rows(TextRows *t);                       /* 释放 TextRows */


#endif /* __MSG_H__ */
