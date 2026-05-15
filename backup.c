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
 * backup.c - this source file contains functions used for backing up
 * (and restoring) files that need to be moved out of the way during
 * installation.
 *
 * 【文件说明】nvidia-installer 备份与卸载子系统实现。
 *
 * 本文件是 nvidia-installer 项目的核心模块之一，负责在安装 NVIDIA 驱动时：
 *   1. 备份被替换的文件和符号链接（保存到 /var/lib/nvidia/ 目录）
 *   2. 记录所有安装操作到日志文件（/var/lib/nvidia/log）
 *   3. 卸载时根据日志文件回滚所有更改（删除已安装文件、恢复备份文件）
 *   4. 检测是否已安装旧版驱动，并支持运行旧版卸载程序
 *   5. 验证已安装文件的完整性（CRC 校验）
 *
 * 备份日志文件格式（/var/lib/nvidia/log）：
 *   - 第1行：驱动版本字符串（向后兼容格式，如 "1.0-105917 (105.9.17)"）
 *   - 第2行：驱动描述信息
 *   - 后续行：文件操作条目，每个条目由类型编号和相关信息组成
 *     - INSTALLED_FILE (1): 安装了一个文件，附带 CRC 校验值
 *     - INSTALLED_SYMLINK (0): 创建了一个符号链接，附带目标路径
 *     - BACKED_UP_SYMLINK (2): 备份了一个被替换的符号链接
 *     - BACKED_UP_FILE_NUM (100+): 备份了一个被替换的文件
 *
 * 目录创建日志文件（/var/lib/nvidia/dirs）：
 *   - 每行一个目录路径，记录安装过程中新创建的目录
 *   - 卸载时按路径长度降序删除（确保子目录先于父目录被删除）
 *
 * 主要函数一览：
 *   - init_backup()          : 初始化备份系统（创建目录、写入版本信息）
 *   - do_backup()            : 备份单个文件或符号链接
 *   - log_install_file()     : 记录已安装的文件
 *   - log_create_symlink()   : 记录已创建的符号链接
 *   - log_mkdir()            : 记录已创建的目录
 *   - do_uninstall()         : 执行卸载操作（删除已安装文件、恢复备份）
 *   - check_for_existing_driver()    : 检查是否已安装驱动
 *   - uninstall_existing_driver()    : 卸载现有驱动
 *   - run_existing_uninstaller()     : 运行旧版 nvidia-uninstall 程序
 *   - test_installed_files()         : 验证已安装文件完整性
 *   - find_installed_file()          : 在备份日志中查找指定文件
 *   - get_installed_driver_version_and_descr() : 获取已安装驱动版本和描述
 */

#include <sys/types.h>   /* mode_t, uid_t, gid_t 等 POSIX 类型 */
#include <sys/stat.h>    /* stat(), fstat(), S_ISREG(), S_ISLNK() 等文件状态相关 */
#include <fcntl.h>       /* open(), O_RDONLY 等文件控制相关 */
#include <string.h>      /* strlen(), strcmp(), strerror() 等字符串操作 */
#include <errno.h>       /* errno 全局错误号 */
#include <unistd.h>      /* access(), unlink(), close(), rmdir() 等 POSIX 系统调用 */
#include <stdio.h>       /* fopen(), fprintf(), fclose() 等标准 I/O */
#include <unistd.h>      /* 重复包含（原代码如此，可能是历史原因） */
#include <sys/mman.h>    /* mmap(), munmap() 内存映射文件 */
#include <ctype.h>       /* isdigit(), isspace() 字符分类函数 */
#include <stdlib.h>      /* strtol(), strtoul(), qsort() 等标准库函数 */

#include "nvidia-installer.h"            /* 核心数据结构定义（Options, Package 等） */
#include "user-interface.h"              /* 用户界面函数（ui_error, ui_message 等） */
#include "backup.h"                      /* 备份子系统头文件（类型常量定义） */
#include "files.h"                       /* 文件操作工具函数 */
#include "crc.h"                         /* CRC 校验计算函数 */
#include "misc.h"                        /* 杂项工具函数 */
#include "kernel.h"                      /* 内核模块相关操作 */
#include "conflicting-kernel-modules.h"  /* 冲突内核模块列表定义 */

/*
 * 备份目录和日志文件的路径常量定义。
 * BACKUP_DIRECTORY: 备份文件的存储根目录
 * BACKUP_LOG:       备份操作日志文件路径（记录安装了哪些文件、备份了哪些文件）
 * BACKUP_MKDIR_LOG: 目录创建日志文件路径（记录安装过程中新创建的目录）
 */
#define BACKUP_DIRECTORY "/var/lib/nvidia"
#define BACKUP_LOG       (BACKUP_DIRECTORY "/log")
#define BACKUP_MKDIR_LOG (BACKUP_DIRECTORY "/dirs")




/*
 * 备份日志文件的格式规范：
 *
 * 1. 第一行是版本字符串，假定格式为：MAJOR.MINOR-PATCH
 *    （实际使用向后兼容格式，如 "1.0-XXXXXX (实际版本号)"）
 *
 * 2. 第二行是驱动描述字符串。
 *
 * XXX 是否需要区分正式版本、每日构建版等不同构建类型？（原作者的待办事项）
 *
 * 3. 文件的其余部分是文件条目，每个条目可以是以下类型之一：
 *
 * INSTALLED_FILE（类型编号 1）: <文件名>
 *   安装了一个新文件，下一行是该文件的 CRC 校验值
 *
 * INSTALLED_SYMLINK（类型编号 0）: <文件名>
 *  <目标路径>
 *   创建了一个符号链接，下一行是链接目标
 *
 * BACKED_UP_SYMLINK（类型编号 2）: <文件名>
 *  <目标路径>
 *  <权限> <所有者ID> <组ID>
 *   备份了一个被替换的符号链接，记录其目标和属性信息
 *
 * BACKED_UP_FILE_NUM（类型编号 >= 100）: <文件名>
 *  <CRC校验值> <权限> <所有者ID> <组ID>
 *   备份了一个被替换的普通文件，记录其 CRC 和属性信息
 *   文件被移动到 /var/lib/nvidia/<编号> 中存储
 */

/*
 * 备份日志文件的权限：仅所有者可读写（0600）。
 * 这是安全设计，防止非 root 用户篡改备份日志。
 */
#define BACKUP_LOG_PERMS (S_IRUSR|S_IWUSR)

/*
 * 备份目录的权限：仅所有者可读写和进入（0700）。
 * 同样是安全设计，保护备份文件不被非授权用户访问。
 */
#define BACKUP_DIRECTORY_PERMS (S_IRUSR|S_IWUSR|S_IXUSR)


/*
 * BackupLogEntry - 备份日志中单个条目的数据结构。
 * 每个条目对应日志文件中的一个文件操作记录。
 */

typedef struct {

    int    num;       /* 条目类型编号：
                       *   INSTALLED_FILE (1) = 安装的文件
                       *   INSTALLED_SYMLINK (0) = 安装的符号链接
                       *   BACKED_UP_SYMLINK (2) = 备份的符号链接
                       *   >= BACKED_UP_FILE_NUM (100) = 备份的文件（编号即为备份文件名） */
    char  *filename;  /* 原始文件的完整路径 */
    char  *target;    /* 符号链接的目标路径（仅对符号链接类型有意义） */
    uint32 crc;       /* 文件的 CRC32 校验值（用于完整性验证） */
    mode_t mode;      /* 文件权限（如 0644），用于卸载时恢复原始权限 */
    uid_t  uid;       /* 文件所有者用户 ID，用于卸载时恢复所有者 */
    gid_t  gid;       /* 文件所有组 ID，用于卸载时恢复所属组 */
    int    ok;        /* 此条目是否有效/是否应该被处理（在检查阶段可能被设为 FALSE） */

} BackupLogEntry;


/*
 * BackupInfo - 从备份日志文件解析出的完整备份信息。
 * 包含驱动版本、描述以及所有日志条目。
 */
typedef struct {
    char *version;       /* 已安装驱动的版本字符串 */
    char *description;   /* 已安装驱动的描述信息 */
    BackupLogEntry *e;   /* 日志条目数组（动态分配） */
    int n;               /* 日志条目的数量 */
} BackupInfo;


/* ======== 静态（内部）函数前向声明 ======== */

/* 读取并解析备份日志文件，返回 BackupInfo 结构体 */
static BackupInfo *read_backup_log_file(Options *op);

/* 释放 BackupInfo 结构体及其关联的所有内存 */
static void free_backup_info(BackupInfo *b);

/* 检查备份日志条目的有效性（用于卸载前的验证） */
static int check_backup_log_entries(Options *op, BackupInfo *b);

/* 执行卸载操作的内部实现 */
static int do_uninstall(Options *op, const char *version,
                        const int skip_depmod);

/* 完整性检查备份日志条目（用于 --sanity 模式） */
static int sanity_check_backup_log_entries(Options *op, BackupInfo *b);

/* 创建向后兼容的版本字符串（兼容旧版 nvidia-installer） */
static char *create_backwards_compatible_version_string(const char *str);

/* 按字符串长度降序排序的比较函数（用于 qsort） */
static int reverse_strlen_compare(const void *a, const void *b);





/*
 * init_backup() - 初始化备份引擎。
 *
 * 功能：
 *   1. 如果备份目录已存在，先删除整个备份目录（清除上次安装的残留）
 *   2. 创建新的备份目录 /var/lib/nvidia/，权限为 0700
 *   3. 创建备份日志文件 /var/lib/nvidia/log，权限为 0600
 *   4. 向日志文件写入驱动版本号和描述信息（作为日志文件的前两行）
 *
 * 参数：
 *   op - Options 结构体指针，包含安装器的全局配置选项
 *   p  - Package 结构体指针，包含待安装驱动包的信息（版本、描述等）
 *
 * 返回值：
 *   TRUE  - 初始化成功
 *   FALSE - 初始化失败（无法创建目录或日志文件）
 */

int init_backup(Options *op, Package *p)
{
    mode_t orig_mode;
    char *version;
    FILE *log;

    /* 如果备份目录已存在，先将其完全删除（清除上次安装的残留数据） */

    if (directory_exists(BACKUP_DIRECTORY)) {
        if (!remove_directory(op, BACKUP_DIRECTORY)) {
            return FALSE;
        }
    }

    /* 创建备份目录，权限设为仅所有者可访问（0700） */

    if (!mkdir_recursive(op, BACKUP_DIRECTORY, BACKUP_DIRECTORY_PERMS, FALSE)) {
        return FALSE;
    }

    /*
     * 下面的 fopen() 创建文件时使用的模式为 0666 & ~umask。
     * 为了确保最终的文件权限恰好为 BACKUP_LOG_PERMS（0600），
     * 我们临时将 umask 设为 ~BACKUP_LOG_PERMS（即 0177），
     * 这样 0666 & ~0177 = 0600。
     *
     * 前提条件：BACKUP_LOG 文件尚不存在（如果已存在，fopen 不会修改权限）。
     * 上面的目录删除和重建操作已经确保了这一点。
     */
    orig_mode = umask(~BACKUP_LOG_PERMS);

    /* 以追加模式创建日志文件 */

    log = fopen(BACKUP_LOG, "a");
    umask(orig_mode);  /* 立即恢复原始 umask，避免影响后续文件操作 */
    if (!log) {
        ui_error(op, "Unable to create backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    /*
     * 向日志文件写入版本号和描述信息。
     * 版本号使用向后兼容格式，以便旧版 nvidia-installer 也能解析。
     */

    version = create_backwards_compatible_version_string(p->version);

    fprintf(log, "%s\n", version);
    fprintf(log, "%s\n", p->description);

    nvfree(version);

    /* 关闭日志文件，并检查关闭操作是否成功 */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    return TRUE;

} /* init_backup() */



/*
 * do_backup() - 备份指定的文件。
 *
 * 功能：
 *   根据文件类型执行不同的备份策略：
 *   - 普通文件：移动到备份目录（/var/lib/nvidia/<编号>），并在日志中记录
 *     原始路径、CRC 校验值、权限、所有者和所属组
 *   - 符号链接：记录链接目标和属性信息后删除链接（链接本身不需要物理备份）
 *   - 目录：当前不支持（报错）
 *   - 其他类型：不支持（报错）
 *
 * 参数：
 *   op       - Options 结构体指针，包含安装器的全局配置选项
 *   filename - 需要备份的文件的完整路径
 *
 * 返回值：
 *   TRUE  - 备份成功
 *   FALSE - 备份失败
 *
 * 注意：
 *   backup_file_number 是一个静态变量，从 BACKED_UP_FILE_NUM (100) 开始递增。
 *   每备份一个普通文件，编号加 1。该编号同时作为备份目录中的文件名和日志条目标识。
 */

int do_backup(Options *op, const char *filename)
{
    int len, ret, ret_val;
    struct stat stat_buf;
    char *tmp = NULL;
    FILE *log;
    uint32 crc;

    /* 静态变量：备份文件编号计数器，从 BACKED_UP_FILE_NUM (100) 开始自增 */
    static int backup_file_number = BACKED_UP_FILE_NUM;

    ret_val = FALSE;

    /* 以追加模式打开备份日志文件 */
    log = fopen(BACKUP_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    /*
     * 使用 lstat() 获取文件属性（lstat 不跟随符号链接，
     * 这样可以正确识别符号链接本身而非其目标文件）
     */
    if (lstat(filename, &stat_buf) == -1) {
        switch (errno) {
        case ENOENT:
            /* 文件不存在：不需要备份，视为成功 */
            ret_val = TRUE;
            break;
        default:
            ui_error(op, "Unable to determine properties for file '%s' (%s).",
                     filename, strerror(errno));
        }
        goto done;
    }

    /* 处理普通文件：计算 CRC、移动到备份目录、记录日志 */
    if (S_ISREG(stat_buf.st_mode)) {
        /* 计算文件的 CRC32 校验值，用于卸载时验证备份完整性 */
        crc = compute_crc(op, filename);

        /* 构建备份目标路径：/var/lib/nvidia/<备份文件编号> */
        len = strlen(BACKUP_DIRECTORY) + 64;
        tmp = nvalloc(len + 1);
        snprintf(tmp, len, "%s/%d", BACKUP_DIRECTORY, backup_file_number);

        /* 将原始文件移动（重命名）到备份目录 */
        if (!nvrename(op, filename, tmp)) {
            ui_error(op, "Unable to backup file '%s'.", filename);
            goto done;
        }

        /* 在日志中记录：备份文件编号和原始文件路径 */
        fprintf(log, "%d: %s\n", backup_file_number, filename);

        /* 在日志中记录：CRC 校验值、文件权限（八进制）、所有者 ID、所属组 ID */
        fprintf(log, "%u %04o %d %d\n", crc, stat_buf.st_mode,
                stat_buf.st_uid, stat_buf.st_gid);

        /* 递增备份文件编号，为下一个备份文件做准备 */
        backup_file_number++;

    /* 处理符号链接：记录链接信息后删除链接 */
    } else if (S_ISLNK(stat_buf.st_mode)) {
        /* 获取符号链接的目标路径 */
        tmp = get_symlink_target(op, filename);

        /* 删除符号链接（链接本身不需要物理备份，只需记录目标和属性信息） */
        ret = unlink(filename);
        if (ret == -1) {
            ui_error(op, "Unable to remove symbolic link '%s' (%s).",
                     filename, strerror(errno));
            goto done;
        }

        /* 在日志中记录：备份类型标识（BACKED_UP_SYMLINK）和原始链接路径 */
        fprintf(log, "%d: %s\n", BACKED_UP_SYMLINK, filename);
        /* 记录链接目标路径 */
        fprintf(log, "%s\n", tmp);
        /* 记录权限（八进制）、所有者 ID、所属组 ID */
        fprintf(log, "%04o %d %d\n", stat_buf.st_mode,
                stat_buf.st_uid, stat_buf.st_gid);

    /* 处理目录：当前未实现，报错 */
    } else if (S_ISDIR(stat_buf.st_mode)) {

        /* XXX 待实现：递归移动目录（原作者的待办事项） */

        ui_error(op, "Unable to backup directory '%s'.", filename);
        goto done;

    /* 未知文件类型：报错 */
    } else {
        ui_error(op, "Unable to backup file '%s' (don't know how to deal with "
                 "file type).", filename);
        goto done;
    }

    ret_val = TRUE;

 done:

    nvfree(tmp);

    /* 关闭日志文件，并检查关闭操作是否成功 */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        ret_val = FALSE;
    }

    return ret_val;

} /* do_backup() */



/*
 * log_install_file() - 在备份日志中记录已安装的文件。
 *
 * 功能：
 *   当安装器向系统安装了一个新文件时，调用此函数将文件路径和
 *   CRC 校验值写入备份日志。卸载时将根据此记录删除该文件。
 *
 * 参数：
 *   op       - Options 结构体指针，包含安装器的全局配置选项
 *   filename - 已安装文件的完整路径
 *
 * 返回值：
 *   TRUE  - 记录成功
 *   FALSE - 记录失败（无法打开或关闭日志文件）
 *
 * 日志格式：
 *   第1行：<INSTALLED_FILE 类型编号>: <文件路径>
 *   第2行：<CRC32 校验值>
 */

int log_install_file(Options *op, const char *filename)
{
    FILE *log;
    uint32 crc;

    /* 以追加模式打开备份日志文件 */

    log = fopen(BACKUP_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    /* 写入条目类型（INSTALLED_FILE = 1）和文件路径 */
    fprintf(log, "%d: %s\n", INSTALLED_FILE, filename);

    /* 计算并写入文件的 CRC32 校验值 */
    crc = compute_crc(op, filename);

    fprintf(log, "%u\n", crc);

    /* 关闭日志文件 */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    return TRUE;

} /* log_install_file() */



/*
 * log_create_symlink() - 在备份日志中记录已创建的符号链接。
 *
 * 功能：
 *   当安装器创建了一个新的符号链接时，调用此函数将链接路径和
 *   目标路径写入备份日志。卸载时将根据此记录删除该符号链接。
 *
 * 参数：
 *   op       - Options 结构体指针，包含安装器的全局配置选项
 *   filename - 已创建的符号链接的完整路径
 *   target   - 符号链接的目标路径
 *
 * 返回值：
 *   TRUE  - 记录成功
 *   FALSE - 记录失败（无法打开或关闭日志文件）
 *
 * 日志格式：
 *   第1行：<INSTALLED_SYMLINK 类型编号>: <链接路径>
 *   第2行：<目标路径>
 */

int log_create_symlink(Options *op, const char *filename, const char *target)
{
    FILE *log;

    /* 以追加模式打开备份日志文件 */

    log = fopen(BACKUP_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    /* 写入条目类型（INSTALLED_SYMLINK = 0）和符号链接路径 */
    fprintf(log, "%d: %s\n", INSTALLED_SYMLINK, filename);
    /* 写入符号链接的目标路径 */
    fprintf(log, "%s\n", target);

    /* 关闭日志文件 */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing backup log file '%s' (%s).",
                 BACKUP_LOG, strerror(errno));
        return FALSE;
    }

    return TRUE;

} /* log_create_symlink() */



/*
 * parse_first_line() - 解析日志条目的第一行。
 *
 * 功能：
 *   解析格式为 "<数字>: <文件名>" 的行，提取条目类型编号和文件名。
 *
 * 参数：
 *   buf      - 待解析的字符串（一行日志内容）
 *   num      - [输出] 解析得到的条目类型编号
 *   filename - [输出] 解析得到的文件名（动态分配，调用者需释放）
 *
 * 返回值：
 *   TRUE  - 解析成功
 *   FALSE - 解析失败（输入为空、格式不正确等）
 *
 * 解析规则：
 *   1. 从行首读取连续的数字字符，遇到 ':' 停止
 *   2. 将 ':' 前的数字部分转换为整数（条目类型编号）
 *   3. 跳过 ':' 之后的空白字符
 *   4. 剩余内容作为文件名
 */
static int parse_first_line(const char *buf, int *num, char **filename)
{
    char *c, *local_buf;

    /* 参数有效性检查 */
    if (!buf || !num || !filename) return FALSE;

    /* 复制输入字符串以避免修改原始数据 */
    local_buf = nvstrdup(buf);

    /* 扫描字符直到遇到 ':'，确保 ':' 之前全是数字 */
    c = local_buf;
    while ((*c != '\0') && (*c != ':')) {
        if (!isdigit(*c)) return FALSE;
        c++;
    }
    /* 如果到达字符串末尾仍未找到 ':'，则格式错误 */
    if (*c == '\0') return FALSE;

    /* 用 '\0' 替换 ':'，将数字部分变成独立字符串 */
    *c = '\0';

    /* 将数字字符串转换为整数（十进制） */
    *num = strtol(local_buf, NULL, 10);

    /* 跳过 ':' 后的空白字符，定位到文件名的起始位置 */
    c++;
    while (isspace(*c)) c++;

    /* 复制文件名字符串 */
    *filename = nvstrdup(c);

    free(local_buf);

    return TRUE;
}


/*
 * parse_mode_uid_gid() - 解析备份符号链接的属性行。
 *
 * 功能：
 *   解析格式为 "<权限> <UID> <GID>" 的行。
 *   用于 BACKED_UP_SYMLINK 类型条目的第三行。
 *
 * 参数：
 *   buf  - 待解析的字符串
 *   mode - [输出] 文件权限（八进制解析）
 *   uid  - [输出] 所有者用户 ID（十进制解析）
 *   gid  - [输出] 所属组 ID（十进制解析）
 *
 * 返回值：
 *   TRUE  - 解析成功
 *   FALSE - 解析失败
 *
 * 注意：权限以八进制形式存储在日志中（如 0777），解析时按八进制转换。
 */
static int parse_mode_uid_gid(const char *buf, mode_t *mode,
                              uid_t *uid, gid_t *gid)
{
    char *c, *local_buf, *str;

    /* 参数有效性检查 */
    if (!buf || !mode || !uid || !gid) return FALSE;

    local_buf = nvstrdup(buf);

    /* 解析权限字段（八进制数字字符串） */
    c = str = local_buf;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *mode = strtol(str, NULL, 8);  /* 按八进制解析权限 */

    /* 解析 UID 字段（十进制） */
    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *uid = strtol(str, NULL, 10);

    /* 解析 GID 字段（十进制） */
    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    *c = '\0';
    *gid = strtol(str, NULL, 10);

    free(local_buf);

    return TRUE;
}


/*
 * parse_crc_mode_uid_gid() - 解析备份文件的属性行。
 *
 * 功能：
 *   解析格式为 "<CRC> <权限> <UID> <GID>" 的行。
 *   用于 BACKED_UP_FILE_NUM 类型条目的第二行。
 *
 * 参数：
 *   buf  - 待解析的字符串
 *   crc  - [输出] CRC32 校验值（十进制解析）
 *   mode - [输出] 文件权限（八进制解析）
 *   uid  - [输出] 所有者用户 ID（十进制解析）
 *   gid  - [输出] 所属组 ID（十进制解析）
 *
 * 返回值：
 *   TRUE  - 解析成功
 *   FALSE - 解析失败
 */
static int parse_crc_mode_uid_gid(const char *buf, uint32 *crc, mode_t *mode,
                                  uid_t *uid, gid_t *gid)
{
    char *c, *local_buf, *str;

    /* 参数有效性检查 */
    if (!buf || !crc || !mode || !uid || !gid) return FALSE;

    local_buf = nvstrdup(buf);

    /* 解析 CRC 字段（十进制无符号整数） */
    c = str = local_buf;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *crc = strtoul(str, NULL, 10);

    /* 解析权限字段（八进制） */
    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *mode = strtol(str, NULL, 8);

    /* 解析 UID 字段（十进制） */
    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    if (*c == '\0') return FALSE;
    *c = '\0';
    *uid = strtol(str, NULL, 10);

    /* 解析 GID 字段（十进制） */
    str = ++c;
    while ((*c != '\0') && (isdigit(*c))) c++;
    *c = '\0';
    *gid = strtol(str, NULL, 10);

    free(local_buf);

    return TRUE;
}


/*
 * parse_crc() - 从字符串中解析 CRC32 校验值。
 *
 * 功能：
 *   解析只包含 CRC 值的行。
 *   用于 INSTALLED_FILE 类型条目的第二行。
 *
 * 参数：
 *   buf - 待解析的字符串
 *   crc - [输出] CRC32 校验值（十进制解析为无符号整数）
 *
 * 返回值：
 *   TRUE  - 解析成功
 *   FALSE - 解析失败（输入为空）
 */
static int parse_crc(const char *buf, uint32 *crc)
{
    char *c, *local_buf, *str;

    if (!buf || !crc) return FALSE;

    local_buf = nvstrdup(buf);

    /* 提取连续的数字字符，转换为无符号整数 */
    c = str = local_buf;
    while ((*c != '\0') && (isdigit(*c))) c++;
    *c = '\0';
    *crc = strtoul(str, NULL, 10);

    free(local_buf);

    return TRUE;

} /* parse_crc() */


/*
 * 目录创建日志文件（BACKUP_MKDIR_LOG）的格式说明：
 *
 * 文件中每行包含一个安装过程中创建的目录的完整路径。
 *
 * XXX 包含换行符 '\n' 的路径名会破坏此文件和常规备份日志文件的解析。
 * （原作者的已知限制，未实现对此的处理）
 */


/*
 * log_mkdir() - 记录安装过程中创建的目录。
 *
 * 功能：
 *   将以换行符分隔的目录列表追加到目录创建日志文件中。
 *   卸载时会读取此日志文件，按路径长度降序删除这些目录。
 *
 * 参数：
 *   op   - Options 结构体指针，包含安装器的全局配置选项
 *   dirs - 以换行符分隔的目录路径列表
 *
 * 返回值：
 *   TRUE  - 记录成功
 *   FALSE - 记录失败
 *
 * 注意：
 *   此函数会在需要时自动创建备份目录 BACKUP_DIRECTORY。
 *   这是因为 BACKUP_MKDIR_LOG 位于 BACKUP_DIRECTORY 内部，
 *   而 log_mkdir() 可能在 init_backup() 之前被调用。
 */
int log_mkdir(Options *op, const char *dirs)
{
    FILE *log;

    /*
     * 如果备份目录不存在则创建它。
     * BACKUP_MKDIR_LOG 位于 BACKUP_DIRECTORY 内部，
     * 所以下面的 fopen() 调用依赖于 BACKUP_DIRECTORY 的存在。
     */
    if (!directory_exists(BACKUP_DIRECTORY) &&
        !mkdir_recursive(op, BACKUP_DIRECTORY, BACKUP_DIRECTORY_PERMS, FALSE)) {
        return FALSE;
    }

    /* 以追加模式打开目录创建日志文件 */

    log = fopen(BACKUP_MKDIR_LOG, "a");
    if (!log) {
        ui_error(op, "Unable to open mkdir log file '%s' (%s).",
                 BACKUP_MKDIR_LOG, strerror(errno));
        return FALSE;
    }

    /* 将目录列表写入日志 */
    fprintf(log, "%s", dirs);

    /* 关闭日志文件 */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing mkdir log file '%s' (%s).",
                 BACKUP_MKDIR_LOG, strerror(errno));
        return FALSE;
    }

    return TRUE;
}



/*
 * reverse_strlen_compare() - 按字符串长度降序排序的比较函数。
 *
 * 功能：
 *   用于 qsort()，将字符串数组按长度从大到小排序。
 *   在 rmdir_recursive() 中使用，确保子目录（路径较长）
 *   在父目录（路径较短）之前被删除。
 *
 * 参数：
 *   a - 指向字符串指针的指针（qsort 要求的格式）
 *   b - 指向字符串指针的指针（qsort 要求的格式）
 *
 * 返回值：
 *   > 0 如果 b 的长度大于 a（b 排在前面）
 *   = 0 如果长度相同
 *   < 0 如果 a 的长度大于 b（a 排在前面）
 */
static int reverse_strlen_compare(const void *a, const void *b)
{
    return strlen(*(char * const *)b) - strlen(*(char * const *)a);
}



/*
 * rmdir_recursive() - 删除安装过程中创建的目录。
 *
 * 功能：
 *   读取目录创建日志文件（BACKUP_MKDIR_LOG），删除其中记录的所有目录。
 *   日志条目先按路径长度降序排序，确保子目录在父目录之前被删除
 *  （rmdir 只能删除空目录，所以必须先删子目录）。
 *
 * 参数：
 *   op - Options 结构体指针
 *
 * 返回值：
 *   TRUE  - 成功找到日志文件并删除了所有目录
 *   FALSE - 日志文件不存在，或部分目录删除失败
 *
 * 处理流程：
 *   1. 打开目录创建日志文件
 *   2. 第一遍扫描：计算总行数
 *   3. 第二遍扫描：读取所有目录路径
 *   4. 按路径长度降序排序
 *   5. 依次尝试删除每个目录（跳过空行和备份目录本身）
 *   6. 如有目录删除失败则发出警告
 */
static int rmdir_recursive(Options *op)
{
    FILE *log;
    char **dirs;
    int eof = FALSE, ret = TRUE, lines, i;

    /* 打开目录创建日志文件 */

    log = fopen(BACKUP_MKDIR_LOG, "r");
    if (!log) {
        /*
         * 静默失败：最可能的原因是当前驱动由不支持目录日志功能的
         * 旧版 nvidia-installer 安装。
         */
        return FALSE;
    }

    /* 第一遍扫描：计算日志文件中的总行数 */

    for (lines = 0; !eof; lines++) {
        char *dir;
        dir = fget_next_line(log, &eof);
        nvfree(dir);
    }

    /* 重置文件指针到文件开头，准备第二遍读取 */
    rewind(log);

    /* 分配字符串指针数组，用于存储所有目录路径 */
    dirs = nvalloc(lines * sizeof(char*));

    /* 第二遍扫描：读取所有目录路径 */
    for (i = 0; i < lines; i++) {
        dirs[i] = fget_next_line(log, &eof);
    }

    /*
     * 按路径长度降序排序。
     * 这确保子目录（路径更长）在父目录之前被删除，
     * 因为 rmdir() 只能删除空目录。
     */
    qsort(dirs, lines, sizeof(char*), reverse_strlen_compare);

    /* 依次尝试删除每个目录 */
    for (i = 0; i < lines; i++) {
        if (dirs[i]) {
            /*
             * 跳过空行和备份目录本身。
             * 备份目录不能在此处删除，因为目录日志文件还在其中，
             * 且备份目录会在 do_uninstall() 的后续步骤中统一删除。
             */
            if (strlen(dirs[i]) && strcmp(dirs[i], BACKUP_DIRECTORY) != 0) {
                if (rmdir(dirs[i]) != 0) {
                    ui_log(op, "Failed to delete the directory '%s' (%s).",
                           dirs[i], strerror(errno));
                    ret = FALSE;
                }
            }
        }
        nvfree(dirs[i]);
    }

    nvfree(dirs);

    /* 如有目录删除失败，向用户发出警告 */
    if (!ret) {
        ui_warn(op, "Failed to delete some directories. See %s for details.",
                op->log_file_name);
    }

    /* 关闭目录创建日志文件 */

    if (fclose(log) != 0) {
        ui_error(op, "Error while closing mkdir log file '%s' (%s).",
                 BACKUP_MKDIR_LOG, strerror(errno));
        return FALSE;
    }

    return ret;
}



/*
 * do_uninstall() - 根据备份日志文件执行驱动卸载操作。
 *
 * 功能：
 *   解析备份日志文件（BACKUP_LOG），执行以下两个步骤：
 *     步骤 1：删除所有之前安装的文件和符号链接
 *     步骤 2：恢复所有之前备份的文件和符号链接（包括权限和所有者信息）
 *   此外还会：
 *     - 移除 DKMS 模块（如果有）
 *     - 卸载冲突的内核模块
 *     - 运行 depmod 和 ldconfig 更新系统缓存
 *     - 重新加载 systemd 守护进程配置
 *     - 删除安装过程中创建的目录
 *     - 删除备份目录本身
 *
 * 参数：
 *   op          - Options 结构体指针，包含安装器的全局配置选项
 *   version     - 要卸载的驱动版本号字符串
 *   skip_depmod - 如果为 TRUE，跳过运行 depmod（当即将安装新驱动时可跳过）
 *
 * 返回值：
 *   TRUE  - 卸载成功
 *   FALSE - 卸载失败（备份目录不存在、日志解析失败等）
 */

static int do_uninstall(Options *op, const char *version,
                        const int skip_depmod)
{
    BackupLogEntry *e;
    BackupInfo *b;
    int i, len, ok;
    char *tmpstr;
    float percent;
    int removal_failed = FALSE, restore_failed = FALSE;

    /*
     * 安装被篡改时的警告信息。
     * 当检测到已安装文件与备份日志不一致时（例如用户通过包管理器
     * 另行安装了驱动），向用户显示此警告。
     */
    static const char existing_installation_is_borked[] =
        "Your driver installation has been "
        "altered since it was initially installed; this may happen, "
        "for example, if you have since installed the NVIDIA driver through "
        "a mechanism other than nvidia-installer (such as your "
        "distribution's native package management system).  "
        "nvidia-installer will attempt to uninstall as best it can.";

    /* 检查备份目录是否存在 */

    if (access(BACKUP_DIRECTORY, F_OK) == -1) {
        ui_message(op, "No driver backed up.");
        return FALSE;
    }

    /* 读取并解析备份日志文件 */
    if ((b = read_backup_log_file(op)) == NULL) return FALSE;

    /* 验证所有备份日志条目的有效性（CRC 校验、文件存在性等） */
    ok = check_backup_log_entries(op, b);

    /* 如果检测到不一致，向用户发出警告 */
    if (!ok) {
        if (op->logging) {
            ui_warn(op, "%s  Please see the file '%s' for details.",
                    existing_installation_is_borked, op->log_file_name);

        } else {
            ui_warn(op, "%s", existing_installation_is_borked);
        }
    }

    /* 构建卸载进度提示字符串 */
    tmpstr = nvstrcat("Uninstalling ", b->description, " (",
                      b->version, "):", NULL);

    /* 运行发行版特定的卸载前钩子脚本 */
    run_distro_hook(op, "pre-uninstall");

    /* 开始显示卸载进度条 */
    ui_status_begin(op, tmpstr, "Uninstalling");

    free(tmpstr);

    /* 如果检测到已安装的 DKMS 模块，先移除它们 */

    if (dkms_module_installed(op, version, NULL)) {
        ui_log(op, "DKMS module detected; removing...");
        if (!dkms_remove_module(op, version)) {
            ui_warn(op, "Failed to remove installed DKMS module!");
        }
    }

    /*
     * 根据备份日志条目执行卸载操作，分为两个阶段：
     *
     * 阶段 1：删除所有之前安装的文件和符号链接
     *   遍历所有条目，删除 INSTALLED_FILE 和 INSTALLED_SYMLINK 类型的文件
     *
     * 阶段 2：恢复所有之前备份的文件和符号链接
     *   将备份文件移回原位，重建备份的符号链接，恢复权限和所有者
     */

    /* ===== 阶段 1：删除已安装的文件和符号链接 ===== */
    for (i = 0; i < b->n; i++) {

        /* 计算进度：前半段（0% ~ 50%）用于删除操作 */
        percent = (float) i / (float) (b->n * 2);

        e = &b->e[i];

        /* 跳过检查阶段标记为无效的条目 */
        if (!e->ok) continue;
        switch (e->num) {

            /*
             * 这是一个之前安装的文件 -- 现在删除它。
             */

        case INSTALLED_FILE:
            if (unlink(e->filename) == -1) {
                ui_log(op, "Unable to remove installed file '%s' (%s).",
                       e->filename, strerror(errno));
                removal_failed = TRUE;
            }
            ui_status_update(op, percent, "%s", e->filename);
            break;

        case INSTALLED_SYMLINK:
            if (unlink(e->filename) == -1) {
                ui_log(op, "Unable to remove installed symlink '%s' (%s).",
                       e->filename, strerror(errno));
                removal_failed = TRUE;
            }
            ui_status_update(op, percent, "%s", e->filename);
            break;
        }
    }

    /* ===== 阶段 2：恢复备份的文件和符号链接 ===== */
    for (i = 0; i < b->n; i++) {

        /* 计算进度：后半段（50% ~ 100%）用于恢复操作 */
        percent = (float) (i + b->n) / (float) (b->n * 2);

        e = &b->e[i];

        /* 跳过检查阶段标记为无效的条目 */
        if (!e->ok) continue;

        switch (e->num) {

          case INSTALLED_FILE:
          case INSTALLED_SYMLINK:
            /* 已安装的文件/链接在阶段 1 中已删除，此处无需操作 */
            break;

          case BACKED_UP_SYMLINK:
            /* 恢复之前备份的符号链接 */
            if (symlink(e->target, e->filename) == -1) {

                /*
                 * XXX 仅在 check_backup_log_entries() 未发现问题时才打印此警告。
                 * （如果检查阶段已经报告了问题，恢复失败可能是预期行为）
                 */

                if (ok) {
                    restore_failed = TRUE;
                }

                ui_log(op, "Unable to restore symbolic link "
                       "%s -> %s (%s).", e->filename, e->target,
                       strerror(errno));
            } else {

                /* XXX 是否需要对符号链接执行 chmod？（原作者的疑问） */

                /* 恢复符号链接的所有者和所属组 */
                if (lchown(e->filename, e->uid, e->gid)) {
                    ui_log(op, "Unable to restore owner (%d) and group "
                           "(%d) for symbolic link '%s' (%s).",
                           e->uid, e->gid, e->filename, strerror(errno));
                    restore_failed = TRUE;
                }
            }
            ui_status_update(op, percent, "%s", e->filename);
            break;

          default:
            /*
             * 默认分支处理备份的普通文件（编号 >= BACKED_UP_FILE_NUM）。
             * 将备份目录中的文件移回原始位置，并恢复权限和所有者。
             */

            /* 构建备份文件的路径：/var/lib/nvidia/<编号> */
            len = strlen(BACKUP_DIRECTORY) + 64;
            tmpstr = nvalloc(len + 1);
            snprintf(tmpstr, len, "%s/%d", BACKUP_DIRECTORY, e->num);

            /* 将备份文件移回原始位置 */
            if (!nvrename(op, tmpstr, e->filename)) {
                ui_log(op, "Unable to restore file '%s'.", e->filename);
                restore_failed = TRUE;
            } else {
                /* 恢复文件的所有者和所属组 */
                if (chown(e->filename, e->uid, e->gid)) {
                    ui_log(op, "Unable to restore owner (%d) and group "
                           "(%d) for file '%s' (%s).",
                           e->uid, e->gid, e->filename, strerror(errno));
                    restore_failed = TRUE;
                } else {
                    /* 恢复文件权限 */
                    if (chmod(e->filename, e->mode) == -1) {
                        ui_log(op, "Unable to restore permissions %04o for "
                               "file '%s'.", e->mode, e->filename);
                        restore_failed = TRUE;
                    }
                }
            }
            ui_status_update(op, percent, "%s", e->filename);
            free(tmpstr);
            break;
        }
    }

    /* 如果有文件/链接删除失败，向用户发出警告 */
    if (removal_failed) {
        ui_warn(op, "Failed to remove some installed files/symlinks. See %s "
                "for details", op->log_file_name);
    }

    /* 如果有备份恢复失败，向用户发出警告 */
    if (restore_failed) {
        ui_warn(op, "Failed to restore some backed up files/symlinks, and/or "
                "their attributes. See %s for details", op->log_file_name);
    }

    /* 删除安装过程中创建的目录 */
    if (!rmdir_recursive(op)) {
        ui_log(op, "Unable to delete directories created by previous "
               "installation.");
    }

    ui_status_end(op, "done.");

    /* 删除整个备份目录 */

    if (!remove_directory(op, BACKUP_DIRECTORY)) {
        /* XXX 如果删除失败该怎么办？... 目前什么也不做（原作者的已知问题） */
    }

    if (!op->skip_module_unload) {
        /*
         * 尝试卸载内核模块。不会因卸载失败而中止操作，原因包括：
         *   - 内核可能未配置模块卸载支持
         *   - 用户可能已经手动卸载了模块
         *   - 模块可能根本就不存在
         */

        for (i = 0; i < num_conflicting_kernel_modules; i++) {
            rmmod_kernel_module(op, conflicting_kernel_modules[i]);
        }
    }

    if (op->uninstall) {
        /*
         * 运行 depmod 和 ldconfig 更新系统缓存。
         * 这会移除刚刚卸载的 DSO（动态共享对象）和内核模块的缓存条目。
         */

        int status = 0;

        ui_log(op, "Running %sldconfig:", op->skip_depmod ? "" : "depmod and ");

        /* 如果不跳过 depmod，先运行 depmod -a 重建模块依赖关系 */
        if (!op->skip_depmod) {
            status |= run_command(op, NULL, FALSE, NULL, FALSE,
                                  op->utils[DEPMOD], " -a ", op->kernel_name, NULL);
        }

        /* 运行 ldconfig 更新共享库缓存 */
        status |= run_command(op, NULL, FALSE, NULL, FALSE,
                              op->utils[LDCONFIG], NULL);

        if (status == 0) {
            ui_log(op, "done.");
        } else {
            ui_log(op, "error!");
            ui_warn(op, "An error occurred while running depmod or ldconfig "
                    "after uninstallation: your system may have stale state "
                    "involving recently uninstalled files.");
        }

        /*
         * 如果刚刚卸载了 systemd 单元文件，运行 `systemctl daemon-reload`
         * 通知 systemd 重新加载配置。
         */
        if (op->utils[SYSTEMCTL]) {
            char *cmd = nvstrcat(op->utils[SYSTEMCTL], " daemon-reload", NULL);

            ui_log(op, "Running `%s`:", cmd);
            status = run_command(op, NULL, FALSE, NULL, FALSE, cmd, NULL);
            nvfree(cmd);

            if (status == 0) {
                ui_log(op, "done.");
            } else {
                ui_log(op, "error!");
                ui_warn(op, "An error occurred while reloading the systemd "
                        "daemon configuration.");
            }
        }
    }

    /* 运行发行版特定的卸载后钩子脚本 */
    run_distro_hook(op, "post-uninstall");

    /* 释放备份信息结构体 */
    free_backup_info(b);

    return TRUE;

} /* do_uninstall() */





/*
 * read_backup_log_file() - 读取并解析备份日志文件。
 *
 * 功能：
 *   将备份日志文件（/var/lib/nvidia/log）通过 mmap 映射到内存，
 *   然后逐行解析，构建 BackupInfo 结构体。
 *
 * 参数：
 *   op - Options 结构体指针
 *
 * 返回值：
 *   成功时返回指向 BackupInfo 结构体的指针（调用者需通过 free_backup_info() 释放）
 *   失败时返回 NULL
 *
 * 处理流程：
 *   1. 检查备份目录和日志文件的权限（安全校验）
 *   2. 将日志文件 mmap 到内存
 *   3. 读取第一行作为版本字符串
 *   4. 读取第二行作为驱动描述
 *   5. 循环读取后续行，解析每个文件条目
 *      - 根据条目类型编号决定后续需要读取多少行额外数据
 *   6. 返回包含所有解析结果的 BackupInfo 结构体
 *
 * 安全设计：
 *   - 验证备份目录权限是否为 0700（防止被篡改）
 *   - 验证日志文件权限是否为 0600（防止被篡改）
 */

static BackupInfo *read_backup_log_file(Options *op)
{
    struct stat stat_buf;
    char *buf, *c, *line, *filename;
    int fd, num, length, line_num = 0;
    float percent;

    BackupLogEntry *e;
    BackupInfo *b = NULL;

    /* 检查备份目录的权限是否符合预期（安全校验） */

    if (stat(BACKUP_DIRECTORY, &stat_buf) == -1) {
        ui_error(op, "Unable to get properties of %s (%s).",
                 BACKUP_DIRECTORY, strerror(errno));
        return NULL;
    }

    /* PERM_MASK 用于屏蔽文件类型位，只保留权限位进行比较 */
    if ((stat_buf.st_mode & PERM_MASK) != BACKUP_DIRECTORY_PERMS) {
        ui_error(op, "The directory permissions of %s have been changed since"
                 "the directory was created!", BACKUP_DIRECTORY);
        return NULL;
    }

    /* 以只读方式打开备份日志文件 */
    if ((fd = open(BACKUP_LOG, O_RDONLY)) == -1) {
        ui_error(op, "Failure opening %s (%s).", BACKUP_LOG, strerror(errno));
        return NULL;
    }

    /* 获取日志文件的属性信息 */
    if (fstat(fd, &stat_buf) == -1) {
        ui_error(op, "Failure getting file properties for %s (%s).",
                 BACKUP_LOG, strerror(errno));
        goto pre_map_fail;
    }

    /* 检查日志文件的权限是否符合预期（安全校验） */
    if ((stat_buf.st_mode & PERM_MASK) != BACKUP_LOG_PERMS) {
        ui_error(op, "The file permissions of %s have been changed since "
                 "the file was written!", BACKUP_LOG);
        goto pre_map_fail;
    }

    /*
     * 将日志文件通过 mmap 映射到内存。
     * 使用 MAP_SHARED 以共享方式映射（虽然只读，不影响），
     * MAP_FILE 表示映射的是文件（而非匿名内存）。
     */

    length = stat_buf.st_size;

    buf = mmap(0, length, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
    if (!buf) {
        ui_error(op, "Unable to mmap file '%s' (%s).", BACKUP_LOG,
                 strerror(errno));
        return NULL;
    }

    /* 开始显示解析进度 */
    ui_status_begin(op, "Parsing log file:", "Parsing");

    /* 分配 BackupInfo 结构体 */
    b = nvalloc(sizeof(BackupInfo));

    /* 读取第一行：驱动版本字符串 */
    b->version = get_next_line(buf, &c, buf, length);
    if (!b->version || !c) goto parse_error;

    /* 更新解析进度 */
    percent = (float) (c - buf) / (float) stat_buf.st_size;
    ui_status_update(op, percent, NULL);

    /* 读取第二行：驱动描述信息 */
    b->description = get_next_line(c, &c, buf, length);
    if (!b->description || !c) goto parse_error;

    /* 初始化条目数组 */
    b->n = 0;
    b->e = NULL;
    line_num = 3;  /* 从第三行开始是文件条目 */

    /* 循环读取并解析所有文件条目 */
    while (1) {

        /* 更新解析进度 */
        percent = (float) (c - buf) / (float) stat_buf.st_size;
        ui_status_update(op, percent, NULL);

        /* 读取并解析下一行（条目的第一行） */

        line = get_next_line(c, &c, buf, length);
        if (!line) break;  /* 到达文件末尾 */

        /* 解析条目第一行，提取类型编号和文件名 */
        if (!parse_first_line(line, &num, &filename)) goto parse_error;
        line_num++;
        free(line);

        /* 动态增长 BackupLogEntry 数组 */

        b->n++;
        b->e = (BackupLogEntry *)
            nvrealloc(b->e, sizeof(BackupLogEntry) * b->n);

        /* 将新条目初始化为零 */
        memset(&b->e[b->n - 1], 0, sizeof(BackupLogEntry));

        /* 设置新条目的基本信息 */
        e = &b->e[b->n - 1];
        e->num = num;
        e->filename = filename;
        e->ok = TRUE;  /* 默认标记为有效，后续检查可能会改为 FALSE */

        /* 根据条目类型，继续读取额外的数据行 */
        switch(e->num) {

        case INSTALLED_FILE:
            /* 已安装文件：读取下一行的 CRC 校验值 */
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            if (!parse_crc(line, &e->crc)) goto parse_error;
            free(line);

            break;

        case INSTALLED_SYMLINK:
            /* 已安装符号链接：读取下一行的链接目标路径 */
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            e->target = line;  /* 注意：此处 line 的所有权转移给 e->target */

            break;

        case BACKED_UP_SYMLINK:
            /* 备份的符号链接：读取链接目标路径和属性信息 */

            /* 读取链接目标路径 */
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            e->target = line;

            /* 读取权限、UID、GID */
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            if (!parse_mode_uid_gid(line, &e->mode, &e->uid, &e->gid))
                goto parse_error;
            free(line);

            break;

        default:
            /*
             * 默认分支处理备份的普通文件（编号 >= BACKED_UP_FILE_NUM）。
             * 如果编号小于 BACKED_UP_FILE_NUM，则为非法条目。
             */
            if (num < BACKED_UP_FILE_NUM) goto parse_error;

            /* 读取 CRC、权限、UID、GID */
            line = get_next_line(c, &c, buf, length);
            if (line == NULL) goto parse_error;
            line_num++;

            if (!parse_crc_mode_uid_gid(line, &e->crc, &e->mode,
                                        &e->uid, &e->gid)) goto parse_error;
            free(line);

            break;
        }

        /* 如果 c 为 NULL，说明已到达文件末尾 */
        if (!c) break;
    }

    ui_status_end(op, "done.");

    /* 取消内存映射并关闭文件 */
    munmap(buf, stat_buf.st_size);
    close(fd);

    return b;

 parse_error:

    ui_status_end(op, "error.");

    munmap(buf, stat_buf.st_size);

    ui_error(op, "Error while parsing line %d of '%s'.", line_num, BACKUP_LOG);

    nvfree(b);

 pre_map_fail:

    close(fd);
    return NULL;

} /* read_backup_log_file() */



/*
 * free_backup_info() - 释放 BackupInfo 结构体及其关联的所有动态内存。
 *
 * 参数：
 *   b - 要释放的 BackupInfo 结构体指针（可以为 NULL，此时直接返回）
 *
 * 释放内容包括：
 *   - 版本字符串（b->version）
 *   - 描述字符串（b->description）
 *   - 每个日志条目中的文件名和目标路径
 *   - 日志条目数组本身
 *   - BackupInfo 结构体自身
 */

static void free_backup_info(BackupInfo *b)
{
    int i;

    if (!b) return;

    nvfree(b->version);
    nvfree(b->description);

    /* 释放每个条目中动态分配的字符串 */
    for (i = 0; i < b->n; i++) {
        nvfree(b->e[i].filename);
        nvfree(b->e[i].target);
    }

    /* 释放条目数组 */
    nvfree((char *) b->e);

    /* 释放 BackupInfo 结构体本身 */
    nvfree((char *) b);

} /* free_backup_info() */



/*
 * check_backup_log_entries() - 对每个备份日志条目执行基本的完整性检查。
 *
 * 功能：
 *   在执行卸载操作之前，验证日志中记录的每个条目是否仍然有效：
 *   - 已安装文件：检查文件是否仍存在，CRC 是否匹配
 *   - 已安装符号链接：检查链接是否仍存在，目标是否一致
 *   - 备份文件：检查备份副本是否仍存在，CRC 是否匹配
 *
 *   如果某个条目检查失败，将其 'ok' 字段设为 FALSE，
 *   卸载时将跳过该条目（不删除/不恢复）。
 *
 * 参数：
 *   op - Options 结构体指针
 *   b  - 包含所有日志条目的 BackupInfo 结构体
 *
 * 返回值：
 *   TRUE  - 所有条目都通过验证
 *   FALSE - 至少有一个条目未通过验证（安装可能已被篡改）
 */

static int check_backup_log_entries(Options *op, BackupInfo *b)
{
    BackupLogEntry *e;
    uint32 crc;
    char *tmpstr;
    int i, j, len, ret = TRUE;
    float percent;

    /* 开始显示验证进度 */
    ui_status_begin(op, "Validating previous installation:", "Validating");

    for (i = 0; i < b->n; i++) {

        percent = (float) i / (float) (b->n);

        e = &b->e[i];

        switch (e->num) {

        case INSTALLED_FILE:

            /*
             * 已安装文件：使用 check_installed_file() 验证文件
             * 是否仍存在、权限和 CRC 是否与日志记录一致。
             */

            e->ok = check_installed_file(op, e->filename, e->mode, e->crc,
                                         ui_log);
            ret = ret && e->ok;

            ui_status_update(op, percent, "%s", e->filename);

            break;

        case INSTALLED_SYMLINK:

            /*
             * 已安装符号链接：检查链接是否仍存在，
             * 以及链接目标是否与安装时记录的一致。
             */

            if (access(e->filename, F_OK) == -1) {
                /* 符号链接已不存在 */
                ui_log(op, "Unable to access previously installed "
                       "symlink '%s' (%s).", e->filename, strerror(errno));
                ret = e->ok = FALSE;
            } else {
                /* 获取当前符号链接的目标路径 */
                tmpstr = get_symlink_target(op, e->filename);
                if (!tmpstr) {
                    ret = e->ok = FALSE;
                } else {
                    /* 比较当前目标与日志记录的目标是否一致 */
                    if (strcmp(tmpstr, e->target) != 0) {
                        ui_log(op, "The previously installed symlink '%s' "
                               "has target '%s', but it was installed "
                               "with target '%s'.  %s will not be "
                               "uninstalled.",
                               e->filename, tmpstr, e->target, e->filename);
                        ret = e->ok = FALSE;

                        /*
                         * 如果已安装的符号链接目标已被更改，则不删除它。
                         * 同时，也不应恢复同名的备份符号链接，
                         * 因为恢复后会与当前状态冲突。
                         */

                        for (j = 0; j < b->n; j++) {
                            if ((b->e[j].num == BACKED_UP_SYMLINK) &&
                                (strcmp(b->e[j].filename, e->filename) == 0)) {
                                b->e[j].ok = FALSE;
                            }
                        }
                    }
                    free(tmpstr);
                }
            }
            ui_status_update(op, percent, "%s", e->filename);

            break;

        case BACKED_UP_SYMLINK:

            /* 备份的符号链接：无需额外验证 */

            break;

        default:

            /*
             * 备份的普通文件：检查备份副本是否仍存在于备份目录中，
             * 以及其 CRC 是否与备份时记录的一致。
             */

            /* 构建备份文件路径：/var/lib/nvidia/<编号> */
            len = strlen(BACKUP_DIRECTORY) + 64;
            tmpstr = nvalloc(len + 1);
            snprintf(tmpstr, len, "%s/%d", BACKUP_DIRECTORY, e->num);
            if (access(tmpstr, F_OK) == -1) {
                /* 备份文件已不存在 */
                ui_log(op, "Unable to access backed up file '%s' "
                       "(saved as '%s') (%s).",
                       e->filename, tmpstr, strerror(errno));
                ret = e->ok = FALSE;
            } else {
                /* 验证备份文件的 CRC 校验值 */
                crc = compute_crc(op, tmpstr);

                if (crc != e->crc) {
                    ui_log(op, "Backed up file '%s' (saved as '%s) has "
                           "different checksum (%" PRIu32 ") than when it was "
                           "backed up (%" PRIu32").  %s will not be restored.",
                           e->filename, tmpstr, crc, e->crc, e->filename);
                    ret = e->ok = FALSE;
                }
            }
            ui_status_update(op, percent, "%s", tmpstr);
            free(tmpstr);
            break;
        }
    }

    ui_status_end(op, "done.");

    return (ret);

} /* check_backup_log_entries() */



/*
 * get_installed_driver_version_and_descr() - 获取当前已安装驱动的版本号和描述信息。
 *
 * 功能：
 *   通过读取备份日志文件（BACKUP_LOG）的前两行，
 *   提取已安装驱动的版本号和描述字符串。
 *
 * 参数：
 *   op       - Options 结构体指针
 *   pVersion - [输出] 已安装驱动的版本号字符串（调用者需释放）
 *   pDescr   - [输出] 已安装驱动的描述字符串（调用者需释放）
 *
 * 返回值：
 *   TRUE  - 成功获取版本和描述信息（说明有驱动已安装）
 *   FALSE - 获取失败（说明没有驱动已安装，或日志文件不存在/损坏）
 *
 * 注意事项（原作者的 XXX 标注）：
 *   - 当前仅通过读取 BACKUP_LOG 判断是否有驱动已安装，这可能不够充分。
 *   - 无法检测在使用新版安装器之前安装的驱动。
 *   - 可以考虑通过扫描系统中的已安装文件并提取 nvid 来检测。
 *   - 可能还需要检查 BACKUP_LOG 的文件权限（安全考虑）。
 */

int get_installed_driver_version_and_descr(Options *op,
                                           char **pVersion, char **pDescr)
{
    struct stat stat_buf;
    char *c, *version = NULL, *descr = NULL, *buf = NULL;
    char *version_line = NULL;
    int length, fd = -1;
    int ret = FALSE;

    /* 尝试打开备份日志文件；如果不存在则说明没有已安装的驱动 */
    if ((fd = open(BACKUP_LOG, O_RDONLY)) == -1) goto done;

    if (fstat(fd, &stat_buf) == -1) goto done;

    /* 将日志文件映射到内存 */

    length = stat_buf.st_size;

    buf = mmap(0, length, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
    if (!buf) goto done;

    /* 读取第一行（向后兼容格式的版本字符串） */
    version_line = get_next_line(buf, &c, buf, length);
    if (!version_line) goto done;

    /*
     * 从向后兼容格式中提取实际版本号。
     * 例如从 "1.0-105917 (105.9.17)" 中提取 "105.9.17"。
     */
    version = extract_version_string(version_line);

    if (!version) goto done;

    /* 读取第二行（驱动描述信息） */
    descr = get_next_line(c, NULL, buf, length);

    if (!descr) goto done;

    /* 将结果复制到输出参数中 */
    *pVersion = strdup(version);
    *pDescr = strdup(descr);

    ret = TRUE;

 done:
    /* 清理所有临时资源 */
    nvfree(version_line);
    nvfree(version);
    nvfree(descr);

    if (buf) munmap(buf, stat_buf.st_size);
    if (fd != -1) close(fd);

    return ret;

} /* get_installed_driver_version_and_descr() */



/*
 * check_for_existing_driver() - 检查系统中是否已安装 NVIDIA 驱动。
 *
 * 功能：
 *   在安装新驱动之前调用，执行以下检查：
 *   1. 检查是否有通过 RPM 等包管理器安装的冲突驱动
 *   2. 检查备份日志文件中是否记录了已安装的驱动
 *   3. 如果是仅安装内核模块模式（--kernel-modules-only），
 *      验证已安装驱动版本是否与待安装模块版本匹配
 *   4. 如果有已安装驱动，询问用户是否要继续安装（会先卸载旧驱动）
 *
 * 参数：
 *   op - Options 结构体指针
 *   p  - Package 结构体指针，包含待安装驱动包的信息
 *
 * 返回值：
 *   TRUE  - 可以继续安装（没有旧驱动，或用户同意卸载旧驱动）
 *   FALSE - 用户选择中止安装
 *
 * 注意事项（原作者的 XXX 标注）：
 *   可以比较新旧版本号，在降级安装时发出警告，
 *   但原作者认为降级和升级没有本质区别，所以未实现。
 */

int check_for_existing_driver(Options *op, Package *p)
{
    char *descr = NULL;
    char *version = NULL;
    int ret = FALSE;
    int localRet;

    /* 检查是否有通过 RPM 等包管理器安装的冲突驱动 */
    if (!check_for_existing_rpms(op)) goto done;

    /* 从备份日志获取已安装驱动的版本和描述 */
    localRet = get_installed_driver_version_and_descr(op, &version, &descr);

    /*
     * 仅安装内核模块模式（--kernel-modules-only）的特殊处理：
     * 此模式要求系统中已有一个相同版本的驱动安装。
     */
    if (op->kernel_modules_only) {
        if (!localRet) {
            /* 没有已安装的驱动，但 --kernel-modules-only 需要已有驱动 */
            ui_error(op, "No NVIDIA driver is currently installed; the "
                     "'--kernel-modules-only' option can only be used "
                     "to install the NVIDIA kernel modules on top of an "
                     "existing driver installation.");
            goto done;
        } else {
            /* 检查版本是否匹配 */
            if (strcmp(p->version, version) != 0) {
                ui_error(op, "The '--kernel-modules-only' option can only be "
                         "used to install kernel modules on top of an "
                         "existing driver installation of the same driver "
                         "version.  The existing driver installation is "
                         "%s, but the kernel modules are %s.\n",
                         version, p->version);
                goto done;
            } else {
                /* 版本匹配，可以继续安装内核模块 */
                ret = TRUE;
                goto done;
            }
        }
    }

    /* 没有已安装的驱动 -- 可以安全地继续安装 */

    if (!localRet) {
        ret = TRUE;
        goto done;
    }

    /*
     * XXX 可以在此处比较版本号，在降级时发出警告。
     * 但原作者认为降级和升级没有本质区别，所以未实现。
     */

    /*
     * 向用户显示确认对话框：
     * 提示已有旧版驱动，安装新版本将会先卸载旧版本。
     * 用户可以选择"继续"或"中止"。
     */
    if (ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                           NUM_CONTINUE_ABORT_CHOICES,
                           CONTINUE_CHOICE, /* 默认选项：继续 */
                           "There appears to already be a driver installed on "
                           "your system (version: %s).  As part of installing "
                           "this driver (version: %s), the existing driver will "
                           "be uninstalled.  Are you sure you want to continue?",
                           version, p->version) == ABORT_CHOICE) {

        ui_log(op, "Installation aborted.");
        goto done;
    }

    ret = TRUE;

 done:

    nvfree(descr);
    nvfree(version);

    return ret;

} /* check_for_existing_driver() */



/*
 * uninstall_existing_driver() - 检查并卸载已安装的驱动。
 *
 * 功能：
 *   1. 获取已安装驱动的版本和描述信息
 *   2. 如果没有已安装驱动，提示用户（交互模式下）
 *   3. 如果是交互式卸载模式，提供恢复 X 配置文件的选项
 *   4. 调用 do_uninstall() 执行实际的卸载操作
 *   5. 报告卸载结果
 *
 * 参数：
 *   op          - Options 结构体指针
 *   interactive - 是否为交互模式（TRUE = 显示提示/确认对话框）
 *   skip_depmod  - 是否跳过运行 depmod（当即将安装新驱动时可跳过）
 *
 * 返回值：
 *   始终返回 TRUE。
 *   设计上此函数不应导致安装过程中止，即使卸载失败也是如此。
 */

int uninstall_existing_driver(Options *op, const int interactive,
                              const int skip_depmod)
{
    int ret;
    char *descr = NULL;
    char *version = NULL;

    /* 获取已安装驱动的版本和描述 */
    ret = get_installed_driver_version_and_descr(op, &version, &descr);
    if (!ret) {
        /* 没有已安装的驱动 */
        if (interactive) {
            ui_message(op, "There is no NVIDIA driver currently installed.");
        }
        return TRUE;
    }

    /*
     * 在交互式卸载模式下，提示用户是否要恢复原始的 X 配置文件。
     * 如果用户之前使用 nvidia-xconfig 修改了 X 配置，
     * 卸载驱动后应该恢复原始配置，否则 X Server 可能无法启动。
     */
    if (interactive && op->uninstall) {
        const char *msg = "If you plan to no longer use the NVIDIA driver, you "
                        "should make sure that no X screens are configured to "
                        "use the NVIDIA X driver in your X configuration file. "
                        "If you used nvidia-xconfig to configure X, it may have "
                        "created a backup of your original configuration. Would "
                        "you like to run `nvidia-xconfig --restore-original-"
                        "backup` to attempt restoration of the original X "
                        "configuration file?";
        run_nvidia_xconfig(op, TRUE, msg, FALSE);
    }

    /* 执行实际的卸载操作 */
    ret = do_uninstall(op, version, skip_depmod);

    /* 报告卸载结果 */
    if (ret) {
        if (interactive) {
            ui_message(op, "Uninstallation of existing driver: %s (%s) "
                       "is complete.", descr, version);
        } else {
            ui_log(op, "Uninstallation of existing driver: %s (%s) "
                   "is complete.", descr, version);
        }
    } else {
        ui_error(op, "Uninstallation failed.");
    }

    nvfree(descr);
    nvfree(version);

    return TRUE;

} /* uninstall_existing_driver() */

/*
 * check_skip_depmod_support() - 检查 nvidia-uninstall 是否支持 --skip-depmod 选项。
 *
 * 功能：
 *   通过检查卸载程序的帮助文本来判断它是否支持 --skip-depmod 选项。
 *   具体方法是运行 `nvidia-uninstall -A`（显示帮助文本），
 *   然后用 grep 搜索是否包含 "--skip-depmod" 字符串。
 *
 * 参数：
 *   op          - Options 结构体指针
 *   uninstaller - nvidia-uninstall 可执行文件的完整路径
 *
 * 返回值：
 *   TRUE  - 卸载程序支持 --skip-depmod 选项
 *   FALSE - 不支持（旧版本的卸载程序）
 */
static int check_skip_depmod_support(Options *op, const char *uninstaller)
{

    /*
     * run_command() 在成功时返回 0，所以这里取反。
     * 命令管道：nvidia-uninstall -A | grep -q '^ \+--skip-depmod$'
     * -A 选项显示高级帮助，grep -q 静默搜索指定选项。
     */
    return !run_command(op, NULL, FALSE, NULL, FALSE,
                        uninstaller, " -A | ", op->utils[GREP],
                        " -q '^ \\+--skip-depmod$'", NULL);
}


/*
 * run_existing_uninstaller() - 尝试运行系统中已有的 nvidia-uninstall 程序。
 *
 * 功能：
 *   1. 在系统 PATH 中搜索 nvidia-uninstall 可执行文件
 *   2. 如果找到，以非交互模式运行它来卸载旧驱动
 *   3. 如果 nvidia-uninstall 不存在或运行失败，
 *      则回退到使用备份日志文件进行卸载
 *
 * 参数：
 *   op - Options 结构体指针
 *
 * 返回值：
 *   TRUE  - 卸载成功
 *   FALSE - 卸载失败
 *
 * 设计说明：
 *   此函数在安装过程中被调用。如果即将安装新的内核模块并会运行 depmod，
 *   则卸载时可以跳过 depmod（避免重复运行），前提是旧版卸载程序也支持此选项。
 */
int run_existing_uninstaller(Options *op)
{
    /* 在系统 PATH 中搜索 nvidia-uninstall 可执行文件 */
    char *uninstaller = find_system_util("nvidia-uninstall");

    /*
     * 此函数作为安装过程的一部分运行。如果我们即将安装内核模块
     * 并在之后运行 depmod，那么在卸载过程中就不需要运行 depmod。
     * op->no_kernel_modules 为 TRUE 表示不安装内核模块，
     * 取反后表示"要安装内核模块，可以跳过卸载时的 depmod"。
     */
    int skip_depmod = !op->no_kernel_modules;

    if (uninstaller) {
        char *uninstall_log_dir, *uninstall_log_file, *uninstall_log_path;
        char *data = NULL;
        int ret;

        /*
         * 进一步检查旧版卸载程序是否支持 --skip-depmod 选项。
         * 只有两者都满足（要安装新模块 && 旧卸载程序支持）才跳过 depmod。
         */
        skip_depmod = skip_depmod && check_skip_depmod_support(op, uninstaller);

        /*
         * 设置卸载日志文件路径：
         * 使用 DEFAULT_UNINSTALL_LOG_FILE_NAME 作为文件名，
         * 存放在与 op->log_file_name 相同的目录下（可能是默认位置或自定义位置）。
         */
        uninstall_log_dir = nv_dirname(op->log_file_name);
        uninstall_log_file = nv_basename(DEFAULT_UNINSTALL_LOG_FILE_NAME);
        uninstall_log_path = nvdircat(uninstall_log_dir, uninstall_log_file,
                                      NULL);
        nvfree(uninstall_log_dir);
        nvfree(uninstall_log_file);

        /*
         * 以非交互模式（-s）运行卸载程序，并显式指定日志文件路径。
         * 显式指定日志路径是因为旧版安装器可能不会自动记录到此位置。
         */
        ui_status_begin(op, "Uninstalling the previous installation", "");
        ui_indeterminate_begin(op, "Running `%s...`", uninstaller);
        ret = run_command(op, &data, FALSE, NULL, TRUE,
                          uninstaller, " -s --log-file-name=",
                          uninstall_log_path,
                          skip_depmod ? " --skip-depmod" : NULL,
                          NULL);
        ui_indeterminate_end(op);

        /*
         * 如果 nvidia-uninstall 成功执行，直接返回。
         * 如果失败，则回退到通过备份日志文件进行卸载。
         */
        if (ret != 0) {
            ui_status_end(op, "failed.");
            ui_log(op, "%s failed; see %s for more details.", uninstaller,
                   uninstall_log_path);
            if (data && strlen(data)) {
                ui_log(op, "The output from %s was:\n%s", uninstaller, data);
            }
        }

        nvfree(data);
        nvfree(uninstaller);
        nvfree(uninstall_log_path);

        if (ret == 0) {
            ui_status_end(op, "done.");
            return TRUE;
        }
    }

    /*
     * 如果 nvidia-uninstall 不存在或执行失败，
     * 回退到通过备份日志文件进行卸载（非交互模式）。
     */
    return uninstall_existing_driver(op, FALSE /* interactive */,
                                     skip_depmod);
}



/*
 * report_driver_information() - 报告当前已安装驱动的基本信息。
 *
 * 功能：
 *   读取已安装驱动的版本和描述信息，并显示给用户。
 *   通常在用户执行 --driver-info 选项时调用。
 *
 * 参数：
 *   op - Options 结构体指针
 *
 * 返回值：
 *   TRUE  - 成功报告了驱动信息
 *   FALSE - 没有已安装的驱动
 */

int report_driver_information(Options *op)
{
    char *descr, *version;
    int ret;

    /* 获取已安装驱动的版本和描述 */
    ret = get_installed_driver_version_and_descr(op, &version, &descr);
    if (!ret) {
        ui_message(op, "There is no NVIDIA driver currently installed.");
        return FALSE;
    }

    /* 向用户显示驱动信息 */
    ui_message(op, "The currently installed driver is: '%s' "
               "(version: %s).", descr, version);

    nvfree(descr);
    nvfree(version);

    return TRUE;

} /* report_driver_information() */



/*
 * test_installed_files() - 测试所有已安装文件的完整性。
 *
 * 功能：
 *   读取备份日志文件，对每个已安装的文件和符号链接执行完整性检查：
 *   - 检查文件是否仍然存在
 *   - 检查文件的 CRC 校验值是否与安装时一致
 *   - 检查符号链接的目标是否与创建时一致
 *   - 检查备份文件是否仍然完好
 *
 *   通常在用户执行 --sanity 选项时调用。
 *
 * 参数：
 *   op - Options 结构体指针
 *
 * 返回值：
 *   TRUE  - 所有文件通过完整性检查
 *   FALSE - 至少有一个文件未通过检查，或无法读取备份日志
 */

int test_installed_files(Options *op)
{
    BackupInfo *b;
    int ret;

    /* 读取并解析备份日志文件 */
    b = read_backup_log_file(op);
    if (!b) return FALSE;

    /* 对每个条目执行完整性检查 */
    ret = sanity_check_backup_log_entries(op, b);

    /* 释放 BackupInfo 相关资源 */

    free_backup_info(b);

    return ret;

} /* test_installed_files() */



/*
 * find_installed_file() - 在备份日志中查找指定的已安装文件。
 *
 * 功能：
 *   扫描备份日志文件中的所有条目，查找是否有指定文件名被记录为已安装文件。
 *   仅搜索 INSTALLED_FILE 类型的条目（不搜索符号链接或备份文件）。
 *
 * 参数：
 *   op       - Options 结构体指针
 *   filename - 要查找的文件的完整路径
 *
 * 返回值：
 *   TRUE  - 找到了该文件（它是由 nvidia-installer 安装的）
 *   FALSE - 未找到，或无法读取备份日志
 *
 * 注意事项（原作者的 XXX 标注）：
 *   可能应该比较 inode 而不是文件名（以处理硬链接等情况），
 *   但目前仅使用文件名比较。
 */

int find_installed_file(Options *op, char *filename)
{
    BackupInfo *b;
    BackupLogEntry *e;
    int i, ret = FALSE;

    /* 读取备份日志文件 */
    if ((b = read_backup_log_file(op)) == NULL) return FALSE;

    /* 遍历所有条目，查找匹配的已安装文件 */
    for (i = 0; i < b->n; i++) {
        e = &b->e[i];
        if ((e->num == INSTALLED_FILE) && strcmp(filename, e->filename) == 0) {

            /* XXX 可能应该比较 inode 而非文件名（原作者的待办事项） */

            ret =  TRUE;
            break;
        }
    }

    free_backup_info(b);

    return ret;

} /* find_installed_file() */



/*
 * sanity_check_backup_log_entries() - 完整性检查备份日志中的所有条目。
 *
 * 功能：
 *   与 check_backup_log_entries() 功能类似，但错误消息有所不同，
 *   因为此函数用于 --sanity 检查路径（检查安装的完整性），
 *   而不是用于卸载前的验证。
 *
 *   具体检查内容：
 *   - INSTALLED_FILE: 检查文件是否仍存在，CRC 是否匹配
 *   - INSTALLED_SYMLINK: 检查链接是否仍存在，目标是否一致
 *   - BACKED_UP_SYMLINK: 无需检查
 *   - 备份文件（default）: 检查备份副本是否存在，CRC 是否匹配
 *
 * 参数：
 *   op - Options 结构体指针
 *   b  - 包含所有日志条目的 BackupInfo 结构体
 *
 * 返回值：
 *   TRUE  - 所有条目通过检查
 *   FALSE - 至少有一个条目未通过检查
 *
 * 与 check_backup_log_entries() 的区别：
 *   - 使用 ui_error() 而非 ui_log() 报告问题（因为这是用户请求的检查）
 *   - 不修改条目的 ok 字段（不影响后续卸载行为）
 *   - 不处理符号链接目标变更的级联效果
 */

static int sanity_check_backup_log_entries(Options *op, BackupInfo *b)
{
    BackupLogEntry *e;
    uint32 crc;
    char *tmpstr;
    int i, len, ret = TRUE;
    float percent;

    /* 开始显示验证进度 */
    ui_status_begin(op, "Validating installation:", "Validating");

    for (i = 0; i < b->n; i++) {

        e = &b->e[i];

        switch (e->num) {

        case INSTALLED_FILE:

            /* 检查已安装文件：是否存在、CRC 是否匹配 */

            if (access(e->filename, F_OK) == -1) {
                ui_error(op, "The installed file '%s' no longer exists.",
                         e->filename);
                ret = FALSE;
            } else {
                /* 计算文件当前的 CRC 并与日志记录比较 */
                crc = compute_crc(op, e->filename);

                if (crc != e->crc) {
                    ui_error(op, "The installed file '%s' has a different "
                             "checksum (%" PRIu32 ") than when it was "
                             "installed (%" PRIu32 ").", e->filename, crc,
                             e->crc);
                    ret = FALSE;
                }
            }
            break;

        case INSTALLED_SYMLINK:

            /* 检查已安装符号链接：是否存在、目标是否一致 */

            if (access(e->filename, F_OK) == -1) {
                ui_error(op, "The installed symbolic link '%s' no "
                         "longer exists.", e->filename);
                ret = FALSE;
            } else {
                /* 获取当前链接目标并与日志记录比较 */
                tmpstr = get_symlink_target(op, e->filename);
                if (!tmpstr) {
                    ret = FALSE;
                } else {
                    if (strcmp(tmpstr, e->target) != 0) {
                        ui_error(op, "The installed symbolic link '%s' "
                                 "has target '%s', but it was installed "
                                 "with target '%s'.",
                                 e->filename, tmpstr, e->target);
                        ret = FALSE;
                    }
                    free(tmpstr);
                }
            }
            break;

        case BACKED_UP_SYMLINK:

            /* 备份的符号链接：无需检查 */

            break;

        default:

            /*
             * 备份的普通文件：检查备份副本是否仍存在，CRC 是否匹配。
             */

            /* 构建备份文件路径 */
            len = strlen(BACKUP_DIRECTORY) + 64;
            tmpstr = nvalloc(len + 1);
            snprintf(tmpstr, len, "%s/%d", BACKUP_DIRECTORY, e->num);
            if (access(tmpstr, F_OK) == -1) {
                ui_error(op, "The backed up file '%s' (saved as '%s') "
                         "no longer exists.", e->filename, tmpstr);
                ret = FALSE;
            } else {
                crc = compute_crc(op, tmpstr);

                if (crc != e->crc) {
                    ui_error(op, "Backed up file '%s' (saved as '%s) has a "
                             "different checksum (%" PRIu32 ") than when it "
                             "was backed up (%" PRIu32 ").", e->filename,
                             tmpstr, crc, e->crc);
                    ret = FALSE;
                }
            }
            free(tmpstr);
            break;
        }

        /* 更新进度条 */
        percent = (float) i / (float) (b->n);
        ui_status_update(op, percent, "%s", e->filename);
    }

    ui_status_end(op, "done.");

    return ret;

} /* sanity_check_backup_log_entries */



/*
 * create_backwards_compatible_version_string() - 创建向后兼容的版本字符串。
 *
 * 功能：
 *   将新格式的版本字符串（如 "105.9.17"）转换为兼容旧版 nvidia-installer
 *   的格式。旧版安装器假定版本号格式为 X.Y-ZZZZ（如 "1.0-1234"）。
 *
 * 兼容策略：
 *   幸运的是，旧版安装器在解析版本时不在意 'Z' 部分的位数是否正好为 4。
 *   它只会寻找前 4 位数字。因此策略是：
 *   1. 将新版本号中的所有句号去掉，得到纯数字串（如 "105917"）
 *   2. 以旧格式输出："1.0-<纯数字> (<原始版本号>)"
 *
 * 例如：
 *   输入: "105.9.17"
 *   输出: "1.0-105917 (105.9.17)"
 *
 *   这样旧版安装器至少能解析这个字符串（虽然可能不完全理解），
 *   而新版安装器可以从括号中提取出实际的版本号。
 *
 * 参数：
 *   str - 新格式的版本字符串（如 "105.9.17"）
 *
 * 返回值：
 *   动态分配的向后兼容版本字符串（调用者需释放）
 */

static char *create_backwards_compatible_version_string(const char *str)
{
    char *version, *scratch, *s, *t;
    int len;

    /*
     * 复制输入字符串，然后在副本中只保留数字字符。
     * 例如："105.9.17" --> "105917"
     */

    scratch = nvstrdup(str);

    for (s = t = scratch; *t; t++) {
        if (isdigit(*t)) {
            *s++ = *t;
        }
    }

    *s = '\0';

    /* 分配结果字符串的空间（足够容纳旧格式和新格式） */

    len = strlen(str);
    version = nvalloc((len * 2) + 16);

    /* 拼接最终的向后兼容版本字符串 */
    sprintf(version, "1.0-%s (%s)", scratch, str);

    nvfree(scratch);

    return version;

} /* create_backwards_compatible_version_string() */
