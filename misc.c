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
 * misc.c - this source file contains miscellaneous routines for use
 * by the nvidia-installer.
 */

/*
 * ============================================================================
 * 文件说明: misc.c - nvidia-installer 杂项工具函数实现文件
 * ============================================================================
 *
 * 本文件是 nvidia-installer 项目的核心工具函数集合，提供了安装程序运行所需的
 * 各种辅助功能。主要包含以下几类函数：
 *
 * 1. 字符串处理工具:
 *    - read_next_word(): 从字符串中读取下一个单词
 *    - get_next_line(): 从缓冲区中读取下一行
 *    - extract_version_string(): 从字符串中提取版本号
 *
 * 2. 系统工具查找:
 *    - find_system_utils(): 查找系统必需工具（如 ldconfig、grep 等）
 *    - find_module_utils(): 查找内核模块管理工具（如 modprobe、rmmod 等）
 *    - find_system_util(): 在 PATH 中搜索指定工具的完整路径
 *    - check_development_tools(): 检查编译开发工具是否可用
 *
 * 3. 系统环境检测:
 *    - check_euid(): 检查是否以 root 权限运行
 *    - check_selinux(): 检测 SELinux 状态
 *    - check_for_running_x(): 检测 X Server 是否正在运行
 *    - check_for_nouveau(): 检测 nouveau 开源驱动是否加载
 *    - secure_boot_enabled(): 检测 UEFI Secure Boot 是否启用
 *    - check_systemd(): 检查 systemd 是否可用
 *
 * 4. PCI 设备扫描:
 *    - pci_device_scan(): 扫描系统中的 NVIDIA GPU 设备
 *    - check_for_nvidia_graphics_devices(): 检查并报告 GPU 支持情况
 *
 * 5. 命令执行:
 *    - run_command(): 执行外部命令并捕获输出
 *
 * 6. 文件操作与验证:
 *    - read_text_file(): 读取文本文件内容
 *    - verify_crc(): CRC 校验验证
 *    - check_installed_file(): 检查已安装文件的完整性
 *    - get_elf_architecture(): 读取 ELF 文件架构信息
 *
 * 7. DKMS（动态内核模块支持）管理:
 *    - dkms_register_module(): 向 DKMS 注册内核模块
 *    - dkms_remove_module(): 从 DKMS 中移除模块
 *    - dkms_module_installed(): 检查模块是否已通过 DKMS 安装
 *
 * 8. 安装流程辅助:
 *    - do_install(): 执行安装操作
 *    - should_install_compat32_files(): 处理 32 位兼容库安装
 *    - should_install_optional_modules(): 处理可选内核模块安装
 *    - check_installed_files_from_package(): 安装后完整性检查
 *    - run_nvidia_xconfig(): 运行 nvidia-xconfig 配置工具
 *    - run_distro_hook(): 运行发行版提供的钩子脚本
 *    - suggest_reboot(): 建议用户重启系统
 *
 * ============================================================================
 */

#include <stdlib.h>    /* 标准库：malloc/free/exit 等 */
#include <stdio.h>     /* 标准 I/O：printf/fopen/fclose 等 */
#include <string.h>    /* 字符串操作：strlen/strcpy/strcmp 等 */
#include <unistd.h>    /* POSIX API：access/getuid/chdir 等 */
#include <errno.h>     /* 错误码定义 */
#include <signal.h>    /* 信号处理：sigaction/SIGWINCH 等 */
#include <sys/types.h> /* 基本系统数据类型：uid_t/pid_t 等 */
#include <sys/utsname.h> /* uname 系统信息 */
#include <sys/stat.h>  /* 文件状态：stat/lstat/S_ISREG 等 */
#include <ctype.h>     /* 字符分类：isspace/isdigit 等 */
#include <fcntl.h>     /* 文件控制：open/O_RDONLY 等 */
#include <sys/mman.h>  /* 内存映射：mmap/munmap */
#include <dirent.h>    /* 目录操作：opendir/readdir 等 */
#include <libgen.h>    /* 路径处理：basename/dirname */
#include <pciaccess.h> /* PCI 设备访问库（libpciaccess） */
#include <elf.h>       /* ELF 文件格式定义 */
#include <link.h>      /* 动态链接器接口 */

#include "nvidia-installer.h"         /* nvidia-installer 主头文件 */
#include "user-interface.h"           /* 用户界面交互函数 */
#include "kernel.h"                   /* 内核模块相关操作 */
#include "files.h"                    /* 文件操作工具 */
#include "misc.h"                     /* 本文件对应的头文件 */
#include "crc.h"                      /* CRC 校验计算 */
#include "nvGpus.h"                   /* NVIDIA GPU 列表（包含 LegacyList 等） */
#include "manifest.h"                 /* 安装包清单解析 */
#include "nvpci-utils.h"              /* PCI 工具函数 */
#include "conflicting-kernel-modules.h" /* 冲突内核模块检测 */
#include "initramfs.h"                /* initramfs 更新相关 */
#include "detect-self-hosted.h"       /* 自托管 GPU 检测（如 Grace Hopper） */

/* 前向声明：check_symlink() 用于检查符号链接是否正确 */
static int check_symlink(Options*, const char*, const char*, const char*);


/*
 * read_next_word() - 从字符串中读取下一个单词
 *
 * 功能说明:
 *   给定字符串 buf，跳过所有前导空白字符，然后复制从当前位置开始直到
 *   遇到下一个空白字符为止的字符序列（即一个"单词"）。
 *   注意：换行符 '\n' 会被视为分隔符而非空白字符来跳过。
 *
 * 参数:
 *   buf - 输入字符串，从此位置开始读取
 *   e   - 输出参数（传引用），如果非 NULL，将被设置为指向单词末尾的位置，
 *          方便调用者连续多次调用 read_next_word() 来逐词解析字符串
 *
 * 返回值:
 *   成功时返回新分配的字符串（包含提取到的单词），调用者需要释放内存
 *   如果没有找到单词（长度为0），返回 NULL
 */

char *read_next_word (char *buf, char **e)
{
    char *c = buf;
    char *start, *ret;
    int len;

    /* 跳过前导空白字符（不包括换行符） */
    while ((*c) && (isspace (*c)) && (*c != '\n')) c++;
    start = c;
    /* 读取非空白字符直到遇到空白或换行符 */
    while ((*c) && (!isspace (*c)) && (*c != '\n')) c++;

    len = c - start;

    /* 如果单词长度为 0，说明没有找到有效单词 */
    if (len == 0) return NULL;

    /* 分配内存并复制单词（+1 用于 NUL 终止符） */
    ret = (char *) nvalloc (len + 1);

    strncpy (ret, start, len);
    ret[len] = '\0';

    /* 如果调用者提供了 e 参数，设置它指向单词末尾 */
    if (e) *e = c;

    return ret;

} /* read_next_word() */



/*
 * check_euid() - 检查当前进程的有效用户 ID（EUID）是否为 root
 *
 * 功能说明:
 *   nvidia-installer 必须以 root 权限运行才能安装驱动和内核模块。
 *   此函数检查 EUID，如果不是 root（EUID != 0），则通过用户界面
 *   打印错误信息并返回 FALSE。
 *
 * 参数:
 *   op - 全局选项结构体指针，用于传递给 ui_error() 显示错误信息
 *
 * 返回值:
 *   TRUE  - 当前为 root 用户（EUID == 0）
 *   FALSE - 当前不是 root 用户
 */

int check_euid(Options *op)
{
    uid_t euid;

    euid = geteuid();

    if (euid != 0) {
        ui_error(op, "nvidia-installer must be run as root");
        return FALSE;
    }

    return TRUE;

} /* check_euid() */



/*
 * adjust_cwd() - 调整当前工作目录到可执行文件所在的目录
 *
 * 功能说明:
 *   此函数从 program_name（即 argv[0]）中提取路径部分，并使用 chdir()
 *   切换到该目录。目的是让安装程序的工作目录变为可执行文件所在的目录，
 *   这样安装程序可以正确地找到与之同目录的其他文件。
 *
 *   假设在调用此函数时，用户界面尚未初始化，因此错误信息直接输出到
 *   stderr 而非通过 ui_error()。
 *
 * 参数:
 *   op           - 全局选项结构体指针
 *   program_name - 程序名（argv[0]），可能包含相对或绝对路径
 *
 * 返回值:
 *   TRUE  - 成功（无需切换目录或切换成功）
 *   FALSE - chdir() 失败
 */

int adjust_cwd(Options *op, const char *program_name)
{
    char *c;
    int success = TRUE;

    /*
     * 从 program_name 中提取路径部分（最后一个 '/' 之前的内容），
     * 并切换到该目录
     */

    c = strrchr(program_name, '/');
    if (c) {
        int len;
        char *path;

        /* 计算路径长度（包含最后的 '/'） */
        len = c - program_name + 1;
        path = (char *) nvalloc(len + 1);
        strncpy(path, program_name, len);
        path[len] = '\0';

        /* 在专家模式下，记录 chdir 操作日志 */
        if (op->expert) log_printf(op, NULL, "chdir(\"%s\")", path);
        if (chdir(path)) {
            fprintf(stderr, "Unable to chdir to %s (%s)",
                    path, strerror(errno));
            success = FALSE;
        }
        free(path);
    }

    return success;
}


/*
 * get_next_line() - 从缓冲区中提取下一行文本
 *
 * 功能说明:
 *   扫描缓冲区 buf，寻找下一个换行符（\n）、回车符（\r）、NUL 终止符
 *   或 EOF。将找到的这一行文本复制到新分配的缓冲区中返回。
 *
 * 参数:
 *   buf    - 当前读取位置的指针
 *   end    - 输出参数（传引用）。如果非 NULL，将被设置为指向下一行的起始位置
 *            （即跳过换行符等不可打印字符后的第一个可打印字符），
 *            如果已到达缓冲区末尾则设为 NULL
 *   start  - 缓冲区的起始地址。如果非 NULL，用于配合 length 参数进行边界检查，
 *            确保不会读取超过 start + length 的位置
 *   length - 缓冲区总长度（从 start 开始计算），仅当 start 非 NULL 时有效
 *
 * 返回值:
 *   成功时返回新分配的字符串（包含一行文本，以 NUL 终止），调用者需释放内存
 *   失败或到达末尾时返回 NULL
 */

char *get_next_line(char *buf, char **end, char *start, int length)
{
    char *c, *retbuf;
    int len;

    /* 如果指定了 start 但 length 小于 1，说明缓冲区无有效数据 */
    if (start && (length < 1)) return NULL;

/* 辅助宏：检查当前位置是否已超出缓冲区末尾 */
#define __AT_END(_start, _current, _length) \
    ((_start) && (((_current) - (_start)) >= (_length)))

    if (end) *end = NULL;

    /*
     * 将 char 转换为 signed char 后与 EOF 比较，
     * 以确保在 char 为 unsigned 的平台（如 ARM GCC）上正确进行符号扩展
     */
    if ((!buf) ||
        __AT_END(start, buf, length) ||
        (*buf == '\0') ||
        (((signed char)*buf) == EOF)) return NULL;

    c = buf;

    /* 向前扫描，直到遇到行结束标志（换行、回车、NUL、EOF）或超出缓冲区 */
    while ((!__AT_END(start, c, length)) &&
           (*c != '\0') &&
           (((signed char)*c) != EOF) &&
           (*c != '\n') &&
           (*c != '\r')) c++;

    /* 复制这一行的内容到新分配的缓冲区 */
    len = c - buf;
    retbuf = nvalloc(len + 1);
    strncpy(retbuf, buf, len);
    retbuf[len] = '\0';

    /* 设置 end 指针，跳过不可打印字符（如换行、回车），指向下一行的起始位置 */
    if (end) {
        while ((!__AT_END(start, c, length)) &&
               (*c != '\0') &&
               (((signed char)*c) != EOF) &&
               (!isprint(*c))) c++;

        if (__AT_END(start, c, length) ||
            (*c == '\0') ||
            (((signed char)*c) == EOF)) *end = NULL;
        else *end = c;
    }

    return retbuf;

} /* get_next_line() */



/*
 * run_command() - 执行外部 shell 命令并捕获其输出
 *
 * 功能说明:
 *   通过 popen() 执行给定的命令，读取命令的标准输出，并将输出内容
 *   存储在动态分配的缓冲区中返回给调用者。支持进度显示、输出匹配
 *   和 stderr 重定向等功能。
 *
 * 参数:
 *   op           - 全局选项结构体指针
 *   data         - 输出参数（传引用），指向命令输出的缓冲区。调用者负责释放。
 *                  如果为 NULL，则命令输出被丢弃
 *   output       - 是否将命令输出实时发送到用户界面显示。
 *                  TRUE = 每读取一行就显示到 UI
 *   output_match - 输出匹配规则数组（以 { 0 } 终止）。用于估算命令将产生
 *                  的输出行数，以便计算进度百分比并更新 UI 进度条。
 *                  可选的 initial_match 字段用于前缀匹配过滤。
 *                  设为 NULL 则跳过进度更新
 *   redirect     - 是否将 stderr 重定向到 stdout（追加 " 2>&1"）。
 *                  TRUE = 重定向，收集所有输出；FALSE = 仅收集 stdout
 *   cmd_start    - 命令字符串的第一部分（可变参数，由 nvvstrcat 拼接，
 *                  以 NULL 结尾）
 *
 * 返回值:
 *   命令的退出状态码（通过 pclose() 获取）。如果命令无法执行，返回 errno
 *
 * 注意:
 *   - 在执行命令前会清除 LANG 和 LC_ALL 环境变量，确保命令输出不受
 *     locale 设置影响（方便后续解析）
 *   - 如果 op->sigwinch_workaround 为真，执行期间会临时忽略 SIGWINCH
 *     信号，避免子进程因窗口大小改变而异常退出
 *   - 命令字符串通过可变参数（va_list）拼接而成
 */

int run_command(Options *op, char **data, int output,
                const RunCommandOutputMatch *output_match, int redirect,
                const char *cmd_start, ...)
{
    int n = 0;      /* 已匹配的输出行计数器 */
    int len = 0;    /* 已实际读取的数据长度 */
    int buflen = 0; /* 输出缓冲区的分配大小 */
    int ret, total_lines;
    char *cmd, *buf = NULL;
    FILE *stream = NULL;
    struct sigaction act, old_act;
    float percent;
    int *match_sizes = NULL;  /* 缓存各匹配规则的 initial_match 字符串长度 */
    va_list ap;

    /* 使用可变参数列表拼接完整的命令字符串 */
    va_start(ap, cmd_start);
    cmd = nvvstrcat(cmd_start, ap);
    va_end(ap);

    if (!cmd) {
        return 1;
    }

    if (data) *data = NULL;

    /*
     * 如果需要显示命令输出，先打印即将执行的命令
     */

    if (output) ui_command_output (op, "executing: '%s'...", cmd);

    /* 如果需要，将 stderr 重定向到 stdout，追加 " 2>&1" */

    if (redirect) {
        char *tmp = cmd;
        cmd = nvstrcat(cmd, " 2>&1", NULL);
        nvfree(tmp);
    }

    /*
     * 临时忽略 SIGWINCH 信号（终端窗口大小改变信号）。
     * 子进程会继承这个信号处理方式，避免子进程因收到 SIGWINCH 而异常终止。
     * 这修复了当父进程捕获 SIGWINCH 时，子进程可能因此中止的问题。
     */
    if (op->sigwinch_workaround) {
        act.sa_handler = SIG_IGN;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;

        if (sigaction(SIGWINCH, &act, &old_act) < 0)
            old_act.sa_handler = NULL;
    }

    /*
     * 清除 LANG 和 LC_ALL 环境变量，确保命令输出不受系统 locale 设置影响。
     * 这对于需要解析命令输出的场景非常重要。
     */

    unsetenv("LANG");
    unsetenv("LC_ALL");


    /*
     * 通过 popen() 创建管道、fork 子进程并执行命令
     */

    stream = popen(cmd, "r");

    if (stream == NULL) {
        ret = errno;
        ui_error(op, "Failure executing command '%s' (%s).",
                 cmd, strerror(errno));
    }

    nvfree(cmd);

    if (stream == NULL) {
        return ret;
    }

    /*
     * 从管道流中持续读取数据，动态增长缓冲区 buf，直到读到 EOF。
     * 每读取一行，如果 output 为真，就将该行发送到 UI 显示。
     */

    if (output_match) {
        int match_count, i;

        /* 计算所有匹配规则的预期总行数 */
        for (total_lines = i = 0; output_match[i].lines; i++) {
            total_lines += output_match[i].lines;
        }

        match_count = i;

        /* 缓存各 initial_match 字符串的长度，避免重复计算 */
        match_sizes = nvalloc(match_count * sizeof(*match_sizes));

        for (i = 0; i < match_count; i++) {
            if (output_match[i].initial_match) {
                match_sizes[i] = strlen(output_match[i].initial_match);
            }
        }

        /* 避免除以零 */
        if (total_lines == 0) total_lines = 1;

        /*
         * 注意: 'match_sizes' 和 'total_lines' 仅在 output_match != NULL 时
         * 才可安全使用。否则 match_sizes 不可解引用，total_lines 未初始化。
         */
    }

    while (1) {

        /* 如果缓冲区剩余空间不足，扩展缓冲区 */
        if ((buflen - len) < NV_MIN_LINE_LEN) {
            buflen += NV_LINE_LEN;
            buf = nvrealloc(buf, buflen);
        }

        /* 读取一行数据，EOF 时退出循环 */
        if (fgets(buf + len, buflen - len, stream) == NULL) break;

        /* 如果需要，将当前行输出到 UI */
        if (output) ui_command_output(op, "%s", buf + len);

        /* 根据匹配规则更新进度 */
        if (output_match) {
            int i;

            /* 检查当前行是否匹配任一规则 */
            for (i = 0; output_match[i].lines; i++) {
                const char *s = output_match[i].initial_match;
                if (s == NULL || strncmp(buf + len, s, match_sizes[i]) == 0) {
                    n++;
                    break;
                }
            }

            /* 确保计数不超过预期总行数 */
            if (n > total_lines) n = total_lines;
            percent = (float) n / (float) total_lines;

            /*
             * 手动调用 SIGWINCH 处理器（如果有设置的话），
             * 在忽略 SIGWINCH 信号期间处理窗口大小调整
             */
            if (op->sigwinch_workaround) {
                /* 确保处理器不是特殊的信号处理指针值 */
                if (old_act.sa_handler != NULL &&
                    old_act.sa_handler != SIG_DFL &&
                    old_act.sa_handler != SIG_IGN &&
                    old_act.sa_handler != SIG_ERR) {
                    old_act.sa_handler(SIGWINCH);
                }
            }

            /* 更新 UI 进度条 */
            ui_status_update(op, percent, NULL);
        }

        len += strlen(buf + len);
    } /* while (1) */

    nvfree(match_sizes);

    /* 关闭 popen() 打开的管道流，获取命令的退出状态 */

    ret = pclose(stream);

    /*
     * 恢复 SIGWINCH 信号的原始处理方式和处理器
     */
    if (op->sigwinch_workaround)
        sigaction(SIGWINCH, &old_act, NULL);

    /* 如果缓冲区最后一个字符是换行符，将其替换为 NUL */

    if ((len > 0) && (buf[len-1] == '\n')) buf[len-1] = '\0';

    /* 将缓冲区传递给调用者，或者释放它 */
    if (data) *data = buf;
    else free(buf);

    return ret;

} /* run_command() */



/*
 * read_text_file() - 读取整个文本文件内容到缓冲区
 *
 * 功能说明:
 *   打开指定的文本文件，逐行读取其内容，将所有行拼接成一个字符串
 *   并存储在新分配的缓冲区中返回给调用者。每行之间用换行符分隔。
 *   缓冲区采用动态扩展策略（每次不足时翻倍增长）。
 *
 * 参数:
 *   filename - 要读取的文本文件路径
 *   buf      - 输出参数（传引用），指向存储文件内容的缓冲区。
 *              调用者负责释放。如果失败则为 NULL
 *
 * 返回值:
 *   TRUE  - 文件读取成功
 *   FALSE - 文件打开失败或内存分配失败
 */

int read_text_file(const char *filename, char **buf)
{
    FILE *fp;
    int index = 0, buflen = 0;
    int eof = FALSE;
    char *line, *tmpbuf;

    *buf = NULL;

    fp = fopen(filename, "r");
    if (!fp)
        return FALSE;

    /* 逐行读取文件，fget_next_line() 在每次调用时分配一行的内容 */
    while (((line = fget_next_line(fp, &eof)) != NULL)) {
        /* 检查缓冲区是否有足够空间，+2 用于换行符和 NUL 终止符 */
        if ((index + strlen(line) + 2) > buflen) {
            /* 按 2 倍增长策略扩展缓冲区 */
            buflen = 2 * (index + strlen(line) + 2);
            tmpbuf = (char *)nvalloc(buflen);
            if (!tmpbuf) {
                if (*buf) nvfree(*buf);
                fclose(fp);
                return FALSE;
            }
            /* 将已有内容复制到新缓冲区 */
            if (*buf) {
                memcpy(tmpbuf, *buf, index);
                nvfree(*buf);
            }
            *buf = tmpbuf;
        }

        /* 将当前行追加到缓冲区，并附加换行符 */
        index += sprintf(*buf + index, "%s\n", line);
        nvfree(line);

        if (eof) {
            break;
        }
    }

    fclose(fp);

    return TRUE;

} /* read_text_file() */



/*
 * find_system_utils() - 在 $PATH 和常用目录中搜索系统必需工具
 *
 * 功能说明:
 *   搜索安装程序所需的各类系统工具（如 ldconfig、grep 等），并将找到的
 *   工具路径存储在 op->utils[] 数组中。搜索路径包括环境变量 $PATH 和
 *   EXTRA_PATH 中定义的额外目录。
 *
 * 返回值:
 *   TRUE  - 所有必需工具都已找到
 *   FALSE - 某个必需工具未找到（会显示缺少的包名提示）
 */

/* 额外的搜索路径，补充 $PATH 中可能缺失的常用目录 */
#define EXTRA_PATH "/bin:/usr/bin:/sbin:/usr/sbin:/usr/X11R6/bin:/usr/bin/X11"

/*
 * 工具列表定义。
 * 注意：数组索引必须与 SystemUtils、SystemOptionalUtils、ModuleUtils
 * 和 DevelopUtils 枚举类型保持同步。
 */

/* Util 结构体：描述一个系统工具的名称和所属软件包 */
typedef struct {
    const char *util;    /* 工具的可执行文件名（如 "ldconfig"） */
    const char *package; /* 工具所属的软件包名称（如 "glibc"），用于提示用户安装 */
} Util;

/*
 * __utils[] - 所有工具的静态注册表
 *
 * 按功能分为四组：
 * - SystemUtils:         系统必需工具（缺失则安装失败）
 * - SystemOptionalUtils: 系统可选工具（缺失不会导致安装失败）
 * - ModuleUtils:         内核模块管理工具（加载/卸载/查看模块）
 * - DevelopUtils:        开发编译工具（构建内核模块时需要）
 *
 * 使用 C99 指定初始化器（designated initializer），通过枚举值作为下标
 * 来确保数组索引与枚举定义的对应关系。
 */
static const Util __utils[] = {

    /* SystemUtils - 系统必需工具 */
    [LDCONFIG] = { "ldconfig", "glibc" },      /* 共享库缓存管理 */
    [GREP]     = { "grep",     "grep" },        /* 文本搜索工具 */
    [DMESG]    = { "dmesg",    "util-linux" },  /* 内核日志查看 */
    [TAIL]     = { "tail",     "coreutils" },   /* 文件尾部查看 */

    /* SystemOptionalUtils - 系统可选工具 */
    [OBJCOPY]         = { "objcopy",        "binutils" },    /* 目标文件操作 */
    [CHCON]           = { "chcon",          "selinux" },     /* SELinux 安全上下文修改 */
    [SELINUX_ENABLED] = { "selinuxenabled", "selinux" },     /* SELinux 状态检测 */
    [GETENFORCE]      = { "getenforce",     "selinux" },     /* SELinux 强制模式查询 */
    [EXECSTACK]       = { "execstack",      "selinux" },     /* 可执行栈标记（SELinux） */
    [PKG_CONFIG]      = { "pkg-config",     "pkg-config" },  /* 库编译参数查询 */
    [XSERVER]         = { "X",              "xserver" },     /* X Server 可执行文件 */
    [OPENSSL]         = { "openssl",        "openssl" },     /* OpenSSL 命令行工具 */
    [DKMS]            = { "dkms",           "dkms"    },     /* 动态内核模块支持 */
    [SYSTEMCTL]       = { "systemctl",      "systemd" },     /* systemd 服务管理 */
    [TAR]             = { "tar",            "tar"     },     /* 归档打包工具 */

    /* ModuleUtils - 内核模块管理工具 */
    [MODPROBE] = { "modprobe", "module-init-tools' or 'kmod" }, /* 加载内核模块 */
    [RMMOD]    = { "rmmod",    "module-init-tools' or 'kmod" }, /* 卸载内核模块 */
    [LSMOD]    = { "lsmod",    "module-init-tools' or 'kmod" }, /* 列出已加载模块 */
    [DEPMOD]   = { "depmod",   "module-init-tools' or 'kmod" }, /* 生成模块依赖关系 */

    /* DevelopUtils - 开发编译工具 */
    [CC]   = { "cc",   "gcc"  },       /* C 编译器 */
    [MAKE] = { "make", "make" },       /* 构建工具 */
    [LD]   = { "ld",   "binutils" },   /* 链接器 */
    [TR]   = { "tr",   "coreutils" },  /* 字符转换工具 */
    [SED]  = { "sed",  "sed" },        /* 流编辑器 */

};

/*
 * find_system_utils() 的实现
 *
 * 处理流程:
 *   1. 搜索所有必需的系统工具（MIN_SYSTEM_UTILS 到 MAX_SYSTEM_UTILS），
 *      任何一个缺失都会导致安装失败
 *   2. 搜索所有可选的系统工具（MIN_SYSTEM_OPTIONAL_UTILS 到
 *      MAX_SYSTEM_OPTIONAL_UTILS），缺失只是不记录，不会报错
 *   3. 特殊处理 X Server：如果找不到名为 "X" 的程序，尝试搜索 "Xorg"
 */
int find_system_utils(Options *op)
{
    int i;

    ui_expert(op, "Searching for system utilities:");

    /* 遍历搜索所有必需的系统工具 */

    for (i = MIN_SYSTEM_UTILS; i < MAX_SYSTEM_UTILS; i++) {
        op->utils[i] = find_system_util(__utils[i].util);
        if (!op->utils[i]) {
            /* 必需工具缺失，报错并提示用户安装对应的软件包 */
            ui_error(op, "Unable to find the system utility `%s`; please "
                     "make sure you have the package '%s' installed.  If "
                     "you do have %s installed, then please check that "
                     "`%s` is in your PATH.",
                     __utils[i].util, __utils[i].package,
                     __utils[i].package, __utils[i].util);
            return FALSE;
        }

        ui_expert(op, "found `%s` : `%s`", __utils[i].util, op->utils[i]);
    }

    /* 遍历搜索所有可选的系统工具（找不到也不报错） */
    for (i = MIN_SYSTEM_OPTIONAL_UTILS; i < MAX_SYSTEM_OPTIONAL_UTILS; i++) {

        op->utils[i] = find_system_util(__utils[i].util);
        if (op->utils[i]) {
            ui_expert(op, "found `%s` : `%s`", __utils[i].util, op->utils[i]);
        }
    }

    /* 如果找不到名为 "X" 的程序，尝试搜索 "Xorg" 作为备选 */
    if (op->utils[XSERVER] == NULL) {
        op->utils[XSERVER] = find_system_util("Xorg");
        if (op->utils[XSERVER]) {
            ui_expert(op, "found `%s` : `%s`",
                      "Xorg", op->utils[XSERVER]);
        }
    }

    return TRUE;

} /* find_system_utils() */


/*
 * find_module_utils() - 在 $PATH 和常用目录中搜索内核模块管理工具
 *
 * 功能说明:
 *   搜索内核模块操作所需的工具（modprobe、rmmod、lsmod、depmod），
 *   这些工具对于加载、卸载和管理 NVIDIA 内核模块是必需的。
 *
 * 参数:
 *   op - 全局选项结构体指针。找到的工具路径将存储在 op->utils[] 中
 *
 * 返回值:
 *   TRUE  - 所有模块工具都已找到
 *   FALSE - 某个模块工具未找到
 */

int find_module_utils(Options *op)
{
    int i;

    ui_expert(op, "Searching for module utilities:");

    /* 在 PATH 中搜索每个模块管理工具 */

    for (i = MIN_MODULE_UTILS; i < MAX_MODULE_UTILS; i++) {
        op->utils[i] = find_system_util(__utils[i].util);
        if (!op->utils[i]) {
            ui_error(op, "Unable to find the module utility `%s`; please "
                     "make sure you have the package '%s' installed.  If "
                     "you do have '%s' installed, then please check that "
                     "`%s` is in your PATH.",
                     __utils[i].util, __utils[i].package,
                     __utils[i].package, __utils[i].util);
            return FALSE;
        }

        ui_expert(op, "found `%s` : `%s`", __utils[i].util, op->utils[i]);
    };

    return TRUE;

} /* find_module_utils() */


/*
 * check_proc_modprobe_path() - 验证 /proc 中报告的 modprobe 路径与实际路径一致
 *
 * 功能说明:
 *   内核通过 /proc/sys/kernel/modprobe 文件记录了系统中 modprobe 工具的路径，
 *   X Server 等组件会读取此路径来加载内核模块。此函数检查该路径是否与
 *   find_system_utils() 找到的 modprobe 路径一致，以确保内核模块加载功能正常。
 *
 *   处理流程:
 *   1. 读取 /proc/sys/kernel/modprobe 文件获取内核记录的 modprobe 路径
 *   2. 如果 modprobe 路径是符号链接，解析到真实路径
 *   3. 比较两个路径是否一致
 *   4. 如果 /proc 文件不存在（如 /proc 未挂载），检查默认路径 /sbin/modprobe
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - 路径一致或虽不一致但 modprobe 可用
 *   FALSE - modprobe 路径有问题且不可用
 */

/* /proc 文件系统中记录 modprobe 路径的文件 */
#define PROC_MODPROBE_PATH_FILE "/proc/sys/kernel/modprobe"
/* 默认的 modprobe 路径（当 /proc 不可用时使用） */
#define DEFAULT_MODPROBE "/sbin/modprobe"

int check_proc_modprobe_path(Options *op)
{
    FILE *fp;
    char *proc_modprobe = NULL, *found_modprobe;
    struct stat st;
    int ret, success = FALSE;

    /* 获取 find_system_utils() 之前找到的 modprobe 路径 */
    found_modprobe = op->utils[MODPROBE];

    /* 尝试从 /proc/sys/kernel/modprobe 读取内核记录的 modprobe 路径 */
    fp = fopen(PROC_MODPROBE_PATH_FILE, "r");
    if (fp) {
        proc_modprobe = fget_next_line(fp, NULL);
        fclose(fp);
    }

    /* 如果 find_system_utils() 找到的 modprobe 是符号链接，解析为真实路径 */

    ret = lstat(found_modprobe, &st);

    if (ret == 0 && S_ISLNK(st.st_mode)) {
        char *target = get_resolved_symlink_target(op, found_modprobe);
        if (target && access(target, F_OK | X_OK) == 0) {
            found_modprobe = target;
        } else {
            nvfree(target);
        }
    }

    if (proc_modprobe) {

        /* 如果内核报告的 modprobe 路径也是符号链接，同样解析为真实路径 */

        ret = lstat(proc_modprobe, &st);

        if (ret == 0 && S_ISLNK(st.st_mode)) {
            char *target = get_resolved_symlink_target(op, proc_modprobe);
            if (target && access(target, F_OK | X_OK) == 0) {
                nvfree(proc_modprobe);
                proc_modprobe = target;
            } else {
                nvfree(target);
            }
        }

        /* 比较内核报告的路径与 nvidia-installer 找到的路径是否一致 */

        if (strcmp(proc_modprobe, found_modprobe) == 0) {
            success = TRUE;
        } else {
            if (access(proc_modprobe, F_OK | X_OK) == 0) {
                ui_warn(op, "The path to the `modprobe` utility reported by "
                        "'%s', `%s`, differs from the path determined by "
                        "`nvidia-installer`, `%s`.  Please verify that `%s` "
                        "works correctly and correct the path in '%s' if "
                        "it does not.",
                        PROC_MODPROBE_PATH_FILE, proc_modprobe, found_modprobe,
                        proc_modprobe, PROC_MODPROBE_PATH_FILE);
                success = TRUE;
            } else {
               ui_error(op, "The path to the `modprobe` utility reported by "
                        "'%s', `%s`, differs from the path determined by "
                        "`nvidia-installer`, `%s`, and does not appear to "
                        "point to a valid `modprobe` binary.  Please correct "
                        "the path in '%s'.",
                        PROC_MODPROBE_PATH_FILE, proc_modprobe, found_modprobe,
                        PROC_MODPROBE_PATH_FILE);
            }
        }
    } else {
        /* 无法读取 /proc/sys/kernel/modprobe，可能因为文件不存在或 /proc
         * 未挂载。回退使用默认 modprobe 路径 /sbin/modprobe。 */

        char * found_mismatch;

        if (strcmp(DEFAULT_MODPROBE, found_modprobe) == 0) {
            found_mismatch = nvstrdup("");
        } else {
            found_mismatch = nvstrcat("This path differs from the one "
                                      "determined by `nvidia-installer`, ",
                                      found_modprobe, ".  ", NULL);
        }

        if (access(DEFAULT_MODPROBE, F_OK | X_OK) == 0) {
            ui_warn(op, "The file '%s' is unavailable; the X server will "
                    "use `" DEFAULT_MODPROBE "` as the path to the `modprobe` "
                    "utility.  %sPlease verify that `" DEFAULT_MODPROBE
                    "` works correctly or mount the /proc file system and "
                    "verify that '%s' reports the correct path.",
                    PROC_MODPROBE_PATH_FILE, found_mismatch,
                    PROC_MODPROBE_PATH_FILE);
            success = TRUE;
        } else {
           ui_error(op, "The file '%s' is unavailable; the X server will "
                    "use `" DEFAULT_MODPROBE "` as the path to the `modprobe` "
                    "utility.  %s`" DEFAULT_MODPROBE "` does not appear to "
                    "point to a valid `modprobe` binary.  Please create a "
                    "symbolic link from `" DEFAULT_MODPROBE "` to `%s` or "
                    "mount the /proc file system and verify that '%s' reports "
                    "the correct path.",
                    PROC_MODPROBE_PATH_FILE, found_mismatch,
                    found_modprobe, PROC_MODPROBE_PATH_FILE);
        }

        nvfree(found_mismatch);
    }

    nvfree(proc_modprobe);
    if (found_modprobe != op->utils[MODPROBE]) {
        nvfree(found_modprobe);
    }

    return success;

} /* check_proc_modprobe_path() */


/*
 * check_development_tool() - 检查单个开发工具是否可用
 *
 * 功能说明:
 *   检查 op->utils[idx] 中指定索引的开发工具是否已找到。如果未找到，
 *   则显示错误信息并提示用户安装相应的软件包。
 *
 * 参数:
 *   op  - 全局选项结构体指针
 *   idx - 工具在 __utils[] 数组中的索引
 *
 * 返回值:
 *   TRUE  - 工具已找到
 *   FALSE - 工具未找到
 */

static int check_development_tool(Options *op, int idx)
{
    if (!op->utils[idx]) {
        ui_error(op, "Unable to find the development tool `%s` in "
                 "your path; please make sure that you have the "
                 "package '%s' installed.  If %s is installed on your "
                 "system, then please check that `%s` is in your "
                 "PATH.",
                 __utils[idx].util, __utils[idx].package,
                 __utils[idx].package, __utils[idx].util);
        return FALSE;
    }

    ui_expert(op, "found `%s` : `%s`", __utils[idx].util, op->utils[idx]);

    return TRUE;
}

/*
 * check_development_tools() - 检查构建内核模块所需的开发工具是否齐全
 *
 * 功能说明:
 *   检查编译 NVIDIA 内核模块所需的开发工具链（cc、make、ld、tr、sed）
 *   以及 libc 头文件是否安装。还会对 C 编译器进行健全性检查。
 *
 *   特殊处理：如果用户通过环境变量 CC 指定了编译器，则跳过对系统默认
 *   cc 的 PATH 搜索（因为 $CC 可能指向不在 PATH 中的编译器）。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *   p  - 安装包信息结构体，包含内核模块构建目录等信息
 *
 * 返回值:
 *   TRUE  - 所有开发工具可用且编译器健全性检查通过
 *   FALSE - 缺少工具或编译器检查失败
 */

int check_development_tools(Options *op, Package *p)
{

    int i, ret;

    /* 优先使用环境变量 CC 指定的编译器 */
    op->utils[CC] = getenv("CC");

    ui_expert(op, "Checking development tools:");

    /*
     * 检查所需的工具链组件是否已安装。
     * 注意：如果用户设置了 CC 环境变量，跳过对 cc 的搜索。
     * 这样做是因为即使 PATH 中没有 cc，$CC 指定的编译器也可能工作正常。
     * $CC 的有效性将在下面通过健全性检查来验证。
     */

    for (i = (op->utils[CC] != NULL) ? MIN_DEVELOP_UTILS + 1 : MIN_DEVELOP_UTILS;
         i < MAX_DEVELOP_UTILS; i++) {

        op->utils[i] = find_system_util(__utils[i].util);
        if (!check_development_tool(op, i)) {
            return FALSE;
        }
    }

    /*
     * 检查 libc 开发头文件是否安装。
     * 构建编译器版本检测工具时需要 stdio.h。
     */
    if (access("/usr/include/stdio.h", F_OK) == -1) {
        ui_error(op, "You do not appear to have libc header files "
                 "installed on your system.  Please install your "
                 "distribution's libc development package.");
        return FALSE;
    }

    /* 如果没有通过环境变量指定编译器，使用默认的 "cc" */
    if (!op->utils[CC]) op->utils[CC] = "cc";

    /* 执行编译器健全性检查：尝试编译一个简单的测试程序 */
    ui_log(op, "Performing CC sanity check with CC=\"%s\".", op->utils[CC]);

    ret = conftest_sanity_check(op, p->kernel_module_build_directory,
                                "CC", "cc_sanity_check");

    if (ret) return TRUE;

    return FALSE;

} /* check_development_tools() */


/*
 * check_precompiled_kernel_interface_tools() - 检查链接预编译内核接口所需的工具
 *
 * 功能说明:
 *   当使用预编译的内核接口时，不需要完整的编译工具链，只需要链接器（ld）。
 *   此函数检查 ld 是否可用。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - 链接器可用
 *   FALSE - 链接器未找到
 */

int check_precompiled_kernel_interface_tools(Options *op)
{
    /*
     * 使用预编译接口时只需要链接器
     */
    op->utils[LD] = find_system_util(__utils[LD].util);
    return check_development_tool(op, LD);

} /* check_precompiled_kernel_interface_tools() */


/*
 * find_system_util() - 在搜索路径中查找指定的系统工具
 *
 * 功能说明:
 *   构建搜索路径（$PATH + EXTRA_PATH），然后在路径中逐个目录搜索
 *   指定名称的可执行文件。搜索时会检查文件是否为普通文件（或符号链接
 *   指向的普通文件）且具有可执行权限。
 *
 * 参数:
 *   util - 要搜索的工具名称（如 "ldconfig"、"modprobe" 等）
 *
 * 返回值:
 *   成功时返回工具的完整路径（如 "/sbin/ldconfig"），调用者需释放内存
 *   未找到时返回 NULL
 */

char *find_system_util(const char *util)
{
    char *buf, *path, *file, *x, *y, c;

    /* 构建搜索路径：$PATH + EXTRA_PATH */

    buf = getenv("PATH");
    if (buf) {
        path = nvstrcat(buf, ":", EXTRA_PATH, NULL);
    } else {
        path = nvstrdup(EXTRA_PATH);
    }

    /* 遍历搜索路径中的每个目录，查找目标工具 */

    for (x = y = path; ; x++) {
        if (*x == ':' || *x == '\0') {
            struct stat st;

            /* 临时将分隔符替换为 NUL 来提取目录名 */
            c = *x;
            *x = '\0';
            file = nvstrcat(y, "/", util, NULL);
            *x = c;

            /*
             * 检查文件是否存在、是否为普通文件（stat 会跟随符号链接），
             * 以及是否具有可执行权限（用户/组/其他中至少一个）
             */
            if (stat(file, &st) == 0 && S_ISREG(st.st_mode) &&
                (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) {
                nvfree(path);
                return file;
            }
            nvfree(file);
            y = x + 1;
            if (*x == '\0') break;
        }
    }

    nvfree(path);

    return NULL;

} /* find_system_util() */



/*
 * continue_after_error() - 报告错误并询问用户是否继续
 *
 * 功能说明:
 *   当安装过程中遇到非致命错误时，向用户显示错误信息，并提供
 *   "继续"或"中止"的选择。默认选择为"继续"。
 *
 * 参数:
 *   op  - 全局选项结构体指针
 *   fmt - 错误消息的格式字符串（printf 风格）
 *   ... - 格式字符串的可变参数
 *
 * 返回值:
 *   TRUE  - 用户选择继续安装
 *   FALSE - 用户选择中止安装
 */

int continue_after_error(Options *op, const char *fmt, ...)
{
    char *msg;
    int ret;

    /* 使用可变参数格式化错误消息 */
    NV_VSNPRINTF(msg, fmt);

    /* 显示多选对话框（继续/中止），默认为继续 */
    ret = (ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                              NUM_CONTINUE_ABORT_CHOICES,
                              CONTINUE_CHOICE, /* 默认选项 */
                              "The installer has encountered the following "
                              "error during installation: '%s'.  Would you "
                              "like to continue installation anyway?",
                              msg) == CONTINUE_CHOICE);

    nvfree(msg);

    return ret;

} /* continue_after_error() */



/*
 * do_install() - 执行安装操作
 *
 * 功能说明:
 *   根据命令列表（CommandList）执行驱动文件的安装操作。命令列表包含了
 *   需要执行的所有文件复制、符号链接创建等操作。安装过程中会显示进度。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *   p  - 安装包信息（包含描述和版本号，用于显示安装进度消息）
 *   c  - 命令列表，包含所有待执行的安装操作
 *
 * 返回值:
 *   TRUE  - 安装成功完成
 *   FALSE - 安装过程中出错
 */

int do_install(Options *op, Package *p, CommandList *c)
{
    char *msg;
    int len, ret;

    /* 构造安装进度消息（如 "Installing 'NVIDIA Accelerated Graphics Driver' (535.xx):"） */
    len = strlen(p->description) + strlen(p->version) + 64;
    msg = (char *) nvalloc(len);
    snprintf(msg, len, "Installing '%s' (%s):",
             p->description, p->version);

    /* 执行命令列表中的所有安装操作 */
    ret = execute_command_list(op, c, msg, "Installing");

    free(msg);

    if (!ret) return FALSE;

    ui_log(op, "Driver file installation is complete.");

    return TRUE;

} /* do_install() */



/*
 * extract_version_string() - 从字符串中提取 NVIDIA 驱动版本号
 *
 * 功能说明:
 *   从给定的字符串中解析出 NVIDIA 驱动的版本号字符串。
 *   支持两种版本号格式：
 *
 *   1. 新格式：由句点分隔的数字序列，如 "105.17.2"、"535.129.03"
 *      - 长度至少 5 个字符
 *      - 必须被空白字符或括号包围（或位于字符串的开头/结尾）
 *      - 数字之间的句点数量不限
 *
 *   2. 旧格式：X.Y-ZZZZ 格式，如 "1.0-9742"
 *      - 精确匹配模式：数字.数字-四位数字
 *
 *   使用状态机来解析字符串，优先尝试新格式，失败后回退到旧格式。
 *
 * 参数:
 *   str - 包含版本信息的输入字符串，例如：
 *         "NVIDIA UNIX x86 Kernel Module  105.17.2  Fri Dec 15 09:54:45 PST 2006"
 *         "1.0-105917 (105.9.17)"
 *
 * 返回值:
 *   成功时返回新分配的版本号字符串（调用者需要 free()），如 "105.17.2"
 *   失败时返回 NULL
 */

char *extract_version_string(const char *str)
{
    char c, *copiedString, *start, *end, *x, *version = NULL;
    int state;

    if (!str) return NULL;

    /* 复制字符串以便安全修改 */
    copiedString = strdup(str);
    x = copiedString;

    /*
     * 使用状态机在字符串中查找由数字和句点组成的块。
     * 版本字符串必须被空白字符或字符串的开头/结尾包围。
     */

    start = NULL;
    end = NULL;

/* 状态机的四种状态 */
#define STATE_IN_VERSION          0  /* 正在解析版本号中 */
#define STATE_NOT_IN_VERSION      1  /* 当前位置不可能是版本号 */
#define STATE_LOOKING_FOR_VERSION 2  /* 正在寻找版本号的起始位置 */
#define STATE_FOUND_VERSION       3  /* 已找到完整的版本号 */

    state = STATE_LOOKING_FOR_VERSION;

    while (*x) {

        c = *x;

        switch (state) {

            /*
             * LOOKING_FOR_VERSION 状态：
             * - 遇到数字 -> 标记起始位置，进入 IN_VERSION
             * - 遇到空白或左括号 -> 继续查找
             * - 遇到其他字符 -> 进入 NOT_IN_VERSION
             */

        case STATE_LOOKING_FOR_VERSION:
            if (isdigit(c)) {
                start = x;
                state = STATE_IN_VERSION;
            } else if (isspace(c) || (c == '(')) {
                state = STATE_LOOKING_FOR_VERSION;
            } else {
                state = STATE_NOT_IN_VERSION;
            }
            break;

            /*
             * IN_VERSION 状态：
             * - 遇到数字或句点 -> 继续保持在版本号中
             * - 遇到空白或右括号，且已读取至少5个字符 -> 找到完整版本号
             * - 遇到其他字符 -> 之前的内容不是版本号，回退到 NOT_IN_VERSION
             */

        case STATE_IN_VERSION:
            if (isdigit(c) || (c == '.')) {
                state = STATE_IN_VERSION;
            } else if ((isspace(c) || (c == ')')) && ((x - start) >= 5)) {
                end = x;
                state = STATE_FOUND_VERSION;
                goto exit_while_loop;
            } else {
                state = STATE_NOT_IN_VERSION;
            }
            break;

            /*
             * NOT_IN_VERSION 状态：
             * - 遇到空白或左括号 -> 重新开始查找版本号
             * - 遇到其他字符 -> 保持在 NOT_IN_VERSION
             */

        case STATE_NOT_IN_VERSION:
            if (isspace(c) || (c == '(')) {
                state = STATE_LOOKING_FOR_VERSION;
            } else {
                state = STATE_NOT_IN_VERSION;
            }
            break;
        }

        x++;
    }

    /*
     * 如果字符串以版本号结尾（NUL 终止符充当了结束标志），
     * 检查是否构成有效的版本号（至少5个字符）
     */

    if ((state == STATE_IN_VERSION) && ((x - start) >= 5)) {
        end = x;
        state = STATE_FOUND_VERSION;
    }

 exit_while_loop:

    /* 如果通过新格式找到了版本号，复制并返回 */

    if (state == STATE_FOUND_VERSION) {
        *end = '\0';
        version = strdup(start);
        goto done;
    }



    /*
     * 新格式未匹配成功，尝试匹配旧格式 X.Y-ZZZZ（如 "1.0-9742"）
     * 精确匹配模式：一位数字.一位数字-四位数字
     */

    x = copiedString;

    while (*x) {
        if (((x[0]) && isdigit(x[0])) &&
            ((x[1]) && (x[1] == '.')) &&
            ((x[2]) && isdigit(x[2])) &&
            ((x[3]) && (x[3] == '-')) &&
            ((x[4]) && isdigit(x[4])) &&
            ((x[5]) && isdigit(x[5])) &&
            ((x[6]) && isdigit(x[6])) &&
            ((x[7]) && isdigit(x[7]))) {

            x[8] = '\0';

            version = strdup(x);
            goto done;
        }
        x++;
    }

 done:

    free(copiedString);

    return version;

} /* extract_version_string() */


/*
 * should_install_compat32_files() - 处理 32 位兼容库的安装决策
 *
 * 功能说明:
 *   在 x86_64 系统上，询问用户是否安装 32 位兼容库。这些库允许 32 位应用程序
 *   使用 NVIDIA GPU 功能。
 *
 *   处理流程:
 *   1. 检查安装包是否包含 32 位兼容文件
 *   2. 确定 32 位兼容库的安装路径
 *   3. 如果用户未通过命令行指定，则交互式询问
 *   4. 如果用户选择不安装，将包中所有 32 位文件标记为无效
 *
 *   此函数仅在 x86_64 平台上有效（通过 NV_X86_64 条件编译控制）。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *   p  - 安装包信息结构体
 */

void should_install_compat32_files(Options *op, Package *p)
{
#if defined(NV_X86_64)

    /* 如果安装包中没有 32 位兼容文件，无需处理 */
    if (!op->compat32_files_packaged) {
        op->install_compat32_libs = NV_OPTIONAL_BOOL_FALSE;
        return;
    }

    /* 确定 32 位兼容库的安装目标路径 */
    get_compat32_path(op);

    /*
     * 如果用户没有通过命令行参数明确指定是否安装 32 位兼容库，
     * 则交互式询问用户。默认建议安装（TRUE）。
     */
    if (op->install_compat32_libs == NV_OPTIONAL_BOOL_DEFAULT) {
        int ret;

        ret = ui_yes_no(op, TRUE,
                        "Install NVIDIA's 32-bit compatibility libraries?");

        op->install_compat32_libs = ret ? NV_OPTIONAL_BOOL_TRUE :
                                          NV_OPTIONAL_BOOL_FALSE;
    }

    /* 如果不安装 32 位兼容库，将包中所有 compat32 文件标记为无效 */
    if (op->install_compat32_libs == NV_OPTIONAL_BOOL_FALSE) {
        int i;

        for (i = 0; i < p->num_entries; i++) {
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                invalidate_package_entry(&(p->entries[i]));
            }
        }
    }
#endif /* NV_X86_64 */
}


/*
 * member_at_offset 宏 - 按字节偏移访问结构体成员
 *
 * 通过基地址 + 字节偏移的方式访问结构体中的成员，并转换为指定的目标类型。
 * 用于动态访问 Options 结构体中的可选模块安装标志字段。
 */
#define member_at_offset(base, offset, target_type) \
    ((target_type *) ((char *) base + offset))


/*
 * set_optional_module_install() - 通过字节偏移设置可选模块的安装标志
 *
 * 参数:
 *   op     - Options 结构体基地址
 *   offset - 目标字段在 Options 中的字节偏移量
 *   val    - 要设置的值（TRUE 或 FALSE）
 */
static void set_optional_module_install(Options *op, int offset, int val) {
    *member_at_offset(op, offset, int) = val;
}

/*
 * get_optional_module_install() - 通过字节偏移获取可选模块的安装标志
 *
 * 参数:
 *   op     - Options 结构体基地址
 *   offset - 目标字段在 Options 中的字节偏移量
 *
 * 返回值:
 *   该字段的当前值（TRUE 或 FALSE）
 */
static int get_optional_module_install(Options *op, int offset) {
    return *member_at_offset(op, offset, int);
}

/*
 * should_install_optional_modules() - 处理可选内核模块的安装决策
 *
 * 功能说明:
 *   遍历所有可选内核模块（如 nvidia-uvm、nvidia-drm 等），检查安装包中
 *   是否包含该模块，并在专家模式下询问用户是否安装。如果用户选择不安装
 *   某个模块，会显示警告信息（说明哪些功能将不可用），并从安装包中移除
 *   该模块。
 *
 * 参数:
 *   op                   - 全局选项结构体指针
 *   p                    - 安装包信息结构体
 *   optional_modules     - 可选模块信息数组（KernelModuleInfo 结构体）
 *   num_optional_modules - 可选模块的数量
 */

void should_install_optional_modules(Options *op, Package *p,
                                     const KernelModuleInfo* optional_modules,
                                     int num_optional_modules)
{
    int i;

    for (i = 0; i < num_optional_modules; i++) {
        /* 获取当前模块的安装标志（通过偏移量动态访问 Options 中的字段） */
        int install = get_optional_module_install(op,
                          optional_modules[i].option_offset);

        /* 如果安装包中不包含该模块，则无法安装，标记为 FALSE 并跳过 */

        if (!package_includes_kernel_module(p,
                                            optional_modules[i].module_name)) {
            set_optional_module_install(op, optional_modules[i].option_offset,
                                        FALSE);
            continue;
        }

        /* 在专家模式下，询问用户是否安装该可选模块 */

        if (op->expert) {
            int default_value = install;
            install = ui_yes_no(op, default_value, "Would you like to install "
                                "the %s kernel module? You must install "
                                "this module in order to use %s.",
                                optional_modules[i].module_name,
                                optional_modules[i].optional_module_dependee);
            if (install != default_value) {
                set_optional_module_install(op,
                                            optional_modules[i].option_offset,
                                            install);
            }
        }

        if (!install) {
            ui_warn(op, "The %s module will not be installed. As a result, %s "
                    "will not function with this installation of the NVIDIA "
                    "driver.", optional_modules[i].module_name,
                    optional_modules[i].optional_module_dependee);

            remove_kernel_module_from_package(p,
                                              optional_modules[i].module_name);
        }
    }
}


/*
 * check_installed_files_from_package() - 安装后完整性检查
 *
 * 功能说明:
 *   遍历安装包中的所有条目，验证每个文件和符号链接是否已正确安装到目标位置。
 *   对于符号链接，检查链接是否存在且指向正确的目标。
 *   对于普通文件，检查文件是否存在、权限是否正确。
 *
 *   此函数在驱动文件安装完成后运行，作为安装后的健全性检查。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *   p  - 安装包信息结构体，包含所有已安装的文件条目
 */

void check_installed_files_from_package(Options *op, Package *p)
{
    int i, ret = TRUE;
    float percent;
    PackageEntryFileTypeList installable_files;

    ui_status_begin(op, "Running post-install sanity check:", "Checking");

    /* 获取可安装文件类型列表，用于确定哪些文件类型需要检查 */
    get_installable_file_type_list(op, &installable_files);

    for (i = 0; i < p->num_entries; i++) {

        /* 更新进度条 */
        percent = (float) i / (float) p->num_entries;
        ui_status_update(op, percent, "%s", p->entries[i].dst);

        if (p->entries[i].caps.is_symlink) {
            /* 检查符号链接：验证链接是否存在且指向正确的目标 */
            if (!check_symlink(op, p->entries[i].target,
                               p->entries[i].dst,
                               p->description)) {
                ret = FALSE;
            }
        } else if (installable_files.types[p->entries[i].type]) {
            /* 检查普通文件：验证文件是否存在、权限是否正确 */
            if (!check_installed_file(op, p->entries[i].dst,
                                      p->entries[i].mode, 0, ui_warn)) {
                ret = FALSE;
            }
        }
    }

    ui_status_end(op, "done.");
    ui_log(op, "Post-install sanity check %s.", ret ? "passed" : "failed");

} /* check_installed_files_from_package() */



/*
 * check_symlink() - 检查符号链接是否正确
 *
 * 功能说明:
 *   验证指定的符号链接是否存在，以及是否指向正确的目标文件。
 *   如果符号链接不存在或指向错误的目标，会打印描述性的警告信息，
 *   并提供手动修复的命令建议。
 *
 * 参数:
 *   op     - 全局选项结构体指针
 *   target - 符号链接应该指向的目标路径
 *   link   - 符号链接的路径
 *   descr  - 对该符号链接用途的描述（用于错误消息中）
 *
 * 返回值:
 *   TRUE  - 符号链接存在且指向正确的目标
 *   FALSE - 符号链接不存在或指向错误
 */

static int check_symlink(Options *op, const char *target, const char *link,
                         const char *descr)
{
    int success = TRUE;
    char *actual_target;

    /* 获取符号链接当前指向的实际目标 */
    actual_target = get_symlink_target(op, link);
    if (!actual_target) {
        ui_warn(op, "The symbolic link '%s' does not exist.  This is "
                "necessary for correct operation of the %s.  You can "
                "create this symbolic link manually by executing "
                "`ln -sf %s %s`.",
                link,
                descr,
                target,
                link);
        return FALSE;
    } 

    if (strcmp(actual_target, target) != 0) {
        ui_warn(op, "The symbolic link '%s' does not point to '%s' "
                "as is necessary for correct operation of the %s.  "
                "It is possible that `ldconfig` has created this "
                "incorrect symbolic link because %s's "
                "\"soname\" conflicts with that of %s.  It is "
                "recommended that you remove or rename the file "
                "'%s' and create the necessary symbolic link by "
                "running `ln -sf %s %s`.",
                link,
                target,
                descr,
                actual_target,
                target,
                actual_target,
                target,
                link);
        success = FALSE;
    }

    nvfree(actual_target);
    return success;
}



/*
 * unprelink() - 对文件执行 prelink -u 以还原到预链接之前的状态
 *
 * 功能说明:
 *   prelink 是一个可以加速动态链接库加载的工具，但它会修改 ELF 文件的内容，
 *   导致 CRC 校验值变化。此函数尝试对文件执行 `prelink -u` 来撤销
 *   预链接操作，恢复文件的原始内容和校验值。
 *
 * 参数:
 *   op       - 全局选项结构体指针
 *   filename - 需要还原的文件路径
 *
 * 返回值:
 *   0     - 成功执行 prelink -u
 *   ENOENT - 未找到 prelink 工具
 *   其他  - prelink 命令执行失败的返回值
 */
static int unprelink(Options *op, const char *filename)
{
    char *cmd;
    int ret = ENOENT;

    cmd = find_system_util("prelink");
    if (cmd) {
        ret = run_command(op, NULL, FALSE, NULL, TRUE, cmd, " -u ", filename, NULL);
        nvfree(cmd);
    }
    return ret;
} /* unprelink() */



/*
 * verify_crc() - CRC 校验验证
 *
 * 功能说明:
 *   计算指定文件的 CRC 校验值，并与预期值进行比较。
 *   用于验证已安装的文件是否被修改过。
 *
 * 参数:
 *   op         - 全局选项结构体指针
 *   filename   - 要验证的文件路径
 *   crc        - 预期的 CRC 值。如果为 0，跳过检查直接返回 TRUE
 *   actual_crc - 输出参数，存储文件实际计算得到的 CRC 值
 *
 * 返回值:
 *   TRUE  - CRC 值匹配（或预期值为 0，跳过检查）
 *   FALSE - CRC 值不匹配
 */
int verify_crc(Options *op, const char *filename, unsigned int crc,
                      unsigned int *actual_crc)
{
    /* 预期 CRC 值为 0 表示不需要检查 */
    if (crc == 0) {
        return TRUE;
    }
    *actual_crc = compute_crc(op, filename);
    return crc == *actual_crc;
} /* verify_crc() */



/*
 * check_installed_file() - 检查已安装文件的完整性
 *
 * 功能说明:
 *   对指定的已安装文件进行多项检查：
 *   1. 文件是否存在
 *   2. 文件类型是否为普通文件
 *   3. 文件权限是否正确
 *   4. CRC 校验值是否正确
 *
 *   如果 CRC 校验失败且文件是 ELF 文件，会尝试通过 prelink -u 还原文件，
 *   然后重新校验。这是因为 prelink 会修改 ELF 文件内容导致 CRC 变化。
 *
 * 参数:
 *   op       - 全局选项结构体指针
 *   filename - 要检查的文件路径
 *   mode     - 预期的文件权限。如果为 0，跳过权限检查（备份日志中可能不保存权限）
 *   crc      - 预期的 CRC 值。如果为 0，跳过 CRC 检查
 *   logwarn  - 错误报告函数指针（ui_log 或 ui_warn），控制错误消息的严重级别
 *
 * 返回值:
 *   TRUE  - 文件检查全部通过
 *   FALSE - 某项检查失败
 */

int check_installed_file(Options *op, const char *filename,
                         const mode_t mode, const uint32 crc,
                         ui_message_func *logwarn)
{
    struct stat stat_buf;
    uint32 actual_crc;

    /* 检查文件是否存在（使用 lstat 避免跟随符号链接） */
    if (lstat(filename, &stat_buf) == -1) {
        logwarn(op, "Unable to find installed file '%s' (%s).",
                filename, strerror(errno));
        return FALSE;
    }

    if (!S_ISREG(stat_buf.st_mode)) {
        logwarn(op, "The installed file '%s' is not of the correct filetype.",
                filename);
        return FALSE;
    }

    /* 如果没有预期的权限值则跳过权限检查。
       备份日志中的条目可能不保存文件权限。 */

    if (mode && ((stat_buf.st_mode & PERM_MASK) != (mode & PERM_MASK))) {
        logwarn(op, "The installed file '%s' has permissions %04o, but it "
                "was installed with permissions %04o.", filename,
                (stat_buf.st_mode & PERM_MASK),
                (mode & PERM_MASK));
        return FALSE;
    }


    /* CRC 校验验证 */
    if (!verify_crc(op, filename, crc, &actual_crc)) {
        int ret;

        /* 如果不是 ELF 文件，不尝试反预链接操作 */

        if (get_elf_architecture(filename) == ELF_INVALID_FILE) {
            logwarn(op, "The installed file '%s' has a different checksum "
                    "(%ul) than when it was installed (%ul).", filename,
                    actual_crc, crc);
            return FALSE;
        }

        /* Otherwise, unprelinking may be able to restore the original file. */

        ui_expert(op, "The installed file '%s' has a different checksum (%ul) "
                  "than when it was installed (%ul). This may be due to "
                  "prelinking; attempting `prelink -u %s` to restore the file.",
                  filename, actual_crc, crc, filename);

        ret = unprelink(op, filename);
        if (ret != 0) {
            logwarn(op, "The installed file '%s' seems to have changed, but "
                    "`prelink -u` failed; unable to restore '%s' to an "
                    "un-prelinked state.", filename, filename);
            return FALSE;
        }

        if (!verify_crc(op, filename, crc, &actual_crc)) {
            logwarn(op, "The installed file '%s' has a different checksum "
                    "(%ul) after running `prelink -u` than when it was "
                    "installed (%ul).",
                    filename, actual_crc, crc);
            return FALSE;
        }

        ui_expert(op, "Un-prelinking successful: %s was restored to its "
                  "original state.", filename);
    }

    return TRUE;
    
}



/*
 * get_xserver_information() - 从 X Server 版本字符串中解析版本信息
 *
 * 功能说明:
 *   解析 `X -version` 命令输出中的版本字符串，推断 X Server 的类型和能力：
 *   1. 判断是否为模块化 X Server（modular vs monolithic）
 *   2. 判断是否支持 OutputClass 配置段（X.Org 1.16+ 支持）
 *
 *   不支持 XFree86 X Server（已过时）。
 *
 * 参数:
 *   op                         - 全局选项结构体指针
 *   versionString              - X -version 命令的输出文本
 *   isModular                  - 输出参数：TRUE = 模块化 X Server
 *   supportsOutputClassSection - 输出参数：TRUE = 支持 OutputClass 段
 *
 * 返回值:
 *   TRUE  - 成功解析版本信息
 *   FALSE - 解析失败（不支持的 X Server 或无法识别的版本格式）
 */

static int get_xserver_information(Options *op,
                                   const char *versionString,
                                   int *isModular,
                                   int *supportsOutputClassSection)
{
/* X Server 版本字符串的两种可能格式 */
#define XSERVER_VERSION_FORMAT_1 "X Window System Version"
#define XSERVER_VERSION_FORMAT_2 "X.Org X Server"

    int major, minor, found;
    const char *ptr;

    /* 检查是否为 XFree86 X Server（不支持） */

    if (strstr(versionString, "XFree86 Version")) {
        ui_error(op, "XFree86 is not supported.");
        return FALSE;
    }


    /*
     * 确认为 X.Org X Server，尝试从字符串中解析 major.minor 版本号
     */

    found = FALSE;

    if (((ptr = strstr(versionString, XSERVER_VERSION_FORMAT_1)) != NULL) &&
        (sscanf(ptr, XSERVER_VERSION_FORMAT_1 " %d.%d", &major, &minor) == 2)) {
        found = TRUE;
    }

    if (!found &&
        ((ptr = strstr(versionString, XSERVER_VERSION_FORMAT_2)) != NULL) &&
        (sscanf(ptr, XSERVER_VERSION_FORMAT_2 " %d.%d", &major, &minor) == 2)) {
        found = TRUE;
    }

    /* 如果无法解析版本号，放弃 */

    if (!found) return FALSE;

    /*
     * 判断是否为模块化 X Server：
     * X.Org X11R6.x 系列是单体式（monolithic），其他都是模块化的（modular）
     */

    if (major == 6) {
        *isModular = FALSE;
    } else {
        *isModular = TRUE;
    }

    /*
     * OutputClass 配置段支持：
     * 用于自动匹配驱动到平台设备的功能，在 X.Org xserver 1.16 中引入。
     * major=6（X11R6.x）、major=7（X11R7.x）、或 major=1 且 minor<16
     * 的版本不支持 OutputClass。
     */
    if ((major == 6) || (major == 7) || ((major == 1) && (minor < 16))) {
        *supportsOutputClassSection = FALSE;
    } else {
        *supportsOutputClassSection = TRUE;
    }

    return TRUE;

} /* get_xserver_information() */



/*
 * query_xorg_version() - 查询并解析 X.Org 版本信息
 *
 * 功能说明:
 *   运行 `X -version` 命令获取 X Server 版本信息，并解析以推断
 *   X Server 的类型和功能。设置以下 Options 字段：
 *     - op->modular_xorg: 是否为模块化 X Server
 *     - op->xorg_supports_output_class: 是否支持 OutputClass 配置段
 *
 *   如果无法确定版本（X Server 未找到、命令失败等），默认假设
 *   X Server 是模块化的但不支持 OutputClass。
 *
 * 参数:
 *   op - 全局选项结构体指针
 */

/* X 协议版本格式字符串（用于解析参考，实际解析在 get_xserver_information 中） */
#define OLD_VERSION_FORMAT "(protocol Version %d, revision %d, vendor release %d)"
#define NEW_VERSION_FORMAT "X Protocol Version %d, Revision %d, Release %d."

void query_xorg_version(Options *op)
{
    char *data = NULL;
    int ret = FALSE;

    /* 如果没有找到 X Server 可执行文件，跳过查询 */
    if (!op->utils[XSERVER])
        goto done;

    /* 执行 `X -version` 命令并捕获输出 */
    if (run_command(op, &data, FALSE, NULL, TRUE,
                    op->utils[XSERVER], " -version", NULL) ||
        (data == NULL)) {
        goto done;
    }

    /*
     * 解析 `X -version` 的输出，推断 X Server 的类型和功能
     */

    ret = get_xserver_information(op, data, &op->modular_xorg,
                                  &op->xorg_supports_output_class);

done:

    /*
     * 如果未找到 X Server、命令执行失败或版本解析失败，
     * 使用安全的默认值：模块化 X Server，不支持 OutputClass
     */

    if (!ret) {
        op->modular_xorg = TRUE;
        op->xorg_supports_output_class = FALSE;
    }

    nvfree(data);
}


/*
 * check_for_running_x() - 检测是否有正在运行的 X Server
 *
 * 功能说明:
 *   运行任何 X Server（即使使用非 NVIDIA 驱动）都可能在安装 NVIDIA 驱动
 *   期间造成稳定性问题。此函数通过扫描 X Server 的锁文件来检测是否有
 *   X Server 正在运行。
 *
 *   检测方法:
 *   1. 扫描 /tmp/.X0-lock 到 /tmp/.X7-lock（对应 Display 0-7）
 *   2. 读取锁文件中的 PID
 *   3. 检查 /proc/<PID> 是否存在（确认进程是否仍在运行）
 *
 *   如果检测到运行中的 X Server：
 *   - 设置 op->running_x_server_detected = TRUE
 *   - 如果 --no-x-check 选项已设置，继续安装
 *   - 否则询问用户是否继续（默认为中止）
 *
 *   特殊情况：如果仅安装内核模块且目标是非当前运行的内核，跳过此检查。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - 未检测到 X Server，或用户选择继续
 *   FALSE - 检测到 X Server 且用户选择中止
 */

int check_for_running_x(Options *op)
{
    char path[14], *buf;
    char procpath[17]; /* /proc/%d 格式，足够容纳 32 位 PID 值 */
    int i, pid;

    /*
     * 如果仅安装内核模块且目标是非当前运行的内核，
     * 跳过 X Server 检测（因为不会影响运行中的系统）
     */

    if (op->kernel_modules_only && op->kernel_name) {
        ui_log(op, "Only installing kernel modules for a non-running "
               "kernel; skipping the \"is an X server running?\" test.");
        return TRUE;
    }
    
    /* 扫描 Display 0 到 Display 7 的锁文件 */
    for (i = 0; i < 8; i++) {
        int ret;
        ret = snprintf(path, 14, "/tmp/.X%1d-lock", i);
        if (ret < 0)
        {
            ui_warn(op, "Failed to determine presence of X lock file");
            return TRUE;
        }

        if (read_text_file(path, &buf) == TRUE) {
            int num = sscanf(buf, "%d", &pid);
            nvfree(buf);
            if (num != 1) {
                ui_warn(op, "Failed to read a pid from X lock file '%s'", path);
                return TRUE;
            }
            snprintf(procpath, 17, "/proc/%d", pid);
            if (access(procpath, F_OK) == 0) {
                ui_log(op, "The file '%s' exists and appears to contain the "
                           "process ID '%d' of a running X server.", path, pid);
                if (op->no_x_check) {
                    ui_log(op, "Continuing per the '--no-x-check' option.");
                } else {
                    int choice = ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                        NUM_CONTINUE_ABORT_CHOICES, ABORT_CHOICE,
                        "You appear to be running an X server.  Installing the "
                        "NVIDIA driver while X is running is not recommended, "
                        "as doing so may prevent the installer from detecting "
                        "some potential installation problems, and it may not "
                        "be possible to start new graphics applications after "
                        "a new driver is installed.  If you choose to continue "
                        "installation, it is highly recommended that you "
                        "reboot your computer after installation to use the "
                        "newly installed driver.");
                    if (choice == CONTINUE_CHOICE) {
                        op->running_x_server_detected = TRUE;
                    } else {
                        return FALSE;
                    }
                }

                /* We found a running X server; no need to check for others. */
                break;
            }
        }
    }
    
    return TRUE;

} /* check_for_running_x() */

/*
 * is_vgpu_host_package() - 检测当前安装包是否为 vGPU 宿主机驱动包
 *
 * 功能说明:
 *   通过以下两种方式判断当前安装包是否为 vGPU（虚拟 GPU）宿主机版本：
 *   1. 检查包内是否存在 "is_vgpu_host_package.txt" 标记文件
 *   2. 检查是否设置了以下环境变量（用于内部构建版本的安装）：
 *      - VGX_BUILD: Xenserver 平台的 vGPU 宿主机驱动
 *      - VGX_KVM_BUILD: KVM 平台的 vGPU 宿主机驱动
 *      - VGX_DEVICE_VM_BUILD: Device VM 的 vGPU 宿主机驱动
 *
 * 返回值:
 *   TRUE  - 这是 vGPU 宿主机安装包
 *   FALSE - 这不是 vGPU 宿主机安装包
 */
static int is_vgpu_host_package(void)
{
    if (access("./is_vgpu_host_package.txt", F_OK) == 0) {
        return TRUE;
    }

    if (getenv("VGX_BUILD") != NULL) {
        return TRUE;
    }

    if (getenv("VGX_KVM_BUILD") != NULL) {
        return TRUE;
    }

    if (getenv("VGX_DEVICE_VM_BUILD") != NULL) {
        return TRUE;
    }

    return FALSE;
}

/*
 * nvpci_dev_is_vgpu_gsp() - 检查 vGPU 宿主机上的 GPU 是否支持 GSP（GPU 系统处理器）
 *
 * 功能说明:
 *   在 vGPU 宿主机场景下，判断指定的 GPU 设备是否支持 GSP。
 *   GSP 是较新 GPU 上的固件处理器，支持 GSP 的 GPU 必须使用
 *   开源内核模块（open kernel modules）。
 *
 *   此函数维护了一个不支持 GSP 的老旧 GPU 设备 ID 列表（如 Tesla M10、
 *   Tesla P40、Tesla V100 等），如果设备 ID 在该列表中则返回 FALSE。
 *
 * 参数:
 *   device_id - PCI 设备 ID
 *
 * 返回值:
 *   TRUE  - 该设备在 vGPU 宿主机上支持 GSP
 *   FALSE - 该设备在 vGPU 宿主机上不支持 GSP，或当前不是 vGPU 宿主机包
 */
static int nvpci_dev_is_vgpu_gsp(unsigned int device_id)
{
    /* 不支持 GSP 的 vGPU 宿主机 GPU 设备 ID 列表 */
    unsigned short vgpu_non_gsp_dev_ids[] = {
        0x13bd, // Tesla M10,
        0x13f2, // Tesla M60
        0x13f3, // Tesla M6
        0x15f7, // Tesla P100-PCIE-12GB
        0x15f8, // Tesla P100-PCIE-16GB
        0x15f9, // Tesla P100-SXM2-16GB
        0x1b38, // Tesla P40
        0x1bb3, // Tesla P4
        0x1bb4, // Tesla P6
        0x1db1, // Tesla V100-SXM2-16GB
        0x1db3, // Tesla V100-FHHL-16GB
        0x1db4, // Tesla V100-PCIE-16GB
        0x1db5, // Tesla V100-SXM2-32GB
        0x1db6, // Tesla V100-PCIE-32GB
        0x1df6, // Tesla V100S-PCIE-32GB,
        0x1e30, // Quadro RTX 8000, Quadro RTX 6000,
        0x1e37, // PG150 SKU220, PG150 SKU215,
        0x1e78, // Quadro RTX 8000, Quadro RTX 6000,
        0x1eb8, // Tesla T4
        0x20b0, // NVIDIA A100-SXM4-40GB
        0x20b2, // NVIDIA A100-SXM4-80GB
        0x20b5, // NVIDIA A100-PCIE-80GB, A100-PCIe-80GB LC,
        0x20b7, // NVIDIA A30
        0x20b8, // NVIDIA A100X,
        0x20b9, // NVIDIA A30X,
        0x20f1, // NVIDIA A100-PCIE-40GB
        0x20f3, // NVIDIA A800-SXM4-80GB
        0x20f5, // NVIDIA A800 80GB PCIe
        0x20f6, // NVIDIA A800 PCIe 40GB Active,
        0x20fd, // NVIDIA AX800,
        0x2230, // NVIDIA RTX A6000
        0x2231, // NVIDIA RTX A5000
        0x2233, // NVIDIA RTX A5500,
        0x2235, // NVIDIA A40
        0x2236, // NVIDIA A10
        0x2237, // NVIDIA A10G
        0x2238, // NVIDIA A10M,
        0x25b6, // NVIDIA A16, NVIDIA A2
    };

    /* 仅在 vGPU 宿主机包的情况下进行检查 */
    if (is_vgpu_host_package()) {
        int i;

        /* 如果设备 ID 在不支持 GSP 的列表中，返回 FALSE */
        for (i = 0; i < ARRAY_LEN(vgpu_non_gsp_dev_ids); i++) {
            if (device_id == vgpu_non_gsp_dev_ids[i]) {
                return FALSE;
            }
        }
        /* 不在排除列表中，认为支持 GSP */
        return TRUE;
    }
    /* 非 vGPU 宿主机包，此函数不适用 */
    return FALSE;
}


/*
 * pci_dev_get_gpu_flags() - 获取指定 PCI 设备的 GPU 特性标志
 *
 * 功能说明:
 *   通过 PCI 设备信息查找 GPU 的特性标志。优先使用四部分 ID 匹配
 *   （device_id + subvendor_id + subdevice_id），如果未匹配到，
 *   再使用两部分 ID 匹配（仅 device_id）。
 *
 *   特性标志包含 GPU_FLAGS_NO_GSP、GPU_FLAGS_VGPU_GUEST 等信息。
 *
 * 参数:
 *   dev - libpciaccess 的 PCI 设备结构体指针
 *
 * 返回值:
 *   GPU 特性标志的位掩码，如果未匹配到任何记录则返回 0
 */
static unsigned short pci_dev_get_gpu_flags(const struct pci_device *dev)
{
    int i;

    /* 优先使用四部分 ID（含子设备 ID）进行精确匹配 */
    for (i = 0; i < ARRAY_LEN(GpuSubDeviceFlagList); i++) {
        if (dev->device_id == GpuSubDeviceFlagList[i].devId &&
            dev->subvendor_id == GpuSubDeviceFlagList[i].subVendorId &&
            dev->subdevice_id == GpuSubDeviceFlagList[i].subDevId) {
            return GpuSubDeviceFlagList[i].flags;
        }
    }

    /* 回退到两部分 ID（仅 device_id）匹配 */
    for (i = 0; i < ARRAY_LEN(GpuFlagList); i++) {
        if (dev->device_id == GpuFlagList[i].devId) {
            return GpuFlagList[i].flags;
        }
    }

    return 0;
}


/*
 * pci_devid_gsp_unlikely() - 判断未知设备 ID 是否不太可能支持 GSP
 *
 * 功能说明:
 *   某些设备 ID 范围（0x1340 - 0x1DFF）中的 GPU 虽然不在已知支持列表中，
 *   但可能实际存在。这个范围内的设备更可能是不支持 GSP 的老旧 GPU，
 *   因此假定它们不支持 GSP。
 *
 * 参数:
 *   devid - PCI 设备 ID
 *
 * 返回值:
 *   非零（TRUE） - 该设备 ID 不太可能支持 GSP
 *   0（FALSE）   - 该设备 ID 不在"不太可能"范围内
 */
static int pci_devid_gsp_unlikely(unsigned short devid)
{
    return devid >= 0x1340 && devid < 0x1E00;
}


/*
 * pci_device_scan() - PCI 设备扫描
 *
 * 功能说明:
 *   使用 libpciaccess 库遍历系统中所有的 NVIDIA PCI GPU 设备，
 *   识别旧版（legacy）设备和支持特定功能（如 GSP）的设备。
 *   扫描结果存储在 op->pci_devices 和 op->open_modules 中，
 *   用于后续安装过程中的决策和提示信息。
 *
 *   主要检测内容:
 *   - 是否存在旧版 GPU（需要使用旧版驱动）
 *   - 是否存在当前版本驱动支持的 GPU
 *   - 是否有 VGA 类型的 GPU（用于决定是否配置 X Server）
 *   - GPU 是否为自托管设备（如 Grace Hopper，必须使用开源内核模块）
 *   - GPU 是否支持 GSP（影响开源/闭源内核模块的选择）
 *
 * 参数:
 *   op - 全局选项结构体指针
 */
void pci_device_scan(Options *op)
{
    struct pci_device_iterator *iter;
    struct pci_device *dev;

    /* 初始化 libpciaccess 库 */
    if (pci_system_init()) {
        return;
    }

    /* 遍历所有 NVIDIA 厂商 ID 的 GPU 设备 */
    iter = nvpci_find_gpu_by_vendor(NV_PCI_VENDOR_ID);

    for (dev = pci_device_next(iter); dev; dev = pci_device_next(iter)) {
        if (dev->device_id >= 0x0020 /* TNT 或更新的 GPU */) {
            int match = -1;
            int i;

            /*
             * 首先检查该 GPU 是否为"旧版"（legacy）设备。
             * 如果是，将其添加到已检测的旧版设备列表中。
             *
             * LegacyList 的结构说明：
             * - 对于名称唯一的设备，只有一行记录（subdevice/subvendor ID 为 0）
             * - 如果同一 device_id 下有不同名称的子设备，才会有包含完整
             *   四部分 ID 的额外行
             *
             * 查找逻辑：
             * - 先匹配两部分 ID（device_id），记录匹配位置
             * - 继续搜索是否有更精确的四部分 ID 匹配（不同名称的子设备）
             * - 使用最后匹配到的索引
             */
            for (i = 0; i < ARRAY_LEN(LegacyList); i++) {
                if (dev->device_id == LegacyList[i].uiDevId) {
                    int found_specific =
                        (dev->subvendor_id == LegacyList[i].uiSubVendorId &&
                         dev->subdevice_id == LegacyList[i].uiSubDevId);

                    if (found_specific || LegacyList[i].uiSubDevId == 0) {
                        match = i;
                    }

                    if (found_specific) {
                        break;
                    }
                }
            }

            /* 非负索引表示在 LegacyList 中找到了匹配；否则为非旧版设备 */
            if (match >= 0) {
                int already_matched = FALSE;

                if (op->pci_devices.num_legacy >=
                    ARRAY_LEN(op->pci_devices.legacy)) {
                    continue;
                }

                for (i = 0; i < op->pci_devices.num_legacy; i++) {
                    if (match == op->pci_devices.legacy[i]) {
                        already_matched = TRUE;
                        break;
                    }
                }

                if (!already_matched) {
                    op->pci_devices.legacy[op->pci_devices.num_legacy] = match;
                    op->pci_devices.num_legacy++;
                }
            } else {
                /* 非旧版设备：标记为当前驱动支持的 GPU */
                op->pci_devices.found_supported = TRUE;

                /* 检查是否为 VGA 类型设备（影响是否询问 nvidia-xconfig 配置） */
                if (nvpci_dev_is_vga(dev)) {
                    op->pci_devices.found_vga = TRUE;
                }

                /*
                 * 根据 GPU 类型确定开源内核模块（open kernel modules）的需求：
                 */
                if (pci_devid_is_self_hosted(dev->device_id)) {
                    /* 自托管 GPU（如 Grace Hopper）必须使用开源内核模块 */
                    op->open_modules.required = TRUE;
                    op->open_modules.supported_gpu_present = TRUE;
                } else if (is_vgpu_host_package()) {
                    /* vGPU 宿主机上：支持 GSP 的 GPU 必须用开源模块，
                     * 不支持 GSP 的 GPU 不能用开源模块 */
                    if (nvpci_dev_is_vgpu_gsp(dev->device_id)) {
                        op->open_modules.required = TRUE;
                        op->open_modules.supported_gpu_present = TRUE;
                    } else {
                        op->open_modules.unsupported_gpu_present = TRUE;
                    }
                } else {
                    /* 普通场景：默认假设设备支持 GSP，除非标志位另有说明 */
                    int device_has_gsp = TRUE;
                    unsigned short flags = pci_dev_get_gpu_flags(dev);

                    if (flags & GPU_FLAGS_NO_GSP ||
                        ((flags & GPU_FLAGS_VGPU_GUEST) &&
                         (flags & GPU_FLAGS_VGPU_NO_GSP))) {
                        device_has_gsp = FALSE;
                    }

                    if (flags == 0 && pci_devid_gsp_unlikely(dev->device_id)) {
                        ui_warn(op, "The unknown GPU with PCI device ID 0x%04X "
                                "is unlikely to support the open GPU kernel "
                                "modules; assuming they are unsupported for "
                                "this device.", dev->device_id);
                        device_has_gsp = FALSE;
                    }

                    if (device_has_gsp) {
                        op->open_modules.supported_gpu_present = TRUE;
                    } else {
                        op->open_modules.unsupported_gpu_present = TRUE;
                    }
                }
            }
        }
    }

    /* 清理 libpciaccess 资源 */
    pci_system_cleanup();
}


/*
 * check_for_nvidia_graphics_devices() - 检查并报告系统中的 NVIDIA GPU 情况
 *
 * 功能说明:
 *   基于 pci_device_scan() 的扫描结果，向用户报告 GPU 相关信息：
 *
 *   1. 旧版 GPU 警告：如果检测到旧版 GPU，列出每个旧版 GPU 的名称
 *      和推荐使用的旧版驱动分支
 *   2. 不支持的 GPU 警告：如果没有检测到当前驱动支持的 GPU，显示警告
 *      并跳过模块加载测试
 *   3. 非 VGA GPU 处理：如果没有检测到 VGA 类型的 GPU，跳过
 *      nvidia-xconfig 配置询问
 *
 * 参数:
 *   op - 全局选项结构体指针
 *   p  - 安装包信息结构体
 */

void check_for_nvidia_graphics_devices(Options *op, Package *p)
{
    if (op->pci_devices.num_legacy > 0) {
        char *list = nvstrdup("\n");
        int i;

        for (i = 0; i < op->pci_devices.num_legacy; i++) {
            const LEGACY_INFO *info = &LegacyList[op->pci_devices.legacy[i]];
            char *old_list = list;
            int j;

            for (j = 0; j < ARRAY_LEN(LegacyStrings); j++) {
                if (LegacyStrings[j].branch == info->branch) {
                    break;
                }
            }

            if (j >= ARRAY_LEN(LegacyStrings)) continue;

            list = nvstrcat(list, info->AdapterString, " requires ",
                            LegacyStrings[j].description, "\n", NULL);
            nvfree(old_list);
        }

        ui_warn(op, "The following NVIDIA GPUs are supported through NVIDIA "
                    "legacy Linux graphics drivers and will be ignored by the "
                    "NVIDIA %s Linux graphics driver:\n%s\nPlease visit "
                    "https://www.nvidia.com/object/unix.html for more "
                    "information.", p->version, list);

        nvfree(list);
    }

    if (!op->pci_devices.found_supported) {
        /* 系统中没有支持的 GPU 时，跳过模块加载测试 */
        op->skip_module_load = TRUE;

        ui_warn(op, "You do not appear to have an NVIDIA GPU supported by the "
                 "%s NVIDIA Linux graphics driver installed in this system.  "
                 "For further details, please see the appendix SUPPORTED "
                 "NVIDIA GRAPHICS CHIPS in the README available on the Linux "
                 "driver download page at www.nvidia.com.", p->version);
    }

    /* 如果没有 VGA 类型的 GPU，不需要询问 nvidia-xconfig 配置 */
    if (!op->pci_devices.found_vga)
        op->no_nvidia_xconfig_question = TRUE;
} /* check_for_nvidia_graphics_devices() */


/*
 * check_selinux() - 检测和配置 SELinux 状态
 *
 * 功能说明:
 *   检测系统上 SELinux 的可用性和启用状态，并设置 op->selinux_enabled。
 *   还会确定用于 chcon 命令的正确安全上下文类型（selinux_chcon_type），
 *   这是安装共享库时标记 SELinux 安全上下文所必需的。
 *
 *   处理逻辑根据 op->selinux_option 的值分三种情况：
 *   - SELINUX_FORCE_YES: 强制启用 SELinux 支持（需要工具可用）
 *   - SELINUX_FORCE_NO: 强制禁用 SELinux 支持（如果 SELinux 处于
 *     Enforcing 模式会发出警告）
 *   - SELINUX_DEFAULT: 自动检测（运行 selinuxenabled 命令判断）
 *
 *   当 SELinux 启用时，还会通过创建临时文件并尝试不同的 chcon 类型
 *   来确定正确的安全上下文类型（textrel_shlib_t / texrel_shlib_t / shlib_t）。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - 检测成功（不论 SELinux 是否启用）
 *   FALSE - 选项冲突（如强制启用但工具不可用）
 */
int check_selinux(Options *op)
{
    /* 检查 SELinux 相关工具是否都已找到 */
    int selinux_available = TRUE;

    if (op->utils[CHCON] == NULL ||
        op->utils[SELINUX_ENABLED] == NULL ||
        op->utils[GETENFORCE] == NULL) {
        selinux_available = FALSE;
    }

    switch (op->selinux_option) {
    case SELINUX_FORCE_YES:
        if (selinux_available == FALSE) {
            /* We have set the option --force-selinux=yes but SELinux 
             * is not available on this system */
            ui_error(op, "Invalid option '--force-selinux=yes'; "
                        "SELinux is not available on this system");
            return FALSE;
        }
        op->selinux_enabled = TRUE;
        break;
        
    case SELINUX_FORCE_NO:
        if (selinux_available == TRUE) {
            char *data = NULL;
            int ret = run_command(op, &data, FALSE, NULL, TRUE,
                                  op->utils[GETENFORCE], NULL);

            if ((ret != 0) || (!data)) {
                ui_warn(op, "Cannot check the current mode of SELinux; "
                             "Command getenforce() failed"); 
            } else if (!strcmp(data, "Enforcing")) {
                /* We have set the option --force-selinux=no but SELinux 
                 * is enforced on this system */
                ui_warn(op, "The option '--force-selinux' has been set to 'no', "
                            "but SELinux is enforced on this system; "
                            "The X server may not start correctly ");
            }
            nvfree(data);
        }        
        op->selinux_enabled = FALSE;
        break;
        
    case SELINUX_DEFAULT:
        op->selinux_enabled = FALSE;
        if (selinux_available == TRUE) {
            int ret = run_command(op, NULL, FALSE, NULL, TRUE,
                                  op->utils[SELINUX_ENABLED], NULL);
            if (ret == 0) {
                op->selinux_enabled = TRUE;
            }
        }
        break;
    }                 

    /* 如果用户未指定 chcon 类型，自动检测合适的安全上下文类型 */
    if (op->selinux_enabled && !op->selinux_chcon_type) {
        unsigned char foo = 0;
        char *tmpfile;
        /* 按优先级排列的 chcon 类型候选列表 */
        static const char* chcon_types[] = {
            "textrel_shlib_t",    /* 含文本重定位的共享库（最佳匹配） */
            "texrel_shlib_t",     /* 上述类型的旧名称（某些系统使用） */
            "shlib_t",            /* 通用共享库类型（兜底选项） */
            NULL
        };

        /* 创建一个临时文件用于测试 chcon 命令 */
        tmpfile = write_temp_file(op, 1, &foo, S_IRUSR);
        if (!tmpfile) {
            ui_warn(op, "Couldn't test chcon.  Assuming shlib_t.");
            op->selinux_chcon_type = "shlib_t";
        } else {
            int i;

            /* 依次尝试每种 chcon 类型，使用第一个成功的 */
            for (i = 0; chcon_types[i]; i++) {
                if (set_security_context(op, tmpfile, chcon_types[i])) {
                    break;
                }
            }

            if (!chcon_types[i]) {
                /* None of them work! */
                ui_warn(op, "Couldn't find a working chcon argument.  "
                            "Defaulting to shlib_t.");
                op->selinux_chcon_type = "shlib_t";
            } else {
                op->selinux_chcon_type = chcon_types[i];
            }

            unlink(tmpfile);
            nvfree(tmpfile);
        }
    }

    if (op->selinux_enabled) {
        ui_log(op, "Tagging shared libraries with chcon -t %s.",
               op->selinux_chcon_type);
    }

    return TRUE;
} /* check_selinux */

/*
 * run_nvidia_xconfig() - 运行 nvidia-xconfig 工具
 *
 * 功能说明:
 *   nvidia-xconfig 是 NVIDIA 提供的 X 配置文件管理工具。不带参数运行时，
 *   它会确保 X 配置文件默认使用 NVIDIA 驱动。也可以通过 restore 参数
 *   来恢复原始的 X 配置文件备份。
 *
 * 参数:
 *   op             - 全局选项结构体指针
 *   restore        - 是否添加 --restore-original-backup 选项来恢复备份
 *   question       - 运行前要向用户询问的问题。如果为 NULL，直接运行不询问
 *   default_answer - question 的默认答案（TRUE/FALSE）
 *
 * 返回值:
 *   TRUE  - nvidia-xconfig 成功执行
 *   FALSE - nvidia-xconfig 未找到、用户拒绝运行、或执行失败
 */

int run_nvidia_xconfig(Options *op, int restore, const char *question,
                       int default_answer)
{
    int ret = FALSE;
    char *nvidia_xconfig;

    /* 搜索 nvidia-xconfig 工具 */
    nvidia_xconfig = find_system_util("nvidia-xconfig");

    if (nvidia_xconfig == NULL) {
        /* 未找到 nvidia-xconfig，跳过不询问 */
        goto done;
    }

    /* 如果有问题要问，询问用户；否则直接运行 */
    ret = question ? ui_yes_no(op, default_answer, "%s", question) : TRUE;

    if (ret) {
        int cmd_ret;
        char *data, *cmd, *args;

        /* 根据 restore 参数决定是否添加恢复备份选项 */
        args = restore ? " --restore-original-backup" : "";

        cmd = nvstrcat(nvidia_xconfig, args, NULL);

        cmd_ret = run_command(op, &data, FALSE, NULL, TRUE, cmd, NULL);

        if (cmd_ret != 0) {
            ui_error(op, "Failed to run `%s`:\n%s", cmd, data);
            ret = FALSE;
        }

        nvfree(cmd);
        nvfree(data);
    }

done:
    nvfree(nvidia_xconfig);

    return ret;

} /* run_nvidia_xconfig() */



/* 发行版钩子脚本所在的目录 */
#define DISTRO_HOOK_DIRECTORY "/usr/lib/nvidia/"

/*
 * run_distro_hook() - 运行发行版提供的钩子脚本
 *
 * 功能说明:
 *   Linux 发行版可以在 /usr/lib/nvidia/ 目录下放置钩子脚本，
 *   在 NVIDIA 驱动安装过程中的特定时机被调用。这些脚本可以用于
 *   发行版特定的集成操作（如更新 initramfs、配置模块加载等）。
 *
 *   跳过条件:
 *   - --kernel-modules-only 模式下不运行发行版脚本
 *   - 钩子脚本文件不存在或不可执行
 *   - 专家模式下用户选择不运行
 *
 * 参数:
 *   op   - 全局选项结构体指针
 *   hook - 钩子脚本的文件名（不含目录部分）
 *
 * 返回值:
 *   HOOK_SCRIPT_SUCCESS - 脚本执行成功
 *   HOOK_SCRIPT_FAIL    - 脚本执行失败
 *   HOOK_SCRIPT_NO_RUN  - 脚本未执行（不存在或被跳过）
 */

HookScriptStatus run_distro_hook(Options *op, const char *hook)
{
    int ret, status, shouldrun = op->run_distro_scripts;
    char *cmd = nvstrcat(DISTRO_HOOK_DIRECTORY, hook, NULL);

    /* 仅安装内核模块时，跳过发行版脚本 */
    if (op->kernel_modules_only) {
        ui_expert(op,
                  "Not running distribution-provided %s script %s because "
                  "--kernel-modules-only was specified.",
                  hook, cmd);
        ret = HOOK_SCRIPT_NO_RUN;
        goto done;
    }

    if (access(cmd, X_OK) < 0) {
        ui_expert(op, "No distribution %s script found.", hook);
        ret = HOOK_SCRIPT_NO_RUN;
        goto done;
    }

    /* in expert mode, ask before running distro hooks */
    if (op->expert) {
        shouldrun = ui_yes_no(op, shouldrun,
                              "Run distribution-provided %s script %s?",
                              hook, cmd);
    }

    if (!shouldrun) {
        ui_expert(op,
                  "Not running distribution-provided %s script %s",
                  hook, cmd);
        ret = HOOK_SCRIPT_NO_RUN;
        goto done;
    }

    ui_status_begin(op, "Running distribution scripts", "Executing %s", cmd);
    status = run_command(op, NULL, TRUE, NULL, TRUE, cmd, NULL);
    ui_status_end(op, "done.");

    ret = (status == 0) ? HOOK_SCRIPT_SUCCESS : HOOK_SCRIPT_FAIL;

done:
    nvfree(cmd);
    return ret;
}


/*
 * prompt_for_user_cancel() - 显示消息并询问用户是否取消安装
 *
 * 功能说明:
 *   向用户显示调用者提供的消息文本，并可选地从指定文件中读取附加的
 *   详细信息一并展示。然后询问用户是否要取消安装。
 *   用于在检测到备用安装方式时提示用户。
 *
 * 参数:
 *   op             - 全局选项结构体指针
 *   file           - 可选的附加信息文件路径（由发行版维护者提供）
 *   default_choice - 默认选择（CONTINUE_CHOICE 或 ABORT_CHOICE）
 *   text           - 主要的提示消息文本
 *
 * 返回值:
 *   TRUE  - 用户选择取消安装
 *   FALSE - 用户选择继续安装
 */
static int prompt_for_user_cancel(Options *op, const char *file,
                                  int default_choice, const char *text)
{
    int ret, file_read, msglen;
    char *message = NULL, *prompt;

    file_read = read_text_file(file, &message);

    if (!file_read || !message) {
        message = nvstrdup("");
    }

    msglen = strlen(message);

    prompt = nvstrcat(text, msglen > 0 ? "\n\nPlease review the message "
                      "provided by the maintainer of this alternate "
                      "installation method and decide how to proceed:" : NULL,
                      NULL);

    ret = ui_paged_prompt(op, prompt, msglen > 0 ? "Information about the "
                          "alternate installation method" : "", message,
                          CONTINUE_ABORT_CHOICES, NUM_CONTINUE_ABORT_CHOICES,
                          default_choice);

    nvfree(message);
    nvfree(prompt);

    if (ret == ABORT_CHOICE) {
        ui_error(op, "The installation was canceled due to the availability "
                 "or presence of an alternate driver installation. Please "
                 "see %s for more details.", op->log_file_name);
        return TRUE;
    }

    return FALSE;
}

/* 标记文件名：指示备用安装方式是否存在或可用 */
#define INSTALL_PRESENT_FILE "alternate-install-present"
#define INSTALL_AVAILABLE_FILE "alternate-install-available"

/*
 * check_for_alternate_install() - 检查是否存在备用的驱动安装方式
 *
 * 功能说明:
 *   检查发行版是否提供了备用的 NVIDIA 驱动安装方式（通常是发行版的包管理器）。
 *   通过检查 /usr/lib/nvidia/ 目录下的标记文件来判断：
 *
 *   1. alternate-install-present: 表示已通过备用方式安装了驱动。
 *      建议用户先卸载已有安装或通过原有方式更新。默认建议中止。
 *
 *   2. alternate-install-available: 表示有备用安装方式可用。
 *      通知用户发行版提供的包可能集成更好。默认建议继续。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - 没有备用安装、检查被跳过、或用户选择继续
 *   FALSE - 用户选择取消安装
 */

int check_for_alternate_install(Options *op)
{
    int shouldcheck = op->check_for_alternate_installs;
    const char *alt_inst_present = DISTRO_HOOK_DIRECTORY INSTALL_PRESENT_FILE;
    const char *alt_inst_avail = DISTRO_HOOK_DIRECTORY INSTALL_AVAILABLE_FILE;

    if (op->expert) {
        shouldcheck = ui_yes_no(op, shouldcheck,
                                "Check for the availability or presence of "
                                "alternate driver installs?");
    }

    if (!shouldcheck) {
        return TRUE;
    }

    if (access(alt_inst_present, F_OK) == 0) {
        const char *msg;

        msg = "The NVIDIA driver appears to have been installed previously "
              "using a different installer. To prevent potential conflicts, it "
              "is recommended either to update the existing installation using "
              "the same mechanism by which it was originally installed, or to "
              "uninstall the existing installation before installing this "
              "driver.";

        return !prompt_for_user_cancel(op, alt_inst_present, ABORT_CHOICE, msg);
    }

    if (access(alt_inst_avail, F_OK) == 0) {
        const char *msg;

        msg = "An alternate method of installing the NVIDIA driver was "
              "detected. (This is usually a package provided by your "
              "distributor.) A driver installed via that method may integrate "
              "better with your system than a driver installed by "
              "nvidia-installer.";

        return !prompt_for_user_cancel(op, alt_inst_avail, CONTINUE_CHOICE, msg);
    }

    return TRUE;
}



/*
 * nouveau_is_present() - 检测 nouveau 开源驱动是否正在使用
 *
 * 功能说明:
 *   通过遍历 /sys/bus/pci/devices/ 下的所有 PCI 设备目录，
 *   读取每个设备的 "driver" 符号链接来确定该设备当前绑定的
 *   内核驱动程序。如果任何设备绑定了 "nouveau" 驱动，则返回 TRUE。
 *
 *   等效于命令: ls -l /sys/bus/pci/devices/*/driver | grep nouveau
 *
 *   nouveau 是 NVIDIA GPU 的开源驱动，与 NVIDIA 官方驱动不兼容，
 *   在安装 NVIDIA 驱动之前必须禁用。
 *
 * 返回值:
 *   TRUE  - 检测到 nouveau 驱动正在使用
 *   FALSE - 未检测到 nouveau 驱动
 */

/* sysfs 中 PCI 设备列表的路径 */
#define SYSFS_DEVICES_PATH "/sys/bus/pci/devices"

int nouveau_is_present(void)
{
    DIR *dir;
    struct dirent * ent;
    int found = FALSE;

    dir = opendir(SYSFS_DEVICES_PATH);

    if (!dir) {
        return FALSE;
    }

    /* 遍历所有 PCI 设备目录 */
    while ((ent = readdir(dir)) != NULL) {

        char driver_path[PATH_MAX];
        char symlink_target[PATH_MAX];
        char *name;
        ssize_t ret;

        /* 跳过 "." 和 ".." 目录项 */
        if ((strcmp(ent->d_name, ".") == 0) ||
            (strcmp(ent->d_name, "..") == 0)) {
            continue;
        }

        /* 构造 driver 符号链接的完整路径 */
        snprintf(driver_path, PATH_MAX,
                 SYSFS_DEVICES_PATH "/%s/driver", ent->d_name);

        driver_path[PATH_MAX - 1] = '\0';

        /* 读取 driver 符号链接的目标路径 */
        ret = readlink(driver_path, symlink_target, PATH_MAX);
        if (ret < 0) {
            /* 该设备没有绑定驱动程序，跳过 */
            continue;
        }

        /* readlink(3) 不会在返回的字符串末尾添加 NUL 终止符 */

        ret = NV_MIN(ret, PATH_MAX - 1);

        symlink_target[ret] = '\0';

        /* 提取符号链接目标路径的最后一部分（即驱动名称） */
        name = basename(symlink_target);

        if (strcmp(name, "nouveau") == 0) {
            found = TRUE;
            break;
        }
    }

    closedir(dir);

    return found;
}



/* modprobe 配置文件可能存放的目录列表 */
static const char* modprobe_directories[] = { "/etc/modprobe.d",
                                              "/usr/lib/modprobe.d" };
/* 用于禁用 nouveau 的配置文件名 */
#define DISABLE_NOUVEAU_FILE "/nvidia-installer-disable-nouveau.conf"

/*
 * 预计算的 CRC 校验值：对应 disable_nouveau() 写入的文件内容。
 * 用于快速判断现有文件是否与我们生成的内容一致。
 */

#define DISABLE_NOUVEAU_FILE_CKSUM 3728279991U

/*
 * disable_nouveau_filename() - 生成禁用 nouveau 的配置文件完整路径
 *
 * 参数:
 *   directory - modprobe 配置目录路径
 *
 * 返回值:
 *   新分配的完整路径字符串（调用者需释放）
 */
static char *disable_nouveau_filename(const char *directory)
{
    return nvstrcat(directory, DISABLE_NOUVEAU_FILE, NULL);
}

/*
 * write_disable_nouveau_file() - 在指定目录中写入禁用 nouveau 的配置文件
 *
 * 功能说明:
 *   在指定的 modprobe 配置目录中创建一个配置文件，内容为：
 *     blacklist nouveau          # 将 nouveau 加入黑名单
 *     options nouveau modeset=0  # 禁用 nouveau 的 modesetting
 *
 * 参数:
 *   directory - modprobe 配置目录路径
 *
 * 返回值:
 *   成功时返回写入的文件完整路径（调用者需释放）
 *   失败时返回 NULL（目录不存在或写入失败）
 */
static char *write_disable_nouveau_file(const char *directory)
{
    int ret;
    struct stat stat_buf;
    FILE *file;
    char *filename;

    /* 验证目标目录是否存在且为目录类型 */
    ret = stat(directory, &stat_buf);

    if (ret != 0 || !S_ISDIR(stat_buf.st_mode)) {
        return NULL;
    }

    filename = disable_nouveau_filename(directory);
    file = fopen(filename, "w+");

    if (!file) {
        nvfree(filename);
        return NULL;
    }

    /* 写入禁用 nouveau 的 modprobe 配置 */
    fprintf(file, "# generated by nvidia-installer\n");
    fprintf(file, "blacklist nouveau\n");
    fprintf(file, "options nouveau modeset=0\n");

    ret = fclose(file);

    if (ret != 0) {
        nvfree(filename);
        return NULL;
    }

    return filename;
}


/*
 * disable_nouveau() - 在所有 modprobe 配置目录中写入禁用 nouveau 的文件
 *
 * 功能说明:
 *   遍历所有已知的 modprobe 配置目录（/etc/modprobe.d 和
 *   /usr/lib/modprobe.d），在每个目录中写入黑名单配置文件。
 *
 * 返回值:
 *   成功时返回所有写入的文件路径列表（逗号分隔），调用者需释放
 *   全部失败时返回 NULL
 */

static char *disable_nouveau(void)
{
    int i;
    char *filelist = NULL;

    for (i = 0; i < ARRAY_LEN(modprobe_directories); i++) {
        char *filename = write_disable_nouveau_file(modprobe_directories[i]);
        if (filename) {
            filelist = nv_prepend_to_string_list(filelist, filename, ", ");
            nvfree(filename);
        }
    }

    return filelist;
}



/*
 * disable_nouveau_file_is_present() - 检查禁用 nouveau 的配置文件是否已存在
 *
 * 功能说明:
 *   检查各个 modprobe 配置目录中是否已存在由 nvidia-installer 生成的
 *   禁用 nouveau 配置文件。通过 CRC 校验确认文件内容与预期一致。
 *
 * 参数:
 *   op                  - 全局选项结构体指针
 *   present_at_all_paths - 输出参数：TRUE 表示在所有可用的配置目录中
 *                          都已存在该文件
 *
 * 返回值:
 *   找到的文件路径列表（逗号分隔），调用者需释放
 *   未找到匹配文件时返回 NULL
 */

static char *disable_nouveau_file_is_present(Options *op,
                                             int *present_at_all_paths)
{
    int i, directory_count = 0, file_count = 0;
    char *filelist = NULL;

    for (i = 0; i < ARRAY_LEN(modprobe_directories); i++) {
        char *filename = disable_nouveau_filename(modprobe_directories[i]);

        if (directory_exists(modprobe_directories[i])) {
            directory_count++;
        }

        if ((access(filename, R_OK) == 0) &&
            (compute_crc(op, filename) == DISABLE_NOUVEAU_FILE_CKSUM)) {
            file_count++;
            filelist = nv_prepend_to_string_list(filelist, filename, ", ");
        }
        nvfree(filename);
    }

    *present_at_all_paths = directory_count == file_count;
    return filelist;
}



/*
 * check_for_nouveau() - 检测并处理 nouveau 驱动的存在
 *
 * 功能说明:
 *   检测 nouveau 开源驱动是否正在使用。如果检测到 nouveau：
 *   1. 检查是否已存在禁用 nouveau 的配置文件
 *   2. 如果没有，提供自动创建配置文件的选项
 *   3. 询问用户是否在 nouveau 存在的情况下继续安装
 *
 *   即使 nouveau 存在，用户也可以选择继续安装，但某些健全性检查将被跳过。
 *   如果用户选择中止，会先提供更新 initramfs 的选项。
 *
 *   使用 --no-nouveau-check 选项可以跳过此检查。
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - nouveau 未检测到、或用户选择继续
 *   FALSE - 用户选择因 nouveau 存在而中止安装
 */

int check_for_nouveau(Options *op)
{
    int ret, nouveau_detected, all_files_written;
    char *disable_files;

#define NOUVEAU_POINTER_MESSAGE                                         \
    "Please consult the NVIDIA driver README and your Linux "           \
        "distribution's documentation for details on how to correctly " \
        "disable the Nouveau kernel driver."

    if (op->no_nouveau_check) return TRUE;

    nouveau_detected = nouveau_is_present();

    if (nouveau_detected) {
        ui_warn(op, "The Nouveau kernel driver is currently in use "
                "by your system.  This driver is incompatible with the NVIDIA "
                "driver, and must be disabled before proceeding.");
    } else {
        return TRUE;
    }

    disable_files = disable_nouveau_file_is_present(op, &all_files_written);

    if (disable_files) {
        ui_warn(op, "One or more modprobe configuration files to disable "
                "Nouveau are already present at: %s.  Please be "
                "sure you have rebooted your system since these files were "
                "written.  If you have rebooted, then Nouveau may be enabled "
                "for other reasons, such as being included in the system "
                "initial ramdisk or in your X configuration file.  "
                NOUVEAU_POINTER_MESSAGE, disable_files);
        nvfree(disable_files);
        if (all_files_written) {
            /* If all of the possible disable files are already present,
             * don't offer to write any more. */
            goto continue_or_abort;
        }
    }

    /* Disable files were missing from at least one of the expected locations:
     * offer to create additional ones. */
    ret = ui_yes_no(op, op->disable_nouveau,
                    "Nouveau can usually be disabled by adding files "
                    "to the modprobe configuration directories and rebuilding "
                    "the initramfs.\n\n"
                    "Would you like nvidia-installer to attempt to create "
                    "these modprobe configuration files for you?");

    if (ret) {
        disable_files = disable_nouveau();

        if (disable_files) {
            ui_message(op, "One or more modprobe configuration files to "
                       "disable Nouveau have been written.  You will need "
                       "to reboot your system and possibly rebuild the initramfs "
                       "before these changes can take effect.  Note if you "
                       "later wish to reenable Nouveau, you will need to "
                       "delete these files: %s",
                       disable_files);
            nvfree(disable_files);
        } else {
            ui_warn(op, "Unable to alter the nouveau modprobe configuration.  "
                    NOUVEAU_POINTER_MESSAGE);
        }
    } else {
        ui_message(op, "Please disable Nouveau manually and attempt to install "
                   "the NVIDIA driver again later.");
        return FALSE;
    }

continue_or_abort:

    ret = ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
        NUM_CONTINUE_ABORT_CHOICES,
        op->allow_installation_with_running_driver ?
        CONTINUE_CHOICE : ABORT_CHOICE,
        "nvidia-installer is not able to perform some of the sanity checks "
        "which detect potential installation problems while Nouveau is loaded. "
        "Would you like to continue installation without these sanity "
        "checks, or abort installation, confirm that Nouveau has been "
        "properly disabled, and attempt installation again later?");

    if (ret == ABORT_CHOICE) {
        /* Offer to update the initramfs: normally, this happens before
         * installing files, but the user is explicitly bailing out. */
        update_initramfs(op);
        return FALSE;
    }

    op->skip_module_load = TRUE;
    ui_log(op, "Proceeding with installation despite the presence of Nouveau. "
               "Kernel module load tests will be skipped.");

    return TRUE;
}

/* DKMS 操作类型常量 */
#define DKMS_STATUS  " status"   /* 查询模块状态 */
#define DKMS_INSTALL " install"  /* 安装模块 */
#define DKMS_REMOVE  " remove"   /* 移除模块 */

/*
 * run_dkms() - 运行 DKMS 工具执行指定操作
 *
 * 功能说明:
 *   封装 DKMS 命令行工具的调用，支持以下操作：
 *   - DKMS_STATUS:  查询指定模块的状态
 *   - DKMS_INSTALL: 为指定内核安装模块（需要 version 参数）
 *   - DKMS_REMOVE:  从所有内核中移除模块（需要 version 参数，自动添加 --all）
 *
 *   自动处理 --no-depmod 选项（如果 DKMS 支持），因为 nvidia-installer
 *   已经单独运行 depmod，无需 DKMS 重复执行。
 *
 * 参数:
 *   op      - 全局选项结构体指针
 *   verb    - DKMS 操作（DKMS_STATUS/DKMS_INSTALL/DKMS_REMOVE）
 *   version - 驱动版本号（可选，为 NULL 时不指定版本）
 *   kernel  - 目标内核版本（可选，为 NULL 时不指定内核，
 *             DKMS_REMOVE 操作忽略此参数并使用 --all）
 *   out     - 输出参数，存储命令输出（可选，为 NULL 时丢弃输出）
 *
 * 返回值:
 *   TRUE  - DKMS 命令执行成功（退出码为 0）
 *   FALSE - DKMS 未找到或命令执行失败
 */
static int run_dkms(Options *op, const char* verb, const char *version,
                    const char *kernel, char** out)
{
    char *cmdline, *veropt, *kernopt = NULL;
    const char *modopt = " -m nvidia"; /* XXX real name is in the Package */
    const char *kernopt_all = "", *depmod_opt = "";
    char *output;
    int ret;

    /* 如果 DKMS 工具未找到，直接失败 */
    if (!op->utils[DKMS]) {
        if (strcmp(verb, DKMS_STATUS) != 0) {
            ui_error(op, "Failed to find dkms on the system!");
        }
        return FALSE;
    }

    /*
     * 如果 DKMS 支持 --no-depmod 选项，在安装/移除操作时添加该选项。
     * 因为 nvidia-installer 已经在内核模块安装流程中单独运行了 depmod(8)。
     */
    if ((strcmp(verb, DKMS_REMOVE) == 0 || strcmp(verb, DKMS_INSTALL) == 0) &&
        option_is_supported(op, op->utils[DKMS], "--help", "--no-depmod")) {
        depmod_opt = " --no-depmod";
    }

    /* 将函数参数转换为命令行参数。可选参数可能为 NULL，
     * nvstrcat() 遇到 NULL 时会提前结束拼接。 */
    veropt = version ? nvstrcat(" -v ", version, NULL) : NULL;

    if (strcmp(verb, DKMS_REMOVE) == 0) {
        /* 移除操作始终从所有内核中移除，避免残留造成混淆 */
        kernopt_all = " --all";
    } else {
        kernopt = kernel ? nvstrcat(" -k ", kernel, NULL) : NULL;
    }

    cmdline = nvstrcat(op->utils[DKMS], verb, depmod_opt, modopt, veropt,
                       kernopt_all, kernopt, NULL);

    /* 执行 DKMS 命令 */
    ret = run_command(op, &output, FALSE, NULL, TRUE, cmdline, NULL);
    if (ret != 0) {
        ui_error(op, "Failed to run `%s`: %s", cmdline, output);
    }

    nvfree(cmdline);
    nvfree(veropt);
    nvfree(kernopt);
    if (out) {
        *out = output;
    } else {
        nvfree(output);
    }

    return ret == 0;
}

/*
 * dkms_module_installed() - 检查模块是否已通过 DKMS 安装
 *
 * 功能说明:
 *   通过 `dkms status` 命令查询 NVIDIA 模块是否已在 DKMS 中注册和安装。
 *   支持按驱动版本和内核版本进行精确查询。
 *
 * 参数:
 *   op     - 全局选项结构体指针
 *   driver - 驱动版本号（可选，为 NULL 时匹配任意版本）
 *   kernel - 内核版本号（可选，为 NULL 时匹配任意内核）
 *
 * 返回值:
 *   TRUE  - DKMS 可用且检测到匹配的已安装模块
 *   FALSE - DKMS 不可用或未检测到匹配的模块
 */
int dkms_module_installed(Options* op, const char *driver, const char *kernel)
{
    int ret, matched = FALSE;
    char *output = NULL;

    ret = run_dkms(op, DKMS_STATUS, driver, kernel, &output);

    if (!ret || output == NULL) {
        return FALSE;
    }

    if (driver != NULL && kernel != NULL) {
        /*
         * 如果同时指定了模块版本和内核版本，只匹配状态为 "installed" 的模块
         */
        if (strstr(output, ": installed") != NULL) {
            matched = TRUE;
        }
    } else {
        /*
         * 否则，匹配任何非空输出来检测任何状态（added、built、installed）的模块
         */
        matched = output[0] != '\0';
    }

    nvfree(output);

    return matched;
}


/*
 * dkms_gen_tarball() - 生成符合 DKMS 导入格式的 tar 归档
 *
 * 功能说明:
 *   创建一个临时目录结构，包含已编译的内核模块、模块源码、构建日志
 *   和 DKMS 元数据，然后将其打包为 tar 归档文件。该归档文件可通过
 *   `dkms ldtarball` 命令导入到 DKMS 数据库中。
 *
 *   归档的目录结构遵循 `dkms mktarball` 的导出格式：
 *   - dkms_source_tree/: 模块源码和 dkms.conf
 *   - dkms_main_tree/<kernel>/<arch>/:
 *     - module/: 已编译的内核模块 (.ko 文件)
 *     - log/make.log: 构建日志
 *   - dkms_main_tree/dkms_dbversion: DKMS 版本兼容性文件
 *
 * 参数:
 *   op     - 全局选项结构体指针
 *   p      - 安装包信息结构体
 *   kernel - 目标内核版本
 *
 * 返回值:
 *   成功时返回 tar 归档文件的路径（临时文件，调用者需删除和释放）
 *   失败时返回 NULL
 */
static char *dkms_gen_tarball(Options *op, Package *p, const char *kernel)
{
    char *tmpdir, *sourcedir, *treedir, *builddir, *logdir, *moduledir, *dst;
    char *tarball = NULL;
    const char *log;
    int ret, i;

    tmpdir = make_tmpdir(op);
    if (!tmpdir) return NULL;

    sourcedir = nvdircat(tmpdir, "dkms_source_tree", NULL);
    treedir = nvdircat(tmpdir, "dkms_main_tree", NULL);
    builddir = nvdircat(treedir, kernel, get_machine_arch(op), NULL);
    logdir = nvdircat(builddir, "log", NULL);
    moduledir = nvdircat(builddir, "module", NULL);

    /*
     * DKMS 2.x checks for dkms_dbversion with a major version of 2.
     * DKMS 3.x ignores dkms_dbversion. Write a dkms_dbversion file
     * for compatibility with DKMS 2.x.
     */
    dst = nvdircat(treedir, "dkms_dbversion", NULL);
    ret = nv_string_to_file(dst, "2.0.0");
    nvfree(dst);
    if (!ret) goto done;

    /* Write the build log to the tarball staging directory */
    dst = nvdircat(logdir, "make.log", NULL);

    if (p->kernel_make_logs) {
        log = p->kernel_make_logs;
    } else {
        log = "This driver was linked from precompiled interfaces and directly "
              "registered with DKMS. This process did not preserve build logs.";
    }

    ret = nv_string_to_file(dst, log);
    nvfree(dst);
    if (!ret) goto done;

    /* Copy the module sources and dkms.conf to the staging directory */
    ret = FALSE;
    for (i = 0; i < p->num_entries; i++) {
        char *dst_copy, *dstdir;
        char *dkms_dstdir, *dkms_srcdir;

        switch (p->entries[i].type) {
        case FILE_TYPE_DKMS_CONF:
        case FILE_TYPE_KERNEL_MODULE_SRC:
            dst = nvdircat(sourcedir, p->entries[i].path, p->entries[i].name,
                           NULL);
            dst_copy = nvstrdup(dst);
            dstdir = dirname(dst_copy);

            if (!directory_exists(dstdir)) {
                ret = mkdir_recursive(op, dstdir, 0755, FALSE);
            }

            dkms_srcdir = nvstrcat("/usr/src/nvidia-", p->version, NULL);
            dkms_dstdir = nvdircat(dkms_srcdir, p->entries[i].path, NULL);
            nvfree(dkms_srcdir);

            /*
             * Create any missing directories which will contain the kernel
             * module sources once the modules are installed via DKMS. This
             * is done ahead of time, with mkdir logging enabled, so these
             * directories can be removed upon uninstallation.
             */
            if (!directory_exists(dkms_dstdir)) {
                mkdir_recursive(op, dkms_dstdir, 0755, TRUE);
            }

            nvfree(dkms_dstdir);

            ret = ret && copy_file(op, p->entries[i].file, dst, 0644);
            nvfree(dst);
            nvfree(dst_copy);
            if (!ret) goto done;
            break;
        default:
            break;
        }
    }

    /* If ret wasn't set to TRUE above, there are no source files */
    if (!ret) goto done;

    ret = mkdir_recursive(op, moduledir, 0755, FALSE);
    if (!ret) goto done;

    /* Copy the (already built) kernel modules */
    for (i = 0; i < p->num_kernel_modules; i++) {
        char *src = nvdircat(p->kernel_module_build_directory,
                             p->kernel_modules[i].module_filename, NULL);

        dst = nvdircat(moduledir, p->kernel_modules[i].module_filename, NULL);
        ret = copy_file(op, src, dst, 0644);
        nvfree(src);
        nvfree(dst);

        if (!ret) goto done;
    }

    /* Reserve a name for a temporary file and run tar(1) to create a tarball */
    tarball = write_temp_file(op, 0, NULL, 0644);
    if (tarball) {
        char *output;

        ret = run_command(op, &output, FALSE, 0, TRUE,
                          op->utils[TAR], " -C ", tmpdir, " -cf ", tarball,
                          " .", NULL);

        if (ret != 0) {
            ui_error(op, "Failed to create a DKMS tarball: %s", output);
        }
        nvfree(output);

        if (ret != 0) {
            unlink(tarball);
            nvfree(tarball);
            tarball = NULL;
            goto done;
        }
    }

done:
    remove_directory(op, tmpdir);
    nvfree(tmpdir);
    nvfree(sourcedir);
    nvfree(treedir);
    nvfree(builddir);
    nvfree(logdir);
    nvfree(moduledir);

    return tarball;
}


/*
 * dkms_register_module() - 将已安装的内核模块注册到 DKMS
 *
 * 功能说明:
 *   将已构建和安装的 NVIDIA 内核模块注册到 DKMS 数据库中。
 *   注册后，当用户更新内核时，DKMS 可以自动重新构建 NVIDIA 内核模块。
 *
 *   处理流程:
 *   1. 检查 DKMS 和 tar 工具是否可用
 *   2. 询问用户是否启用 DKMS
 *   3. 生成 DKMS 格式的 tar 归档
 *   4. 使用 `dkms ldtarball` 导入归档
 *   5. 使用 `dkms install` 将模块标记为已安装
 *   6. 通过 `dkms status` 验证注册是否成功
 *
 * 参数:
 *   op     - 全局选项结构体指针
 *   p      - 安装包信息结构体
 *   kernel - 目标内核版本
 */
void dkms_register_module(Options *op, Package *p, const char *kernel)
{
    char *tarball;
    int ret = FALSE;

    /* 如果 dkms(8) 或 tar(1) 未安装，无法进行 DKMS 注册 */

    if (!op->utils[DKMS] || !op->utils[TAR]) return;

    /*
     * 仅在内核模块和源码都已安装的情况下，才提供 DKMS 选项
     */
    if (op->no_kernel_modules || op->no_kernel_module_source) return;

    op->dkms = ui_yes_no(op, op->dkms,
                         "Would you like to register the kernel module sources "
                         "with DKMS? This will allow DKMS to automatically "
                         "build a new module, if your kernel changes later.");

    /* 用户决定不使用 DKMS，直接返回 */
    if (!op->dkms) return;

    ui_status_begin(op, "Registering the kernel modules with DKMS:",
                    "Generating DKMS tarball");

    /* 创建 DKMS 格式的 tar 归档 */
    tarball = dkms_gen_tarball(op, p, kernel);
    if (tarball) {
        char *output;

        /* 将 tar 归档导入 DKMS 数据库 */
        ui_status_update(op, .5, "Importing DKMS tarball");
        ret = run_command(op, &output, FALSE, 0, TRUE,
                          op->utils[DKMS], " ldtarball ", tarball, NULL);

        if (ret != 0) {
            ui_error(op, "Failed to load DKMS tarball: %s", output);
        }

        unlink(tarball);
        nvfree(tarball);
        nvfree(output);

        if (ret != 0) {
            goto done;
        }
    } else {
        ui_error(op, "Failed to create a DKMS tarball");
        goto done;
    }

    /*
     * 导入 tarball 后，模块在 DKMS 中处于 "built" 状态。
     * 运行 `dkms install` 将其标记为 "installed"，
     * DKMS 还会执行其他操作（如更新 weak-modules）。
     */
    ui_status_update(op, .75, "Marking modules as installed");
    ret = run_dkms(op, DKMS_INSTALL, p->version, kernel, NULL);
    if (!ret) goto done;

    /*
     * 最终验证：确认 `dkms status` 显示模块已成功 "installed"
     */
    ret = dkms_module_installed(op, p->version, kernel);

done:
    if (ret) {
        ui_status_end(op, "done.");
    } else {
        ui_status_end(op, "Error.");
        ui_warn(op, "Failed to register the NVIDIA kernel modules with DKMS. "
                    "The NVIDIA kernel modules will be installed, but will not "
                    "be automatically rebuilt if you change your kernel.");
    }

    op->dkms_registered = ret;
}

/*
 * dkms_remove_module() - 从 DKMS 中移除指定版本的模块
 *
 * 功能说明:
 *   从所有已注册的内核中移除指定版本的 NVIDIA DKMS 模块。
 *   使用 --all 选项确保从所有内核版本中彻底移除。
 *
 * 参数:
 *   op      - 全局选项结构体指针
 *   version - 要移除的驱动版本号
 *
 * 返回值:
 *   TRUE  - 移除成功
 *   FALSE - 移除失败
 */
int dkms_remove_module(Options *op, const char *version)
{
    return run_dkms(op, DKMS_REMOVE, version, NULL, NULL);
}

/*
 * test_last_bit() - 读取文件的最后一个字节并测试其最低位
 *
 * 功能说明:
 *   读取文件的全部内容直到 EOF，然后检查最后一个字节的最低位（bit 0）。
 *   用于检测 UEFI Secure Boot 变量的状态（启用/禁用）。
 *
 *   注意：不使用 fseek 是因为对 sysfs 中的 UEFI 变量文件使用
 *   fseek(stream, -1, SEEK_END) 后读取会遇到过早 EOF 的问题。
 *
 * 参数:
 *   file - 要读取的文件路径
 *
 * 返回值:
 *   1    - 最低位为 1（Secure Boot 已启用）
 *   0    - 最低位为 0（Secure Boot 已禁用）
 *   < 0  - 错误（返回负的 errno 值）
 */
static int test_last_bit(const char *file) {
    char buf;
    int ret, data_read = FALSE;
    FILE *fp = fopen(file, "r");

    if (!fp) {
        return -errno;
    }

    /* XXX Using fseek(3) could make this more efficient for larger files, but
     * trying to read after an fseek(stream, -1, SEEK_END) call on a UEFI
     * variable file in sysfs hits a premature EOF. */

    while (fread(&buf, 1, 1, fp)) {
        data_read = TRUE;
    }

    if (ferror(fp)) {
        ret = -ferror(fp);
    } else if (data_read) {
        ret = buf & 1;
    } else {
        ret = -EIO;
    }

    fclose(fp);
    return ret;
}

/*
 * Secure Boot 状态信息在 sysfs 中的已知路径。
 * 两种路径对应不同的 sysfs 接口（旧式 vars 接口和新式 efivars 接口）。
 * GUID 8be4df61-93ca-11d2-aa0d-00e098032b8c 是 EFI 全局变量命名空间。
 */
static const char* secure_boot_files[] = {
    "/sys/firmware/efi/vars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c/data",
    "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",
};

/*
 * secure_boot_enabled() - 检测 UEFI Secure Boot 是否启用
 *
 * 功能说明:
 *   依次检查已知的 sysfs 路径来读取 Secure Boot 状态。
 *   Secure Boot 的值存储在 UEFI 变量的最后一个字节的最低位中。
 *
 *   当 Secure Boot 启用时，安装未签名的内核模块可能会失败，
 *   安装程序需要据此向用户发出适当的警告。
 *
 * 返回值:
 *   1    - Secure Boot 已启用
 *   0    - Secure Boot 已禁用
 *   < 0  - 无法检测（如非 UEFI 系统或文件不可读）
 */
int secure_boot_enabled(void) {
    int i, ret = -ENOENT;

    for (i = 0; i < ARRAY_LEN(secure_boot_files); i++) {
        if (access(secure_boot_files[i], R_OK) == 0) {
            ret = test_last_bit(secure_boot_files[i]);
            if (ret >= 0) {
                break;
            }
        }
    }

    return ret;
}



/*
 * get_elf_architecture() - 读取 ELF 文件的架构类型
 *
 * 功能说明:
 *   尝试读取指定文件的 ELF 头部，解析其架构类型（32位/64位）。
 *   首先验证 ELF 魔数（\177ELF），然后读取 e_ident[EI_CLASS] 字段。
 *
 *   此函数用于：
 *   - 在 CRC 校验失败时判断是否可以尝试 unprelink
 *   - 确定安装的库文件的架构类型
 *
 * 参数:
 *   filename - 要检查的文件路径
 *
 * 返回值:
 *   ELF_ARCHITECTURE_32      - 32 位 ELF 文件
 *   ELF_ARCHITECTURE_64      - 64 位 ELF 文件
 *   ELF_ARCHITECTURE_UNKNOWN - 有效的 ELF 文件但架构未知（ELFCLASSNONE）
 *   ELF_INVALID_FILE         - 不是有效的 ELF 文件或读取出错
 */

ElfFileType get_elf_architecture(const char *filename)
{
    FILE *fp;
    ElfW(Ehdr) header;  /* 使用 ElfW 宏自动选择 32/64 位 ELF 头结构体 */

    fp = fopen(filename, "r");

    /* 读取 ELF 文件头 */

    if (fp) {
        int ret = fread(&header, sizeof(header), 1, fp);
        fclose(fp);

        if (ret != 1) {
            return ELF_INVALID_FILE;
        }
    } else {
        return ELF_INVALID_FILE;
    }

    /* 验证 ELF 魔数：0x7F 'E' 'L' 'F' */

    if (strncmp((char *) header.e_ident, "\177ELF", 4) != 0) {
        return ELF_INVALID_FILE;
    }

    /* 从 ELF 头的 EI_CLASS 字段解析架构类型 */

    switch(header.e_ident[EI_CLASS]) {
        case ELFCLASS32:   return ELF_ARCHITECTURE_32;
        case ELFCLASS64:   return ELF_ARCHITECTURE_64;
        case ELFCLASSNONE: return ELF_ARCHITECTURE_UNKNOWN;
        default:           return ELF_INVALID_FILE;
    }
}



/*
 * set_concurrency_level() - 设置编译并发级别
 *
 * 功能说明:
 *   确定内核模块编译时的并发级别（类似 make -j N）。
 *   如果用户未在命令行指定，则自动检测在线 CPU 数量。
 *   为避免系统资源耗尽，设置最大默认值为 32。
 *   在专家模式下，用户可以交互式修改并发级别。
 *
 * 参数:
 *   op - 全局选项结构体指针。结果存储在 op->concurrency_level 中
 */

void set_concurrency_level(Options *op)
{
    int detected_cpus;

    if (op->concurrency_level) {
        /* 用户已通过命令行指定并发级别 */
        ui_log(op, "Concurrency level set to %d on the command line.",
               op->concurrency_level);
    } else {
        /* CPU 数量非常多的系统可能会超出最大任务限制 */
        static const int max_default_cpus = 32;
        int default_concurrency;
#if defined _SC_NPROCESSORS_ONLN
        detected_cpus = sysconf(_SC_NPROCESSORS_ONLN);

        if (detected_cpus < max_default_cpus) {
            default_concurrency = detected_cpus;
        } else {
            default_concurrency = max_default_cpus;
        }

        if (detected_cpus >= 1) {
            ui_log(op, "Detected %d CPUs online; setting concurrency level "
                   "to %d.", detected_cpus, default_concurrency);
        } else
#else
#warning _SC_NPROCESSORS_ONLN not defined; nvidia-installer will not be able \
to detect the number of processors.
#endif
        {
            ui_log(op, "Unable to detect the number of processors: setting "
                   "concurrency level to 1.");
            default_concurrency = 1;
        }
        op->concurrency_level = default_concurrency;
    }

    if (op->expert) {
        int val = op->concurrency_level;
        do {
           char *strval = nvasprintf("%d", val);
           val = atoi(ui_get_input(op, strval, "Concurrency level"));
           nvfree(strval);
        } while (val < 1);
        op->concurrency_level = val;
    }
}

/*
 * get_pkg_config_variable() - 通过 pkg-config 查询包变量的值
 *
 * 功能说明:
 *   执行 `pkg-config --variable=<VARIABLE> <PKG>` 命令，获取指定包中
 *   指定变量的值。用于查询 systemd 的各种目录路径（如 systemd 单元文件
 *   目录、sleep 脚本目录等）。
 *
 *   注意：pkg-config 在包存在但变量不存在时，会返回成功状态码但输出空行。
 *   此函数会将空行视为"未找到"并返回 NULL。
 *
 * 参数:
 *   op       - 全局选项结构体指针
 *   pkg      - 包名（如 "systemd"）
 *   variable - 变量名（如 "systemdsystemunitdir"）
 *
 * 返回值:
 *   成功时返回变量值的字符串（调用者需释放）
 *   变量不存在、包不存在或 pkg-config 不可用时返回 NULL
 */
char *
get_pkg_config_variable(Options *op,
                        const char *pkg, const char *variable)
{
    char *prefix = NULL;
    int ret;

    if (!op->utils[PKG_CONFIG]) {
        return NULL;
    }

    ret = run_command(op, &prefix, FALSE, NULL, TRUE,
                      op->utils[PKG_CONFIG],
                      " --variable=", variable, " ", pkg,
                      NULL);

    if (ret != 0 ||
        /*
         * pkg-config 在包存在但变量不存在时不会返回错误，
         * 而是返回成功并输出空行到 stdout。
         * 当路径为空时，返回 NULL 以回退到默认值。
         */
        (prefix && prefix[0] == '\0')) {
        nvfree(prefix);
        prefix = NULL;
    }

    return prefix;
}

/*
 * check_systemd() - 检查 systemd 是否可用并配置相关路径
 *
 * 功能说明:
 *   检测系统上 systemd 的可用性，并通过 pkg-config 查询 systemd
 *   相关目录路径。设置以下 Options 字段：
 *   - op->use_systemd: 是否使用 systemd（基于 systemctl 是否可用）
 *   - op->systemd_unit_prefix: systemd 单元文件目录
 *   - op->systemd_sleep_prefix: systemd sleep 脚本目录
 *   - op->systemd_sysconf_prefix: systemd 系统配置目录
 *
 * 参数:
 *   op - 全局选项结构体指针
 *
 * 返回值:
 *   TRUE  - 检测成功（不论 systemd 是否可用）
 *   FALSE - 用户指定了 --systemd 但 systemctl 不可用
 */
int check_systemd(Options *op)
{
    /*
     * 如果用户指定了 --no-systemd，跳过所有 systemd 相关检查
     */
    if (op->use_systemd == NV_OPTIONAL_BOOL_FALSE) {
        return TRUE;
    }

    if (op->utils[SYSTEMCTL] == NULL) {
        if (op->use_systemd == NV_OPTIONAL_BOOL_TRUE) {
            ui_error(op, "Option '--systemd' was specified but systemctl was "
                     "not found on this system");
            return FALSE;
        }

        op->use_systemd = NV_OPTIONAL_BOOL_FALSE;
        return TRUE;
    }

    op->use_systemd = NV_OPTIONAL_BOOL_TRUE;

    /*
     * 如果 pkg-config 和 systemd.pc 可用，通过它们查询
     * 单元文件和 systemd-sleep 脚本的安装路径
     */
    if (op->systemd_unit_prefix == NULL) {
        op->systemd_unit_prefix =
            get_pkg_config_variable(op, "systemd", "systemdsystemunitdir");
    }

    if (op->systemd_sleep_prefix == NULL) {
        op->systemd_sleep_prefix =
            get_pkg_config_variable(op, "systemd", "systemdsleepdir");
    }

    if (op->systemd_sysconf_prefix == NULL) {
        op->systemd_sysconf_prefix =
            get_pkg_config_variable(op, "systemd", "systemdsystemconfdir");
    }

    return TRUE;
}


/*
 * option_is_supported() - 检查命令是否支持指定的选项
 *
 * 功能说明:
 *   运行 `$cmd $help` 获取命令的帮助文本，然后在帮助文本中搜索
 *   指定的选项字符串。搜索时确保选项两侧由空白字符、方括号或
 *   其他分隔符包围，以避免子串误匹配。
 *
 *   例如：用于检测 DKMS 是否支持 --no-depmod 选项。
 *
 * 参数:
 *   op     - 全局选项结构体指针
 *   cmd    - 要检查的命令路径
 *   help   - 获取帮助的参数（如 "--help"）
 *   option - 要搜索的选项字符串（如 "--no-depmod"）
 *
 * 返回值:
 *   TRUE  - 帮助文本中找到了该选项
 *   FALSE - 未找到
 */
int option_is_supported(Options *op, const char *cmd, const char *help,
                        const char *option)
{
    int option_found = FALSE, option_len = strlen(option);
    char *helptext, *match;

    /* 忽略返回值：某些程序没有专门的 "help" 选项，
     * 在收到无效选项时会打印帮助信息并返回失败状态。 */
    run_command(op, &helptext, FALSE, 0, TRUE,
                cmd, " ", help, NULL);

    for (match = helptext; match && *match; match = strstr(match + 1, option)) {
        int before_clear;

        /*
         * 查找 "option"：确保它两侧被空白字符、方括号等分隔，
         * 或位于帮助文本的开头/结尾，以避免子串误匹配。
         */
        if (match == helptext) {
            if (strncmp(match, option, option_len)) continue;

            before_clear = TRUE;
        } else {
            const char *before = match - 1;

            if (isspace(*before)) {
                before_clear = TRUE;
            } else {
                switch (*before) {
                case '[': case '<': case '|': case '-':
                    before_clear = TRUE; break;
                default:;
                }
            }
        }

        if (before_clear) {
            const char *after = match + option_len;
            int after_clear;

            if (isspace(*after)) {
                after_clear = TRUE;
            } else {
                switch (*after) {
                case '\0': case ']': case '>': case '|':
                    after_clear = TRUE; break;
                default:;
                }
            }

            if (after_clear) {
                option_found = TRUE;
                break;
            }
        }
    }

    nvfree(helptext);
    return option_found;
}


/*
 * detect_library() - 通过 dlopen 检测动态库是否可用
 *
 * 参数:
 *   library - 库的 soname（如 "libvulkan.so.1"）
 *
 * 返回值:
 *   TRUE  - 库存在且可加载
 *   FALSE - 库不存在或加载失败
 */
static int detect_library(const char *library)
{
    void *handle = dlopen(library, RTLD_NOW);

    if (handle) {
        dlclose(handle);
        return TRUE;
    }

    return FALSE;
}


/*
 * check_for_vulkan_loader() - 检测系统是否安装了 Vulkan 加载器
 *
 * 功能说明:
 *   如果 NVIDIA 驱动包包含 Vulkan ICD（可安装客户端驱动），
 *   检测系统上是否安装了 Vulkan 加载器（libvulkan.so.1）。
 *   没有 Vulkan 加载器，NVIDIA Vulkan ICD 将无法工作。
 *
 * 参数:
 *   op - 全局选项结构体指针
 */
void check_for_vulkan_loader(Options *op)
{
    if (!op->vulkan_icd_json_packaged) {
        /*
         * 如果安装包不包含 Vulkan ICD，不需要检查加载器
         */
        return;
    }

    if (!detect_library("libvulkan.so.1")) {
        ui_warn(op, "This NVIDIA driver package includes Vulkan components, "
                "but no Vulkan ICD loader was detected on this system. "
                "The NVIDIA Vulkan ICD will not function without the loader. "
                "Most distributions package the Vulkan loader; try installing "
                "the \"vulkan-loader\", \"vulkan-icd-loader\", or "
                "\"libvulkan1\" package.");
    }
}


/*
 * add_bullet_list_item() - 向项目符号列表追加一个条目
 *
 * 功能说明:
 *   将新的文本项追加到已有的字符串列表中，格式为 "  * <text>\n"。
 *   会释放原有的字符串并将 *orig 指向新分配的拼接结果。
 *
 * 参数:
 *   new  - 要添加的新条目文本
 *   orig - 现有列表字符串的指针（传引用），会被更新为新字符串
 */
void add_bullet_list_item(const char *new, char **orig)
{
    char *tmp = *orig;

    *orig = nvstrcat(*orig, "  * ", new, "\n", NULL);
    nvfree(tmp);
}


/*
 * suggest_reboot() - 根据安装期间的条件建议用户重启
 *
 * 功能说明:
 *   在安装完成后，检查是否存在需要重启的情况，如果有，
 *   向用户显示强烈建议重启的警告消息。
 *
 *   可能需要重启的条件包括：
 *   1. 安装期间检测到已加载的 NVIDIA 内核模块（旧模块仍在运行）
 *   2. 安装期间检测到运行中的 X Server
 *   3. nouveau 驱动仍在运行（禁用配置需要重启才能生效）
 *
 * 参数:
 *   op - 全局选项结构体指针
 */

void suggest_reboot(Options *op)
{
    char *reason = nvstrdup("");

    /* 条件 1: 已加载的 NVIDIA 内核模块 */
    if (op->loaded_kernel_module_detected) {
        add_bullet_list_item("Existing NVIDIA kernel modules were loaded "
                             "during installation, and are likely still "
                             "loaded.", &reason);
    }

    /* 条件 2: 运行中的 X Server */
    if (op->running_x_server_detected) {
        add_bullet_list_item("A running X server was detected during "
                             "installation.", &reason);
    }

    /* 条件 3: nouveau 驱动仍在运行 */
    if (nouveau_is_present()) {
        add_bullet_list_item("Nouveau is running: any attempt to disable it "
                             "will not take effect until after a reboot.",
                             &reason);
    }

    /* 如果存在任何需要重启的条件，显示警告 */
    if (reason[0]) {
        ui_warn(op, "It is strongly recommended that you reboot your computer "
                    "after exiting the installer, due to the following "
                    "condition(s) which the installer detected: \n\n%s\n"
                    "If you continue to use the computer without rebooting, "
                    "you may not be able to start new programs which use the "
                    "NVIDIA GPU(s) until after you reboot or reload the NVIDIA "
                    "kernel modules.", reason);
    }

    nvfree(reason);
}
