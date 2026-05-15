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
 * 文件说明: msg.c
 *
 * 本文件实现了消息格式化输出和文本换行处理功能，主要包括：
 *
 * 1. 冗余度（verbosity）控制：
 *    - 通过全局变量 __verbosity 控制消息输出级别
 *    - 支持从静默模式到全量输出的多个级别
 *
 * 2. 格式化输出函数：
 *    - nv_error_msg()   : 输出错误消息（带 "ERROR: " 前缀）
 *    - nv_warning_msg() : 输出警告消息（带 "WARNING: " 前缀）
 *    - nv_deprecated_msg(): 输出弃用消息（带 "DEPRECATED: " 前缀）
 *    - nv_info_msg()    : 输出信息消息（自定义前缀）
 *    - nv_msg()         : 无条件输出消息
 *    - nv_msg_preserve_whitespace(): 保留空白字符的消息输出
 *
 * 3. 文本行格式化（TextRows）：
 *    - nv_format_text_rows() : 将长文本按指定宽度分行，支持前缀和词边界对齐
 *    - nv_text_rows_append() : 向已有的 TextRows 追加一行
 *    - nv_concat_text_rows() : 合并两个 TextRows
 *    - nv_free_text_rows()   : 释放 TextRows 结构
 *
 * 4. 终端宽度检测：
 *    - reset_current_terminal_width() : 通过 TIOCGWINSZ ioctl 获取终端宽度
 *
 * 所有格式化输出函数在输出到终端时会自动进行换行处理，
 * 输出到非终端（如管道/文件）时则直接输出原始文本。
 */

#define _GNU_SOURCE // fileno 需要此定义

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#if defined(__sun)
#include <sys/termios.h>
#endif

#include "msg.h"
#include "common-utils.h"


/*
 * __verbosity - 全局冗余度级别变量。
 * 控制哪些级别的消息会被输出。
 * 默认值为 NV_VERBOSITY_DEFAULT（即 NV_VERBOSITY_ALL，输出所有消息）。
 */

static NvVerbosity __verbosity = NV_VERBOSITY_DEFAULT;

/*
 * nv_get_verbosity() - 获取当前冗余度级别。
 *
 * 返回值：
 *   当前的 NvVerbosity 级别
 */
NvVerbosity nv_get_verbosity(void)
{
    return __verbosity;
}

/*
 * nv_set_verbosity() - 设置冗余度级别。
 *
 * 参数：
 *   level - 要设置的冗余度级别（NvVerbosity 枚举值）
 */
void nv_set_verbosity(NvVerbosity level)
{
    __verbosity = level;
}


/****************************************************************************/
/* 格式化 I/O 函数                                                          */
/****************************************************************************/

/*
 * DEFAULT_WIDTH - 当无法检测终端宽度时使用的默认行宽（75 列）
 */
#define DEFAULT_WIDTH 75

/*
 * __terminal_width - 缓存的终端宽度值。
 * 初始值为 0，表示尚未检测。
 */
static unsigned short __terminal_width = 0;

/*
 * reset_current_terminal_width() - 检测或设置终端宽度。
 *
 * 功能：
 *   - 如果 new_val 非零，直接使用 new_val 作为终端宽度
 *   - 如果 new_val 为零，使用 TIOCGWINSZ ioctl 自动检测终端宽度
 *   - 如果 ioctl 失败或返回零宽度，使用默认值 DEFAULT_WIDTH (75)
 *   - 实际使用的宽度为终端宽度减 1（为换行符留出空间）
 *
 * 参数：
 *   new_val - 要设置的终端宽度值，0 表示自动检测
 */

void reset_current_terminal_width(unsigned short new_val)
{
    struct winsize ws;

    if (new_val) {
        __terminal_width = new_val;
        return;
    }

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        __terminal_width = DEFAULT_WIDTH;
    } else {
        __terminal_width = ws.ws_col - 1;
    }
}


/*
 * format() - 内部格式化输出函数。
 *
 * 功能：根据输出目标是否为终端，选择不同的输出策略：
 *   - 终端输出：使用 nv_format_text_rows() 将文本按终端宽度分行，
 *               支持前缀和词边界对齐
 *   - 非终端输出（管道/文件）：直接输出前缀+原始文本+换行
 *
 * 参数：
 *   stream     - 输出流（如 stdout 或 stderr）
 *   prefix     - 前缀字符串（如 "ERROR: "），可以为 NULL
 *   buf        - 要输出的消息文本
 *   whitespace - 是否在词边界处换行（TRUE/FALSE）
 */

static void format(FILE *stream, const char *prefix, const char *buf,
                   const int whitespace)
{
    if (isatty(fileno(stream))) {
        int i;
        TextRows *t;

        /* 如果终端宽度尚未检测，先进行检测 */
        if (!__terminal_width) reset_current_terminal_width(0);

        /* 将文本按终端宽度分行 */
        t = nv_format_text_rows(prefix, buf, __terminal_width, whitespace);

        /* 逐行输出 */
        for (i = 0; i < t->n; i++) fprintf(stream, "%s\n", t->t[i]);

        nv_free_text_rows(t);
    } else {
        /* 非终端输出：直接拼接前缀和内容 */
        fprintf(stream, "%s%s\n", prefix ? prefix : "", buf);
    }
}


/*
 * NV_FORMAT 宏 - 格式化可变参数并调用 format() 输出。
 *
 * 处理流程：
 *   1. 使用 NV_VSNPRINTF 将格式字符串和参数格式化为字符串 buf
 *   2. 调用 format() 进行输出
 *   3. 释放 buf
 *
 * 参数：
 *   stream     - 输出流
 *   prefix     - 前缀字符串
 *   fmt        - printf 风格的格式字符串（必须是包含 va_start 的函数中的最后一个固定参数名）
 *   whitespace - 是否在词边界处换行
 */
#define NV_FORMAT(stream, prefix, fmt, whitespace) \
do {                                               \
    char *buf;                                     \
    NV_VSNPRINTF(buf, fmt);                        \
    format(stream, prefix, buf, whitespace);       \
    free (buf);                                    \
} while (0)


/*
 * nv_error_msg() - 打印格式化的错误消息。
 *
 * 功能：以 "ERROR: " 前缀打印消息到 stderr，并在前后各加一空行。
 *       只有当冗余度级别 >= NV_VERBOSITY_ERROR 时才输出。
 *
 * 参数：
 *   fmt - printf 风格的格式字符串
 *   ... - 格式参数
 *
 * 应该用于所有错误消息。
 */

void nv_error_msg(const char *fmt, ...)
{
    if (__verbosity < NV_VERBOSITY_ERROR) return;

    format(stderr, NULL, "", TRUE);      /* 输出空行 */
    NV_FORMAT(stderr, "ERROR: ", fmt, TRUE);
    format(stderr, NULL, "", TRUE);      /* 输出空行 */
} /* nv_error_msg() */


/*
 * nv_deprecated_msg() - 打印格式化的弃用警告消息。
 *
 * 功能：以 "DEPRECATED: " 前缀打印消息到 stderr，并在前后各加一空行。
 *       只有当冗余度级别 >= NV_VERBOSITY_DEPRECATED 时才输出。
 *
 * 参数：
 *   fmt - printf 风格的格式字符串
 *   ... - 格式参数
 *
 * 应该用于所有弃用提示消息。
 */

void nv_deprecated_msg(const char *fmt, ...)
{
    if (__verbosity < NV_VERBOSITY_DEPRECATED) return;

    format(stderr, NULL, "", TRUE);
    NV_FORMAT(stderr, "DEPRECATED: ", fmt, TRUE);
    format(stderr, NULL, "", TRUE);
}


/*
 * nv_warning_msg() - 打印格式化的警告消息。
 *
 * 功能：以 "WARNING: " 前缀打印消息到 stderr，并在前后各加一空行。
 *       只有当冗余度级别 >= NV_VERBOSITY_WARNING 时才输出。
 *
 * 参数：
 *   fmt - printf 风格的格式字符串
 *   ... - 格式参数
 *
 * 应该用于所有警告消息。
 */

void nv_warning_msg(const char *fmt, ...)
{
    if (__verbosity < NV_VERBOSITY_WARNING) return;

    format(stderr, NULL, "", TRUE);
    NV_FORMAT(stderr, "WARNING: ", fmt, TRUE);
    format(stderr, NULL, "", TRUE);
} /* nv_warning_msg() */


/*
 * nv_info_msg() - 打印格式化的信息消息到 stdout。
 *
 * 功能：以自定义前缀打印消息到 stdout。
 *       只有当冗余度级别 >= NV_VERBOSITY_ALL 时才输出。
 *
 * 参数：
 *   prefix - 前缀字符串（可以为 NULL）
 *   fmt    - printf 风格的格式字符串
 *   ...    - 格式参数
 *
 * 应该用于显示详细/调试信息。
 */

void nv_info_msg(const char *prefix, const char *fmt, ...)
{
    if (__verbosity < NV_VERBOSITY_ALL) return;

    NV_FORMAT(stdout, prefix, fmt, TRUE);
} /* nv_info_msg() */


/*
 * nv_info_msg_to_file() - 打印格式化的信息消息到指定流。
 *
 * 功能：与 nv_info_msg() 相同，但可以指定输出流（而非固定 stdout）。
 *       只有当冗余度级别 >= NV_VERBOSITY_ALL 时才输出。
 *
 * 参数：
 *   stream - 输出流（如 stdout, stderr, 或文件指针）
 *   prefix - 前缀字符串（可以为 NULL）
 *   fmt    - printf 风格的格式字符串
 *   ...    - 格式参数
 */

void nv_info_msg_to_file(FILE *stream, const char *prefix, const char *fmt, ...)
{
    if (__verbosity < NV_VERBOSITY_ALL) return;

    NV_FORMAT(stream, prefix, fmt, TRUE);
} /* nv_info_msg_to_file() */


/*
 * nv_msg() - 无条件打印格式化消息到 stdout。
 *
 * 功能：以自定义前缀打印消息到 stdout，不受冗余度级别的控制。
 *       总是会输出，用于必须显示的消息。
 *
 * 参数：
 *   prefix - 前缀字符串（可以为 NULL）
 *   fmt    - printf 风格的格式字符串
 *   ...    - 格式参数
 */

void nv_msg(const char *prefix, const char *fmt, ...)
{
    NV_FORMAT(stdout, prefix, fmt, TRUE);
} /* nv_msg() */


/*
 * nv_msg_preserve_whitespace() - 打印保留空白字符的消息。
 *
 * 功能：与 nv_msg() 类似，但在文本格式化时不会跳过空白字符，
 *       保留原始的空白格式。当 whitespace 参数为 FALSE 时，
 *       format() 不会在词边界处换行并跳过空白。
 *
 * 参数：
 *   prefix - 前缀字符串（可以为 NULL）
 *   fmt    - printf 风格的格式字符串
 *   ...    - 格式参数
 */

void nv_msg_preserve_whitespace(const char *prefix, const char *fmt, ...)
{
    NV_FORMAT(stdout, prefix, fmt, FALSE);
} /* nv_msg_preserve_whitespace() */


/*
 * 当使用 gcc 的 '-ansi' 选项编译时，vsnprintf 可能未被声明，
 * 因此在严格 ANSI 模式下手动声明其原型。
 */

#if defined(__STRICT_ANSI__)
int vsnprintf(char *str, size_t size, const char  *format,
              va_list ap);
#endif


/****************************************************************************/
/* TextRows 辅助函数                                                        */
/****************************************************************************/

/*
 * nv_format_text_rows() - 将字符串按指定宽度分行。
 *
 * 功能：将给定的字符串 str 拆分为多行，每行不超过指定的宽度 width。
 *
 * 特性：
 *   - 如果 prefix 非 NULL，第一行前面会加上该前缀，
 *     后续行使用等量空格缩进以对齐文本
 *   - 如果 word_boundary 为 TRUE，尝试只在单词边界处断行
 *   - 如果遇到换行符 '\n'，强制在该位置断行
 *
 * 处理流程：
 *   1. 分配 TextRows 结构并初始化
 *   2. 复制输入字符串以便修改
 *   3. 循环处理每一行：
 *      a. 如果剩余字符串可以放在一行，直接使用
 *      b. 否则，找到合适的断行位置（词边界或固定宽度）
 *      c. 检查断行前是否有换行符，如有则在该处断行
 *      d. 创建新行字符串（前缀 + 文本内容）
 *      e. 将新行追加到 TextRows 数组
 *      f. 更新指针和剩余长度
 *   4. 第一行使用 prefix，后续行将 prefix 替换为等量空格
 *
 * 参数：
 *   prefix        - 第一行的前缀字符串（如 "ERROR: "），可以为 NULL
 *   str           - 要格式化的文本字符串
 *   width         - 最大行宽（包含前缀）
 *   word_boundary - 是否在单词边界处换行（TRUE/FALSE）
 *
 * 返回值：
 *   TextRows 结构指针，包含格式化后的所有行，调用者需使用 nv_free_text_rows() 释放
 */

TextRows *nv_format_text_rows(const char *prefix, const char *str, int width,
                              int word_boundary)
{
    int len, prefix_len, z, w, i;
    char *line, *buf, *local_prefix, *a, *b, *c;
    TextRows *t;

    /* 初始化 TextRows 结构 */

    t = (TextRows *) malloc(sizeof(TextRows));

    if (!t) return NULL;

    t->t = NULL;    /* 行数组指针 */
    t->n = 0;       /* 行数 */
    t->m = 0;       /* 最大行长 */

    if (!str) return t;

    buf = strdup(str);

    if (!buf) return t;

    z = strlen(buf); /* 剩余字符串的长度 */
    a = buf;         /* 指向当前行起始位置的指针 */

    /* 初始化前缀相关字段 */

    if (prefix) {
        prefix_len = strlen(prefix);
        local_prefix = strdup(prefix);
    } else {
        prefix_len = 0;
        local_prefix = NULL;
    }

    /* 调整可用宽度（减去前缀占用的宽度） */

    w = width - prefix_len;

    do {
        /*
         * 如果剩余字符串可以放在一行中，
         * 将 b 指向字符串末尾
         */

        if (z < w) b = a + z;

        /*
         * 如果剩余字符串放不下一行，将 b 移到行尾应该在的位置，
         * 然后往回找空格（词边界）；如果一直找到 a 都没找到空格，
         * 就在固定宽度处强制断行
         */

        else {
            b = a + w;

            if (word_boundary) {
                while ((b >= a) && (!isspace(*b))) b--;
                if (b <= a) b = a + w;
            }
        }

        /* 在 a 和 b 之间查找换行符，如找到则在该处断行 */

        for (c = a; c < b; c++) if (*c == '\n') { b = c; break; }

        /*
         * 复制从 a 到 b 的字符串，并在前面加上前缀（如果有）
         */

        len = b-a;
        len += prefix_len;
        line = (char *) malloc(len+1);
        if (local_prefix) strncpy(line, local_prefix, prefix_len);
        strncpy(line + prefix_len, a, len - prefix_len);
        line[len] = '\0';

        /* 将新行追加到 TextRows 的行数组中 */

        t->t = (char **) realloc(t->t, sizeof(char *) * (t->n + 1));
        t->t[t->n] = line;
        t->n++;

        /* 更新最大行长 */
        if (t->m < len) t->m = len;

        /*
         * 调整剩余字符串的长度，并将指针移到新行的开头
         */

        z -= (b - a + 1);
        a = b + 1;

        /* 跳过行首的空白字符（换行符除外） */

        if (word_boundary && isspace(*b)) {
            while ((z) && (isspace(*a)) && (*a != '\n')) a++, z--;
        } else {
            if (!isspace(*b)) z++, a--;
        }

        /*
         * 第一行之后，将前缀替换为等量的空格，
         * 使得后续行的文本与第一行的文本对齐
         */
        if (local_prefix) {
            for (i = 0; i < prefix_len; i++) local_prefix[i] = ' ';
        }

    } while (z > 0);

    if (local_prefix) free(local_prefix);
    free(buf);

    return t;
}


/*
 * nv_text_rows_append() - 向已有的 TextRows 追加一行。
 *
 * 功能：在 TextRows 的末尾追加一行文本。
 *       如果 msg 为 NULL，则追加一个 NULL 条目。
 *
 * 参数：
 *   t   - 要追加到的 TextRows 结构
 *   msg - 要追加的文本行（可以为 NULL）
 */

void nv_text_rows_append(TextRows *t, const char *msg)
{
    int len;

    /* 扩展行数组以容纳新行 */
    t->t = realloc(t->t, sizeof(char *) * (t->n + 1));

    if (msg) {
        t->t[t->n] = strdup(msg);
        len = strlen(msg);
        if (t->m < len) t->m = len;    /* 更新最大行长 */
    } else {
        t->t[t->n] = NULL;
    }

    t->n++;
}

/*
 * nv_concat_text_rows() - 将两个 TextRows 合并，结果存入 t0。
 *
 * 功能：将 t1 中的所有行复制并追加到 t0 的末尾。
 *       t1 的内容不会被修改或释放。
 *
 * 参数：
 *   t0 - 目标 TextRows（会被扩展）
 *   t1 - 源 TextRows（其行会被复制到 t0 末尾）
 */

void nv_concat_text_rows(TextRows *t0, TextRows *t1)
{
    int n, i;

    n = t0->n + t1->n;

    /* 扩展 t0 的行数组以容纳 t1 的所有行 */
    t0->t = realloc(t0->t, sizeof(char *) * n);

    /* 将 t1 的每一行复制到 t0 的末尾 */
    for (i = 0; i < t1->n; i++) {
        t0->t[i + t0->n] = strdup(t1->t[i]);
    }

    /* 更新最大行长和行数 */
    t0->m = NV_MAX(t0->m, t1->m);
    t0->n = n;

} /* nv_concat_text_rows() */


/*
 * nv_free_text_rows() - 释放 nv_format_text_rows() 分配的 TextRows 结构。
 *
 * 功能：释放 TextRows 中的所有行字符串、行指针数组，以及 TextRows 结构本身。
 *
 * 参数：
 *   t - 要释放的 TextRows 结构指针（允许为 NULL）
 */

void nv_free_text_rows(TextRows *t)
{
    int i;

    if (!t) return;
    for (i = 0; i < t->n; i++) free(t->t[i]);  /* 释放每一行 */
    if (t->t) free(t->t);                       /* 释放行指针数组 */
    free(t);                                     /* 释放结构本身 */

} /* nv_free_text_rows() */
