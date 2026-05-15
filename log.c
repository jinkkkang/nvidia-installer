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
 * 【文件说明】安装日志系统实现。
 * 提供日志初始化和写入功能。安装过程中所有重要操作、错误、警告
 * 都会记录到日志文件中（默认 /var/log/nvidia-installer.log）。
 * 日志文件头部包含时间戳、安装器版本、命令行参数等信息。
 *
 * log.c
 */

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#include "nvidia-installer.h"
#include "misc.h"

/* 全局日志文件流 */
static FILE *log_file_stream;


/* 将布尔值转换为字符串的便捷宏 */
#define BOOLSTR(b) ((b) ? "true" : "false")

/* 将可能为 NULL 的字符串转换为安全字符串的便捷宏 */
#define STRSTR(x) ((x) ? (x) : "(not specified)")

/* 将 SELinux 选项值转换为字符串的便捷宏 */
#define SELINUXSTR(x) ({ \
    const char *__selinux_str = NULL; \
    switch (x) { \
        case SELINUX_FORCE_YES: __selinux_str = "yes"; break; \
        case SELINUX_FORCE_NO: __selinux_str = "no"; break; \
        case SELINUX_DEFAULT: __selinux_str = "default"; break; \
        default: __selinux_str = "(not specified)"; \
    } \
    __selinux_str; \
})

/*
 * log_init() - 初始化日志系统。
 *
 * 如果日志功能已启用：
 *   1. 打开日志文件（写模式，覆盖已有内容）
 *   2. 如果打开失败，向 stderr 输出错误并禁用日志
 *   3. 如果打开成功，写入日志头部信息：
 *      - 日志文件路径
 *      - 创建时间
 *      - 安装器版本
 *      - 当前 PATH 环境变量
 *      - 完整的命令行参数
 *
 * 特殊处理：--add-this-kernel 允许非 root 用户运行，
 * 此时默认日志路径 /var/log/ 可能无法写入，会自动重定向到临时目录。
 */

void log_init(Options *op, int argc, char * const argv[])
{
    time_t now;
    char *path;
    int i;

    if (!op->logging) return;

    /* 非 root 用户运行 --add-this-kernel 时，重定向日志到临时目录 */
    if (op->add_this_kernel && geteuid() != 0) {
        if (strcmp(DEFAULT_LOG_FILE_NAME, op->log_file_name) == 0) {
            char *basename = nv_basename(DEFAULT_LOG_FILE_NAME);

            op->log_file_name = nvdircat(op->tmpdir, basename, NULL);
            nvfree(basename);
        }
    }

    /* 打开日志文件 */
    log_file_stream = fopen(op->log_file_name, "w");

    if (!log_file_stream) {
        fprintf(stderr, "%s: Error opening log file '%s' for "
                "writing (%s); disabling logging.\n",
                PROGRAM_NAME, op->log_file_name, strerror(errno));
        op->logging = FALSE;
        return;
    }

    /* 写入日志头部信息 */
    log_printf(op, NULL, "%s log file '%s'",
               PROGRAM_NAME, op->log_file_name);

    now = time(NULL);
    log_printf(op, NULL, "creation time: %s", ctime(&now));
    log_printf(op, NULL, "installer version: %s",
               NVIDIA_INSTALLER_VERSION);
    log_printf(op, NULL, "");

    path = getenv("PATH");
    log_printf(op, NULL, "PATH: %s", STRSTR(path));
    log_printf(op, NULL, "");

    /* 记录完整命令行参数 */
    log_printf(op, NULL, "nvidia-installer command line:");
    for (i = 0; i < argc; i++) {
        log_printf(op, "    ", "%s", argv[i]);
    }

    log_printf(op, NULL, "");

} /* log_init() */



/*
 * log_printf() - 向日志文件写入格式化文本。
 *
 * 如果日志未启用（op->logging == FALSE），则直接返回。
 * 支持可选的行前缀（prefix），以及 printf 风格的格式化字符串。
 * 如果格式化后的字符串末尾没有换行符，会自动追加一个。
 * 每次写入后都会 flush 文件流，确保日志实时写入磁盘。
 */

void log_printf(Options *op, const char *prefix, const char *fmt, ...)
{
    char *buf;
    int append_newline = TRUE;

    if (!op->logging) return;

    /* 使用 NV_VSNPRINTF 宏进行可变参数格式化 */
    NV_VSNPRINTF(buf, fmt);

    /* 如果调用者已在字符串末尾添加了换行符，则不再重复追加 */
    if (buf && buf[0] && (buf[strlen(buf) - 1] == '\n')) {
        append_newline = FALSE;
    }

    /* 输出可选前缀 */
    if (prefix) {
        fprintf(log_file_stream, "%s", prefix);
    }
    /* 输出格式化文本 */
    fprintf(log_file_stream, "%s%s", buf, append_newline ? "\n" : "");

    nvfree(buf);

    /* 立即刷新，确保日志内容不会因程序崩溃而丢失 */
    fflush(log_file_stream);

} /* log_printf() */
