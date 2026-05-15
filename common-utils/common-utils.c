/*
 * Copyright (C) 2010-2012 NVIDIA Corporation
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
 * 文件说明: common-utils.c
 *
 * 本文件实现了 NVIDIA 安装程序中使用的通用工具函数集合，主要包括：
 *
 * 1. 内存管理工具：
 *    - nvalloc()    : 带错误检查的 calloc 封装，分配失败时直接退出程序
 *    - nvrealloc()  : 带错误检查的 realloc 封装
 *    - nvstrdup()   : 带错误检查的 strdup 封装
 *    - nvstrndup()  : 带错误检查的 strndup 实现
 *    - nvfree()     : 安全的 free 封装（允许传入 NULL）
 *
 * 2. 字符串工具：
 *    - nvstrcat()   : 可变参数字符串拼接，自动分配内存
 *    - nvvstrcat()  : nvstrcat 的 va_list 版本
 *    - nvstrtolower(): 字符串转小写
 *    - nvstrtoupper(): 字符串转大写
 *    - nvasprintf() : 带错误检查的 asprintf 实现
 *    - nv_append_sprintf(): 向动态字符串追加格式化内容
 *    - nv_trim_space(): 去除字符串首尾空白
 *    - nv_trim_char(): 去除字符串首尾指定字符
 *
 * 3. 文件操作工具：
 *    - nv_open()         : 带错误检查的 open(2) 封装
 *    - nv_get_file_length(): 获取文件长度
 *    - nv_set_file_length(): 设置文件长度
 *    - nv_mmap()         : 带错误检查的 mmap(2) 封装
 *    - nv_basename()     : 跨平台的 basename 实现
 *    - nv_dirname()      : 跨平台的 dirname 实现
 *    - nv_mkdir_recursive(): 递归创建目录
 *    - nv_string_to_file(): 将字符串写入文件
 *
 * 4. 路径处理工具：
 *    - tilde_expansion()          : 波浪号路径展开（~/ 和 ~user/）
 *    - nvdircat()                 : 路径拼接（自动插入 '/'）
 *    - remove_trailing_slashes()  : 移除尾部斜杠
 *    - collapse_multiple_slashes(): 合并连续的斜杠
 *
 * 所有内存分配函数在失败时会打印错误信息到 stderr 并调用 exit(1)，
 * 因此调用方不需要检查返回值是否为 NULL（除非文档另有说明）。
 */

#include <stdio.h>
#include <stdarg.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pwd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "common-utils.h"


/****************************************************************************/
/* 内存分配辅助函数                                                          */
/****************************************************************************/

/*
 * nvalloc() - 对 calloc 的封装，带有错误检查功能。
 *
 * 功能：分配 size 字节的内存，并将其初始化为零（因为使用 calloc）。
 *       如果内存分配失败，则向 stderr 打印错误信息并调用 exit(1) 退出程序。
 *       因此本函数只会在分配成功时返回。
 *
 * 参数：
 *   size - 需要分配的内存大小（字节数）
 *
 * 返回值：
 *   指向新分配的已清零内存块的指针（永远不会返回 NULL）
 *
 * 注意：使用 calloc(1, size) 而非 malloc(size)，确保内存被初始化为全零
 */

void *nvalloc(size_t size)
{
    void *m = calloc(1, size);

    if (!m) {
        fprintf(stderr, "%s: memory allocation failure (%s)! \n",
                PROGRAM_NAME, strerror(errno));
        exit(1);
    }
    return m;

} /* nvalloc() */



/*
 * nvvstrcat() - 将多个字符串拼接到一个新分配的字符串中（va_list 版本）。
 *
 * 功能：接受一个字符串和一个 va_list 参数列表，将所有字符串拼接在一起。
 *       可变参数列表必须以 NULL 结尾。
 *
 * 处理流程：
 *   1. 首先遍历一次所有参数，计算拼接后字符串的总长度（+1 用于 '\0'）
 *   2. 使用 nvalloc() 分配足够的内存
 *   3. 再次遍历所有参数，依次将每个字符串拼接到结果中
 *
 * 参数：
 *   str - 第一个要拼接的字符串
 *   ap  - 后续字符串的 va_list，以 NULL 结尾
 *
 * 返回值：
 *   新分配的拼接后的字符串，调用者负责释放
 *
 * 注意：使用 va_copy 复制 va_list 以便进行两次遍历
 */

char *nvvstrcat(const char *str, va_list ap)
{
    const char *s;
    char *result;
    size_t len;
    va_list ap2;

    /* 遍历可变参数列表，计算结果字符串的总长度 */

    va_copy(ap2, ap);

    for (s = str, len = 1; s; s = va_arg(ap2, char *)) {
        len += strlen(s);
    }

    va_end(ap2);

    /* 分配结果字符串的内存 */

    result = nvalloc(len);
    if (!result) {
        return result;
    }
    result[0] = '\0';

    /* 将所有输入字符串依次拼接到结果字符串中 */

    for (s = str; s; s = va_arg(ap, char *)) {
        strcat(result, s);
    }

    return result;
} /* nvstrcat() */


/*
 * nvstrcat() - 将多个字符串拼接到一个新分配的字符串中。
 *
 * 功能：接受可变数量的字符串参数，将它们全部拼接成一个新字符串。
 *       参数列表必须以 NULL 结尾。
 *
 * 使用示例：
 *   char *result = nvstrcat("hello", " ", "world", NULL);
 *   // result == "hello world"
 *
 * 参数：
 *   str - 第一个要拼接的字符串
 *   ... - 后续字符串，以 NULL 结尾
 *
 * 返回值：
 *   新分配的拼接后的字符串，调用者负责释放
 *
 * 注意：内部委托给 nvvstrcat() 实现
 */

char *nvstrcat(const char *str, ...)
{
    va_list ap;
    char *ret;

    va_start(ap, str);
    ret = nvvstrcat(str, ap);
    va_end(ap);

    return ret;
}

/*
 * nvrealloc() - 对 realloc 的封装，带有错误检查功能。
 *
 * 功能：重新调整之前分配的内存块的大小。如果内存重新分配失败，
 *       则向 stderr 打印错误信息并调用 exit(1) 退出程序。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   ptr  - 指向之前分配的内存块的指针（如果为 NULL，则等同于 nvalloc）
 *   size - 新的内存大小（字节数）
 *
 * 返回值：
 *   指向重新分配的内存块的指针
 *
 * 注意：当 ptr 为 NULL 时，内部调用 nvalloc(size)，这样新内存会被初始化为零
 */

void *nvrealloc(void *ptr, size_t size)
{
    void *m;

    if (ptr == NULL) return nvalloc(size);

    m = realloc(ptr, size);
    if (!m) {
        fprintf(stderr, "%s: memory re-allocation failure (%s)! \n",
                PROGRAM_NAME, strerror(errno));
        exit(1);
    }
    return m;

} /* nvrealloc() */



/*
 * nvstrdup() - 对 strdup() 的封装，带有返回值检查。
 *
 * 功能：复制一个字符串到新分配的内存中。如果内存分配失败，
 *       则向 stderr 打印错误信息并调用 exit(1) 退出程序。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   s - 要复制的源字符串（如果为 NULL，则直接返回 NULL）
 *
 * 返回值：
 *   新分配的字符串副本，调用者负责释放；
 *   如果输入为 NULL，则返回 NULL
 */

char *nvstrdup(const char *s)
{
    char *m;

    if (!s) return NULL;

    m = strdup(s);

    if (!m) {
        fprintf(stderr, "%s: memory allocation failure during strdup (%s)! \n",
                PROGRAM_NAME, strerror(errno));
        exit(1);
    }
    return m;

} /* nvstrdup() */



/*
 * nvstrndup() - strndup() 的实现，带有返回值检查。
 *
 * 功能：复制源字符串的前 n 个字符到新分配的内存中，并在末尾添加 '\0'。
 *       如果内存分配失败，则向 stderr 打印错误信息并调用 exit(1) 退出程序。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   s - 要复制的源字符串（如果为 NULL，则直接返回 NULL）
 *   n - 要复制的最大字符数
 *
 * 返回值：
 *   新分配的字符串副本（最多 n 个字符加上终止符 '\0'），调用者负责释放
 *
 * 注意：手动使用 malloc + strncpy 实现，而非依赖系统的 strndup
 *       （某些系统可能不提供 strndup）
 */

char *nvstrndup(const char *s, size_t n)
{
    char *m;

    if (!s) return NULL;

    m = malloc(n + 1);

    if (!m) {
        fprintf(stderr, "%s: memory allocation failure during malloc (%s)! \n",
                PROGRAM_NAME, strerror(errno));
        exit(1);
    }

    strncpy (m, s, n);
    m[n] = '\0';

    return m;

} /* nvstrndup() */



/*
 * nvstrtolower() - 将给定字符串原地转换为小写。
 *
 * 参数：
 *   s - 要转换的字符串（会被原地修改）；如果为 NULL，则返回 NULL
 *
 * 返回值：
 *   指向字符串开头的指针（与输入相同）
 */

char *nvstrtolower(char *s)
{
    char *start = s;

    if (s == NULL) return NULL;

    while (*s) {
        *s = tolower(*s);
        s++;
    }

    return start;

} /* nvstrtolower() */



/*
 * nvstrtoupper() - 将给定字符串原地转换为大写。
 *
 * 参数：
 *   s - 要转换的字符串（会被原地修改）；如果为 NULL，则返回 NULL
 *
 * 返回值：
 *   指向字符串开头的指针（与输入相同）
 */

char *nvstrtoupper(char *s)
{
    char *start = s;

    if (s == NULL) return NULL;

    while (*s) {
        *s = toupper(*s);
        s++;
    }

    return start;

} /* nvstrtoupper() */



/*
 * nvasprintf() - asprintf() 的实现，带有返回值检查。
 *
 * 功能：按照格式字符串 fmt 格式化输出，并将结果存入新分配的字符串中。
 *       如果发生错误，则向 stderr 打印错误信息并调用 exit。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   fmt - printf 风格的格式字符串
 *   ... - 格式字符串对应的参数
 *
 * 返回值：
 *   新分配的格式化后的字符串，调用者负责释放
 *
 * 注意：内部使用 NV_VSNPRINTF 宏来处理不同 vsnprintf 实现的差异
 */
char *nvasprintf(const char *fmt, ...)
{
    char *str;

    NV_VSNPRINTF(str, fmt);

    return str;

} /* nvasprintf() */

/*
 * nv_append_sprintf() - 类似 glib 的 g_string_append_printf()，
 * 但操作的是 (char **) 而非 GString。
 *
 * 功能：将格式化的字符串追加到 *buf 指向的动态分配字符串的末尾。
 *       如果 *buf 为 NULL，则结果就是格式化后的字符串本身。
 *       该过程可能会重新分配字符串内存。
 *       本函数只会在成功时返回。
 *
 * 处理流程：
 *   1. 保存 *buf 的当前值作为前缀
 *   2. 使用 NV_VSNPRINTF 格式化新的后缀字符串
 *   3. 如果前缀为 NULL，直接使用后缀作为结果
 *   4. 否则，用 nvstrcat 拼接前缀和后缀，并释放旧的前缀和后缀
 *
 * 参数：
 *   buf - 指向动态字符串指针的指针（*buf 可以为 NULL）
 *   fmt - printf 风格的格式字符串
 *   ... - 格式字符串对应的参数
 */
void nv_append_sprintf(char **buf, const char *fmt, ...)
{
    char *prefix, *suffix;

    prefix = *buf;
    NV_VSNPRINTF(suffix, fmt);

    if (!prefix) {
        *buf = suffix;
    } else {
        *buf = nvstrcat(prefix, suffix, NULL);
        free(prefix);
        free(suffix);
    }
}


/*
 * nvfree() - 释放通过 nvalloc() 分配的内存。
 *
 * 功能：安全地释放内存，在释放前会检查指针是否为 NULL。
 *       如果指针为 NULL，则不执行任何操作（避免对 NULL 调用 free 的问题，
 *       尽管 C 标准允许对 NULL 调用 free）。
 *
 * 参数：
 *   s - 要释放的内存指针（允许为 NULL）
 */
void nvfree(void *s)
{
    if (s) free(s);

} /* nvfree() */



/****************************************************************************/
/* 杂项工具函数                                                              */
/****************************************************************************/

/*
 * tilde_expansion() - 对给定的路径名进行波浪号展开。
 *
 * 基于 comp.unix.programmer FAQ 中的代码片段。
 *
 * 波浪号展开规则：
 *   - 如果波浪号 '~' 单独出现或后面跟着 '/'，则替换为当前用户的主目录
 *   - 如果后面跟着用户名（如 ~user），则替换为该用户的主目录
 *
 * 获取主目录的优先级：
 *   - 对于当前用户：先尝试 $HOME 环境变量，若不存在则通过 getpwuid 查询 /etc/passwd
 *   - 对于指定用户：通过 getpwnam 查询 /etc/passwd
 *
 * 参数：
 *   str - 要展开的路径字符串
 *
 * 返回值：
 *   如果参数为 NULL，返回 NULL；
 *   否则返回一个通过 malloc 分配的展开后的字符串，调用者负责释放
 *
 * 注意：如果无法确定主目录（例如用户不存在），则返回原始字符串的副本
 */

char *tilde_expansion(const char *str)
{
    char *prefix = NULL;
    const char *replace;
    char *user, *ret;
    struct passwd *pw;
    int len;

    if (!str) return NULL;

    /* 如果不以 '~' 开头，直接返回字符串的副本 */
    if (str[0] != '~') return strdup(str);

    if ((str[1] == '/') || (str[1] == '\0')) {

        /* 展开为当前用户的主目录 */

        prefix = getenv("HOME");
        if (!prefix) {

            /* $HOME 未设置；从 /etc/passwd 获取主目录 */

            pw = getpwuid(getuid());
            if (pw) prefix = pw->pw_dir;
        }

        /* replace 指向 '~' 之后的部分（即 '/' 或 '\0'） */
        replace = str + 1;

    } else {

        /* 展开为指定用户的主目录 */

        /* 找到用户名后面的 '/' 位置 */
        replace = strchr(str, '/');
        if (!replace) replace = str + strlen(str);

        /* 提取用户名（~ 和 / 之间的部分） */
        len = replace - str;
        user = malloc(len + 1);
        strncpy(user, str+1, len-1);
        user[len] = '\0';
        pw = getpwnam(user);
        if (pw) prefix = pw->pw_dir;
        free (user);
    }

    /* 如果无法确定主目录，返回原始字符串的副本 */
    if (!prefix) return strdup(str);

    /* 构造结果：主目录 + 波浪号后面的路径部分 */
    ret = malloc(strlen(prefix) + strlen(replace) + 1);
    strcpy(ret, prefix);
    strcat(ret, replace);

    return ret;

} /* tilde_expansion() */


/*
 * nv_prepend_to_string_list() - 在字符串列表的前面添加一个新字符串。
 *
 * 功能：将 item 添加到以 delim 分隔的字符串列表 list 的开头。
 *       如果 list 为 NULL，则结果只有 item 自身（不添加分隔符）。
 *       原始的 list 会被释放。
 *
 * 参数：
 *   list  - 当前的字符串列表（可以为 NULL）
 *   item  - 要在前面添加的字符串
 *   delim - 分隔符字符串
 *
 * 返回值：
 *   新分配的字符串列表，调用者负责释放
 */

char *nv_prepend_to_string_list(char *list, const char *item, const char *delim)
{
    char *new_list = nvstrcat(item, list ? delim : NULL, list, NULL);
    nvfree(list);
    return new_list;
}


/*
 * fget_next_line() - 从给定的 FILE 流中读取一行数据。
 *
 * 功能：从文件流中逐字符读取，直到遇到换行符 '\n'、EOF 或空字符 '\0'。
 *       使用可增长的缓冲区存储读取的数据。
 *       当遇到 EOF 时，如果 eof 参数非 NULL，则将 *eof 设置为 TRUE。
 *       无论何种情况，返回的字符串都以 '\0' 结尾。
 *
 * 处理流程：
 *   1. 每次循环检查缓冲区是否需要扩展（以 32 字节为增量）
 *   2. 使用 fgetc() 逐个读取字符
 *   3. 遇到 EOF、'\n' 或 '\0' 时结束读取
 *   4. 否则将字符写入缓冲区并继续
 *
 * 参数：
 *   fp  - 要读取的文件流指针
 *   eof - 输出参数，如果遇到 EOF 则设为 TRUE（可以为 NULL）
 *
 * 返回值：
 *   新分配的字符串，包含读取的一行内容（不含换行符），调用者负责释放
 *
 * 注意：此实现使用 fgetc() 逐字符读取，性能较慢，
 *       但可以正确处理 EOF 和各种行结束符。
 *       使用 fgets() 可以更快，但需要额外的换行符/EOF 解析逻辑。
 */
char *fget_next_line(FILE *fp, int *eof)
{
    char *buf = NULL, *tmpbuf;
    char *c = NULL;
    int len = 0, buflen = 0;
    int ret;

    /* 每次缓冲区增长的字节数 */
    const int __fget_next_line_len = 32;

    if (eof) {
        *eof = FALSE;
    }

    while (1) {
        if (buflen == len) { /* 缓冲区不够大 -- 扩展它 */
            buflen += __fget_next_line_len;
            tmpbuf = nvalloc(buflen);
            if (buf) {
                memcpy(tmpbuf, buf, len);
                nvfree(buf);
            }
            buf = tmpbuf;
            c = buf + len;
        }

        ret = fgetc(fp);

        if ((ret == EOF) && (eof)) {
            *eof = TRUE;
        }

        if ((ret == EOF) || (ret == '\n') || (ret == '\0')) {
            *c = '\0';
            return buf;
        }

        *c = (char) ret;

        len++;
        c++;

    } /* while (1) */

    return NULL; /* 正常情况下不应该执行到这里 */
}

/*
 * nvstrchrnul() - 类似 strchr()，但在未找到字符时返回指向字符串末尾 '\0' 的指针，
 *                 而非返回 NULL。
 *
 * 功能：在字符串 s 中查找字符 c 的第一次出现。
 *       如果找到，返回指向该字符的指针；
 *       如果未找到，返回指向字符串末尾 '\0' 的指针。
 *
 * 参数：
 *   s - 要搜索的字符串
 *   c - 要查找的字符
 *
 * 返回值：
 *   指向找到的字符或字符串末尾的指针
 *
 * 注意：某些系统提供 strchrnul() 函数（GNU 扩展），本函数提供可移植的替代实现
 */
char *nvstrchrnul(char *s, int c)
{
    char *result = strchr(s, c);
    if (!result) {
        return (s + strlen(s));
    }
    return result;
}

/****************************************************************************/
/* 文件操作辅助函数                                                          */
/****************************************************************************/

/*
 * nv_open() - 对 open(2) 系统调用的封装，带有错误检查。
 *
 * 功能：打开指定路径的文件。如果打开失败，打印错误信息并调用 exit(1) 退出。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   pathname - 要打开的文件路径
 *   flags    - 打开标志（如 O_RDONLY, O_WRONLY, O_RDWR 等）
 *   mode     - 创建新文件时的权限模式（仅当 flags 包含 O_CREAT 时有效）
 *
 * 返回值：
 *   文件描述符
 */

int nv_open(const char *pathname, int flags, mode_t mode)
{
    int fd;
    fd = open(pathname, flags, mode);
    if (fd == -1) {
        fprintf(stderr, "Failure opening %s (%s).\n",
                pathname, strerror(errno));
        exit(1);
    }
    return fd;

} /* nv_name() */



/*
 * nv_get_file_length() - 获取文件长度的 stat(2) 封装。
 *
 * 功能：使用 stat() 系统调用获取指定文件的大小。
 *       如果调用失败，打印错误信息并调用 exit(1) 退出。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   filename - 要查询大小的文件路径
 *
 * 返回值：
 *   文件大小（字节数）
 */

int nv_get_file_length(const char *filename)
{
    struct stat stat_buf;
    int ret;

    ret = stat(filename, &stat_buf);
    if (ret == -1) {
        fprintf(stderr, "Unable to determine '%s' file length (%s).\n",
                filename, strerror(errno));
        exit(1);
    }
    return stat_buf.st_size;

} /* nv_get_file_length() */



/*
 * nv_set_file_length() - 设置文件长度的封装函数。
 *
 * 功能：通过 lseek() 将文件指针移动到 len-1 位置，然后写入一个空字节，
 *       从而将文件长度设置为 len 字节。
 *       如果任一系统调用失败，打印错误信息并调用 exit(1) 退出。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   filename - 文件名（仅用于错误信息）
 *   fd       - 已打开的文件描述符
 *   len      - 要设置的文件长度
 *
 * 注意：错误信息中使用了 fd 而非 len 作为长度参数，
 *       这可能是原始代码中的一个 bug（疑似应为 len）
 */

void nv_set_file_length(const char *filename, int fd, int len)
{
    if ((lseek(fd, len - 1, SEEK_SET) == -1) ||
        (write(fd, "", 1) == -1)) {
        fprintf(stderr, "Unable to set file '%s' length %d (%s).\n",
                filename, fd, strerror(errno));
        exit(1);
    }
} /* nv_set_file_length() */



/*
 * nv_mmap() - 对 mmap(2) 系统调用的封装，带有错误检查。
 *
 * 功能：将文件映射到内存中。如果映射失败，打印错误信息并调用 exit(1) 退出。
 *       本函数只会在成功时返回。
 *
 * 参数：
 *   filename - 文件名（仅用于错误信息）
 *   len      - 要映射的长度（字节数）
 *   prot     - 内存保护标志（如 PROT_READ, PROT_WRITE 等）
 *   flags    - 映射标志（如 MAP_SHARED, MAP_PRIVATE 等）
 *   fd       - 要映射的文件描述符
 *
 * 返回值：
 *   指向映射内存区域的指针
 *
 * 注意：映射从文件偏移 0 开始（最后一个参数固定为 0）
 */

void *nv_mmap(const char *filename, size_t len, int prot, int flags, int fd)
{
    void *ret;

    ret = mmap(0, len, prot, flags, fd, 0);
    if (ret == (void *) -1) {
        fprintf(stderr, "Unable to mmap file %s (%s).\n",
                filename, strerror(errno));
        exit(1);
    }
    return ret;

} /* nv_mmap() */


/*
 * nv_basename() - basename(3) 的替代实现。
 *
 * 功能：从路径中提取文件名部分（最后一个 '/' 之后的内容）。
 *       与标准 basename(3) 不同，此实现：
 *       - 从不修改原始字符串
 *       - 返回值总是可以安全地传给 free(3)
 *
 * 参数：
 *   path - 文件路径
 *
 * 返回值：
 *   新分配的文件名字符串，调用者负责释放
 *   如果路径中没有 '/'，返回整个路径的副本
 */

char *nv_basename(const char *path)
{
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        return strdup(last_slash+1);
    } else {
        return strdup(path);
    }
}


/*
 * nv_mkdir_recursive() - 递归创建目录及所有必要的父目录。
 *
 * 功能：类似 "mkdir -p" 命令，创建指定路径的目录，
 *       如果父目录不存在也会一并创建。
 *
 * 处理流程：
 *   1. 复制路径字符串并移除尾部斜杠
 *   2. 从头到尾遍历路径中的每个 '/' 分隔符
 *   3. 对于每个目录层级，检查是否已存在，如不存在则创建
 *   4. 如果提供了 dir_list 参数，则记录每个新创建的目录路径
 *
 * 参数：
 *   path      - 要创建的目录路径
 *   mode      - 新创建目录的权限模式（如 0755）
 *   error_str - 输出参数，如果发生错误则包含错误描述字符串
 *   dir_list  - 可选输出参数，如果非 NULL，则设置为换行符分隔的已创建目录列表
 *
 * 返回值：
 *   成功返回 TRUE，失败返回 FALSE
 */
int nv_mkdir_recursive(const char *path, const mode_t mode,
                       char **error_str, char **dir_list)
{
    char *c, *tmp, ch, *list;
    int success = FALSE;

    if (!path || !path[0]) {
        return FALSE;
    }

    /* 复制路径并移除尾部斜杠 */
    tmp = nvstrdup(path);
    remove_trailing_slashes(tmp);

    list = NULL;

    /* 逐个字符遍历路径，在每个 '/' 处尝试创建目录 */
    c = tmp;
    do {
        c++;
        if ((*c == '/') || (*c == '\0')) {
            ch = *c;
            *c = '\0';    /* 临时截断字符串以获取当前层级的路径 */
            if (!directory_exists(tmp)) {
                char *tmplist;
                if (mkdir(tmp, mode) != 0) {
                    *error_str =
                        nvasprintf("Failure creating directory '%s' : (%s)",
                                   tmp, strerror(errno));
                    goto done;
                }
                /* 将创建的目录路径添加到列表前部 */
                if (dir_list) {
                    tmplist = list;
                    list = nvstrcat(tmp, "\n", tmplist, NULL);
                    free(tmplist);
                }
            }
            *c = ch;    /* 恢复被截断的字符 */
        }
    } while (*c);

    /* 记录所有创建的目录 */
    if (dir_list && list) {
        *dir_list = list;
    }

    success = TRUE;

 done:

    if (!dir_list) {
        free(list);
    }
    free(tmp);
    return success;
}



/*
 * nvdircat() - 路径拼接函数，在每个路径元素之间插入 '/' 分隔符。
 *
 * 功能：接受可变数量的路径元素，将它们用 '/' 连接起来，
 *       并合并结果中的多余斜杠。
 *
 * 使用示例：
 *   char *path = nvdircat("/usr", "local", "bin", NULL);
 *   // path == "/usr/local/bin"
 *
 * 参数：
 *   str - 第一个路径元素
 *   ... - 后续路径元素，以 NULL 结尾
 *
 * 返回值：
 *   新分配的拼接后的路径字符串，调用者负责释放
 *
 * 注意：第一个元素前不会添加 '/'，后续元素前会自动添加 '/'
 */

char *nvdircat(const char *str, ...)
{
    const char *s;
    char *result = nvstrdup("");
    va_list ap;

    va_start(ap, str);

    for (s = str; s; s = va_arg(ap, char *)) {
        char *oldresult = result;

        /* 第一个元素不加前缀 '/'，后续元素加 '/' */
        result = nvstrcat(result, s == str ? "" : "/", s, NULL);
        nvfree(oldresult);
    }

    va_end(ap);

    /* 合并结果中可能出现的连续斜杠（如 "foo//bar" -> "foo/bar"） */
    collapse_multiple_slashes(result);

    return result;
}


/*
 * nv_dirname() - dirname(3) 的替代实现。
 *
 * 功能：从路径中提取目录部分（最后一个 '/' 之前的内容）。
 *       与标准 dirname(3) 不同，此实现：
 *       - 总是返回通过堆分配的字符串，可以安全地传给 free(3)
 *       - 从不修改原始字符串
 *
 * 参数：
 *   path - 文件路径
 *
 * 返回值：
 *   新分配的目录路径字符串，调用者负责释放
 *   如果路径中没有 '/'，返回 "."
 */
char *nv_dirname(const char *path)
{
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        return nvstrndup(path, last_slash - path);
    } else {
        return nvstrdup(".");
    }
}


/*
 * nv_string_to_file() - 将以 NUL 结尾的字符串内容写入文件。
 *
 * 功能：将 data 字符串的内容写入 destination 指定的文件。
 *       如果数据末尾没有换行符，会自动追加一个换行符。
 *       如果目标目录不存在，会尝试递归创建。
 *       写入完成后将文件权限设为 0644。
 *
 * 处理流程：
 *   1. 获取目标文件的目录路径
 *   2. 递归创建目录（如果不存在）
 *   3. 以写入模式打开文件
 *   4. 写入数据内容
 *   5. 如果数据末尾没有换行符，追加一个换行符
 *   6. 关闭文件并设置权限为 0644
 *
 * 参数：
 *   destination - 目标文件路径
 *   data        - 要写入的字符串内容
 *
 * 返回值：
 *   成功返回 TRUE，失败返回 FALSE
 */
int nv_string_to_file(const char *destination, const char *data)
{
    char *dname = nv_dirname(destination);
    int written, newline_success = TRUE;
    char *error = NULL;
    int len, ret;
    FILE *fp;

    /* 确保目标目录存在 */
    ret = nv_mkdir_recursive(dname, 0755, &error, NULL);
    nvfree(dname);
    nvfree(error);

    if (!ret) return FALSE;

    fp = fopen(destination, "w");
    if (!fp) return FALSE;

    /* 写入数据内容 */
    len = strlen(data);
    written = fwrite(data, 1, len, fp);

    /* 如果数据末尾没有换行符，追加一个 */
    if (data[len-1] != '\n') {
        if (fwrite("\n", 1, 1, fp) != 1) {
            newline_success = FALSE;
        }
    }

    if (fclose(fp)) return FALSE;
    if (chmod(destination, 0644)) return FALSE;

    return written == len && newline_success;
}



/****************************************************************************/
/* 字符串辅助函数                                                            */
/****************************************************************************/

/*
 * nv_trim_space() - 去除字符串首尾的空白字符。
 *
 * 功能：跳过字符串开头的空白字符，并将末尾的空白字符替换为 '\0'。
 *
 * 参数：
 *   string - 要处理的字符串（可能会被修改末尾的字符）
 *
 * 返回值：
 *   指向去除前导空白后的字符串的指针
 *
 * 重要提示：
 *   - 返回的指针可能不指向 string 的起始位置
 *   - 因此不能对返回值调用 free()
 *   - 如果原始字符串是 malloc 分配的，应保留原始指针用于 free
 */

char *nv_trim_space(char *string) {
    char *ret, *end;

    /* 跳过前导空白字符 */
    for (ret = string; *ret && isspace(*ret); ret++);
    /* 从末尾开始，将尾部空白字符替换为 '\0' */
    for (end = ret + strlen(ret) - 1; end >= ret && isspace(*end); end--) {
        *end = '\0';
    }

    return ret;
}

/*
 * trim_char() - 内部辅助函数，去除字符串首尾的指定字符。
 *
 * 功能：如果字符串的第一个字符等于 trim，则跳过它（指针后移）；
 *       如果字符串的最后一个字符等于 trim，则将其替换为 '\0'。
 *       可选地报告替换了多少个位置（0、1 或 2）。
 *
 * 参数：
 *   string - 要处理的字符串
 *   trim   - 要去除的字符
 *   count  - 输出参数，替换的次数（可以为 NULL）
 *
 * 返回值：
 *   处理后的字符串指针（可能与输入不同），不能对其调用 free()
 */

static char *trim_char(char *string, char trim, int *count) {
    int len, replaced = 0;

    if (count) {
        *count = 0;
    }

    if (string == NULL || trim == '\0') {
        return string;
    }

    /* 如果首字符匹配，跳过它 */
    if (string[0] == trim) {
        string++;
        replaced++;
    }

    len = strlen(string);

    /* 如果尾字符匹配，替换为 '\0' */
    if (string[len - 1] == trim) {
        string[len - 1] = '\0';
        replaced++;
    }

    if (count) {
        *count = replaced;
    }

    return string;
}

/*
 * nv_trim_char() - 去除字符串首尾的指定字符。
 *
 * 参数：
 *   string - 要处理的字符串
 *   trim   - 要去除的字符
 *
 * 返回值：
 *   处理后的字符串指针，不能对其调用 free()（参见 nv_trim_space 的说明）
 */

char *nv_trim_char(char *string, char trim) {
    return trim_char(string, trim, NULL);
}

/*
 * nv_trim_char_strict() - 严格模式的首尾字符去除。
 *
 * 功能：去除字符串首尾的指定字符，但有以下限制：
 *       - 如果没有进行任何替换（count == 0），返回原始字符串
 *       - 如果首尾都进行了替换（count == 2），返回处理后的字符串
 *       - 如果只有一端进行了替换（count == 1），返回 NULL
 *
 * 这在解析需要配对的界定符时很有用（例如引号必须成对出现）。
 *
 * 参数：
 *   string - 要处理的字符串
 *   trim   - 要去除的字符
 *
 * 返回值：
 *   处理后的字符串指针，或者如果只匹配了一端则返回 NULL
 *   返回的非 NULL 指针不能对其调用 free()
 */

char *nv_trim_char_strict(char *string, char trim) {
    int count;
    char *trimmed;

    trimmed = trim_char(string, trim, &count);

    if (count == 0 || count == 2) {
        return trimmed;
    }

    return NULL;
}

/*
 * directory_exists() - 检测给定路径的目录是否存在。
 *
 * 功能：使用 stat() 检查指定路径是否存在且为目录。
 *
 * 参数：
 *   dir - 要检查的目录路径
 *
 * 返回值：
 *   如果目录存在返回 TRUE，否则返回 FALSE
 */

int directory_exists(const char *dir)
{
    struct stat stat_buf;

    if ((stat (dir, &stat_buf) == -1) || (!S_ISDIR(stat_buf.st_mode))) {
        return FALSE;
    } else {
        return TRUE;
    }
}

/*
 * remove_trailing_slashes() - 移除字符串末尾的所有斜杠。
 *
 * 功能：从字符串末尾开始，将连续的 '/' 字符替换为 '\0'。
 *
 * 参数：
 *   string - 要处理的字符串（会被原地修改）
 *
 * 注意：如果字符串为 NULL 则不执行任何操作
 */

void remove_trailing_slashes(char *string)
{
    int len;

    if (string == NULL) {
        return;
    }

    len = strlen(string);

    while (string[len-1] == '/') {
        string[--len] = '\0';
    }

}



/*
 * collapse_multiple_slashes() - 合并字符串中所有连续的 '//' 为单个 '/'。
 *
 * 功能：在原地修改字符串，将所有出现的连续多个斜杠替换为单个斜杠。
 *       例如 "foo//bar///baz" 变为 "foo/bar/baz"。
 *
 * 处理流程：
 *   1. 使用 strstr 查找 "//" 的出现
 *   2. 找到后，保留第一个 '/'，移除后续的连续 '/'
 *   3. 通过左移字符串内容来覆盖多余的斜杠
 *   4. 重复直到没有连续斜杠
 *
 * 参数：
 *   s - 要处理的字符串（会被原地修改）
 */

void collapse_multiple_slashes(char *s)
{
    char *p;

    while ((p = strstr(s, "//")) != NULL) {
        p++; /* 前进到第二个 '/' */
        while (*p == '/') {
            unsigned int i, len;

            /* 将 p 之后的所有字符左移一位，覆盖当前的 '/' */
            len = strlen(p);
            for (i = 0; i < len; i++) p[i] = p[i+1];
        }
    }
}
