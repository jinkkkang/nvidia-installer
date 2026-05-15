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
 * files.c - this source file contains routines for manipulating
 * files and directories for the nv-instaler.
 *
 * 【文件说明】nvidia-installer 文件操作子系统实现。
 *
 * 本文件是 nvidia-installer 的核心模块之一，实现了安装过程中所有与文件系统
 * 相关的操作。主要功能包括：
 *
 * 1. 安装路径计算：
 *    - set_destinations(): 根据文件类型和安装选项，计算每个文件的安装目标路径
 *    - get_prefixes(): 在专家模式下允许用户自定义各类安装前缀
 *    - get_default_prefixes_and_paths(): 设置所有默认安装路径（基于架构/发行版）
 *
 * 2. 文件安装操作：
 *    - copy_file(): 使用 mmap/memcpy 进行文件复制
 *    - install_file(): 安装单个文件（自动创建目标目录）
 *    - install_symlink(): 创建符号链接
 *    - write_temp_file(): 将数据写入临时文件
 *
 * 3. 目录操作：
 *    - remove_directory(): 递归删除目录（类似 rm -rf）
 *    - touch_directory(): 递归更新目录中所有文件的时间戳
 *    - mkdir_recursive(): 递归创建目录（类似 mkdir -p）
 *    - copy_directory_contents(): 递归复制目录内容
 *
 * 4. 包条目过滤（用于根据安装选项移除不需要的文件）：
 *    - remove_non_kernel_module_files_from_package(): 仅保留内核模块文件
 *    - remove_opengl_files_from_package(): 移除 OpenGL 文件
 *    - remove_wine_files_from_package(): 移除 Wine 兼容文件
 *    - remove_systemd_files_from_package(): 移除 systemd 服务文件
 *
 * 5. 模板处理：
 *    - process_template_file(): 通用模板文件处理（token 替换）
 *    - process_dot_desktop_files(): 处理 .desktop 桌面快捷方式文件
 *    - process_dkms_conf(): 处理 DKMS 配置文件模板
 *
 * 6. 其他功能：
 *    - set_security_context(): 设置 SELinux 安全上下文
 *    - pack_precompiled_files(): 打包预编译内核接口
 *    - check_libglvnd_files(): 检查并处理 libglvnd 库的安装
 *    - get_compat32_path(): 检测 32 位兼容库的安装路径
 *    - nvrename(): 跨文件系统的文件重命名
 *    - check_for_existing_rpms(): 检查并移除冲突的旧版 RPM 包
 */

/* ===== 系统头文件 ===== */
#include <sys/types.h>      /* 基本系统数据类型（mode_t, pid_t 等） */
#include <sys/stat.h>       /* 文件状态信息（stat 结构体） */
#include <unistd.h>         /* POSIX 操作系统 API（readlink, unlink 等） */
#include <dirent.h>         /* 目录操作（opendir, readdir 等） */
#include <string.h>         /* 字符串操作函数 */
#include <fcntl.h>          /* 文件控制选项（O_RDONLY, O_CREAT 等） */
#include <errno.h>          /* 错误号定义 */
#include <sys/mman.h>       /* 内存映射（mmap, munmap） */
#include <sys/utsname.h>    /* 系统信息（uname） */
#include <stdlib.h>         /* 标准库函数 */
#include <limits.h>         /* 系统限制常量（PATH_MAX, LONG_MAX 等） */
#include <libgen.h>         /* 路径名操作（dirname, basename） */
#include <utime.h>          /* 文件时间戳修改 */
#include <time.h>           /* 时间操作 */
#include <sys/wait.h>       /* 进程等待（WIFEXITED, WEXITSTATUS） */

/* ===== 项目内部头文件 ===== */
#include "nvidia-installer.h"  /* 全局数据结构定义（Options, Package 等） */
#include "user-interface.h"    /* 用户界面交互函数（ui_error, ui_warn 等） */
#include "files.h"             /* 本模块的函数声明 */
#include "misc.h"              /* 通用工具函数（nvalloc, nvstrcat 等） */
#include "precompiled.h"       /* 预编译内核接口相关定义 */
#include "backup.h"            /* 备份/日志功能 */
#include "kernel.h"            /* 内核模块相关功能 */


/* 前向声明：获取 X 库和模块的安装路径 */
static void  get_x_library_and_module_paths(Options *op);


/*
 * remove_directory() - 递归删除目录及其所有内容（等价于 `rm -rf`）。
 *
 * 参数：
 *   op     - 全局选项结构体，用于错误消息输出
 *   victim - 要删除的目录路径
 *
 * 处理流程：
 *   1. 使用 lstat() 检查目标是否存在，并确认它是一个目录
 *   2. 打开目录，遍历其中的每个条目
 *   3. 对于子目录，递归调用自身进行删除
 *   4. 对于普通文件（包括符号链接等），使用 unlink() 删除
 *   5. 删除所有内容后，使用 rmdir() 删除空目录本身
 *
 * 注意：使用 lstat() 而非 stat()，这样可以正确处理符号链接
 * （不会跟随链接到其目标，而是操作链接本身）。
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int remove_directory(Options *op, const char *victim)
{
    struct stat stat_buf;  /* 文件状态信息缓冲区 */
    DIR *dir;              /* 目录流指针 */
    struct dirent *ent;    /* 目录条目指针 */
    int success = TRUE;    /* 操作结果标志 */

    /* 第一步：使用 lstat 获取目标的文件信息（不跟随符号链接） */
    if (lstat(victim, &stat_buf) == -1) {
        ui_error(op, "failure to open '%s'", victim);
        return FALSE;
    }

    /* 第二步：确认目标是一个目录 */
    if (S_ISDIR(stat_buf.st_mode) == 0) {
        ui_error(op, "%s is not a directory", victim);
        return FALSE;
    }

    /* 第三步：打开目录进行遍历 */
    if ((dir = opendir(victim)) == NULL) {
        ui_error(op, "Failure reading directory %s", victim);
        return FALSE;
    }

    /* 第四步：遍历目录中的每个条目，逐个删除 */
    while (success && (ent = readdir(dir)) != NULL) {
        char *filename;
        int len;

        /* 跳过当前目录 "." 和父目录 ".." 条目 */
        if (((strcmp(ent->d_name, ".")) == 0) ||
            ((strcmp(ent->d_name, "..")) == 0)) continue;

        /* 构造子条目的完整路径 */
        len = strlen(victim) + strlen(ent->d_name) + 2;
        filename = (char *) nvalloc(len);
        snprintf(filename, len, "%s/%s", victim, ent->d_name);

        if (lstat(filename, &stat_buf) == -1) {
            ui_error(op, "failure to open '%s'", filename);
            success = FALSE;
        } else {
            if (S_ISDIR(stat_buf.st_mode)) {
                /* 子目录：递归删除 */
                success = remove_directory(op, filename);
            } else {
                /* 普通文件或符号链接：直接 unlink 删除 */
                if (unlink(filename) != 0) {
                    ui_error(op, "Failure removing file %s (%s)",
                             filename, strerror(errno));
                    success = FALSE;
                }
            }
        }

        free(filename);
    }

    closedir(dir);

    /* 第五步：所有内容已删除后，删除空目录本身 */
    if (rmdir(victim) != 0) {
        ui_error(op, "Failure removing directory %s (%s)",
                 victim, strerror(errno));
        return FALSE;
    }

    return success;
}



/*
 * touch_directory() - 递归地更新指定目录中所有文件和子目录的访问时间和
 * 修改时间为当前时间（类似于 `touch` 命令的递归版本）。
 *
 * 参数：
 *   op     - 全局选项结构体，用于错误消息输出
 *   victim - 要更新时间戳的目录路径
 *
 * 处理流程：
 *   1. 验证路径存在且为目录
 *   2. 获取当前系统时间作为新的时间戳
 *   3. 遍历目录中的每个条目：
 *      a. 如果是子目录，递归调用自身
 *      b. 使用 utime() 更新每个条目的访问/修改时间
 *   4. 任何一个条目处理失败都会立即终止并返回错误
 *
 * 用途：在安装完成后更新文件时间戳，确保文件系统缓存一致性。
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int touch_directory(Options *op, const char *victim)
{
    struct stat stat_buf;       /* 文件状态信息缓冲区 */
    DIR *dir;                   /* 目录流指针 */
    struct dirent *ent;         /* 目录条目指针 */
    struct utimbuf time_buf;    /* 时间戳缓冲区，用于 utime() */
    int success = FALSE;        /* 操作结果标志，默认为失败 */

    /* 检查目标路径是否存在 */
    if (lstat(victim, &stat_buf) == -1) {
        ui_error(op, "failure to open '%s'", victim);
        return FALSE;
    }

    /* 确认目标是一个目录 */
    if (S_ISDIR(stat_buf.st_mode) == 0) {
        ui_error(op, "%s is not a directory", victim);
        return FALSE;
    }

    /* 打开目录进行遍历 */
    if ((dir = opendir(victim)) == NULL) {
        ui_error(op, "Failure reading directory %s", victim);
        return FALSE;
    }

    /* 获取当前系统时间，作为所有文件的新时间戳 */

    time_buf.actime = time(NULL);       /* 访问时间 */
    time_buf.modtime = time_buf.actime; /* 修改时间（与访问时间相同） */

    /* 遍历目录中的每个条目 */

    while ((ent = readdir(dir)) != NULL) {
        char *filename;
        int entry_failed = FALSE;

        /* 跳过 "." 和 ".." */
        if (((strcmp(ent->d_name, ".")) == 0) ||
            ((strcmp(ent->d_name, "..")) == 0)) continue;

        /* 构造子条目的完整路径 */
        filename = nvstrcat(victim, "/", ent->d_name, NULL);

        /* 获取文件类型信息 */

        if (lstat(filename, &stat_buf) == -1) {
            ui_error(op, "failure to open '%s'", filename);
            entry_failed = TRUE;
            goto entry_done;
        }

        /* 如果是子目录，递归处理 */

        if (S_ISDIR(stat_buf.st_mode)) {
            if (!touch_directory(op, filename)) {
                entry_failed = TRUE;
                goto entry_done;
            }
        }

        /* 设置文件的访问时间和修改时间为当前时间 */

        if (utime(filename, &time_buf) != 0) {
            ui_error(op, "Error setting modification time for %s", filename);
            entry_failed = TRUE;
            goto entry_done;
        }

 entry_done:
        nvfree(filename);
        if (entry_failed) {
            /* 有任何条目处理失败，立即终止遍历 */
            goto done;
        }
    }

    success = TRUE;

 done:

    if (closedir(dir) != 0) {
        ui_error(op, "Error while closing directory %s.", victim);
        success = FALSE;
    }

    return success;
}


/*
 * copy_file() - 使用 mmap 和 memcpy 将源文件复制到目标文件。
 *
 * 参数：
 *   op      - 全局选项结构体，用于错误消息输出
 *   srcfile - 源文件路径
 *   dstfile - 目标文件路径
 *   mode    - 目标文件的权限模式（如 0755）
 *
 * 处理流程：
 *   1. 以只读方式打开源文件
 *   2. 如果目标文件已存在，先 unlink 删除（避免覆盖正在被其他程序使用的文件）
 *   3. 创建新的目标文件
 *   4. 获取源文件大小，如果为 0 则直接返回成功
 *   5. 将目标文件大小设置为与源文件相同（通过 lseek + write 扩展文件）
 *   6. 使用 mmap 将源文件和目标文件都映射到内存
 *   7. 使用 memcpy 一次性复制全部数据
 *   8. 解除内存映射
 *   9. 使用 fchmod 显式设置权限（因为创建时可能受 umask 影响）
 *
 * 设计参考：Richard Stevens 的《UNIX 环境高级编程》12.9 节中的 mmap 复制示例。
 *
 * 注意：先删除目标文件再创建新文件的做法是为了确保不会覆盖正在被其他程序
 * 使用的文件内容（inode 层面的安全保护）。
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int copy_file(Options *op, const char *srcfile,
              const char *dstfile, mode_t mode)
{
    int src_fd = -1, dst_fd = -1;  /* 源文件和目标文件的文件描述符 */
    int success = FALSE;
    struct stat stat_buf;
    char *src, *dst;               /* mmap 映射后的内存指针 */

    /* 以只读方式打开源文件 */
    if ((src_fd = open(srcfile, O_RDONLY)) == -1) {
        ui_error (op, "Unable to open '%s' for copying (%s)",
                  srcfile, strerror (errno));
        goto done;
    }
    if (stat(dstfile, &stat_buf) == 0) {
        /*
         * 如果目标文件已经存在，先删除它。这样做是为了确保目标文件是全新创建的，
         * 而不是覆盖一个可能正在被其他程序使用的文件的内容。
         */
        if (unlink(dstfile) == -1 && errno != ENOENT) {
            ui_error (op, "Unable to delete existing file '%s' (%s)",
                      dstfile, strerror (errno));
            goto done;
        }
    }
    /* 以读写方式创建目标文件 */
    if ((dst_fd = open(dstfile, O_RDWR | O_CREAT, mode)) == -1) {
        ui_error (op, "Unable to create '%s' for copying (%s)",
                  dstfile, strerror (errno));
        goto done;
    }
    /* 获取源文件的大小 */
    if (fstat(src_fd, &stat_buf) == -1) {
        ui_error (op, "Unable to determine size of '%s' (%s)",
                  srcfile, strerror (errno));
        goto done;
    }
    /* 空文件无需复制内容，直接成功返回 */
    if (stat_buf.st_size == 0) {
        success = TRUE;
        goto done;
    }
    /*
     * 通过 lseek 到文件末尾并写入一个字节来扩展目标文件的大小，
     * 使其与源文件大小一致。这是为 mmap 做准备（mmap 需要文件有实际大小）。
     */
    if (lseek(dst_fd, stat_buf.st_size - 1, SEEK_SET) == -1) {
        ui_error (op, "Unable to set file size for '%s' (%s)",
                  dstfile, strerror (errno));
        goto done;
    }
    if (write(dst_fd, "", 1) != 1) {
        ui_error (op, "Unable to write file size for '%s' (%s)",
                  dstfile, strerror (errno));
        goto done;
    }
    /* 将源文件映射到内存（只读） */
    if ((src = mmap(0, stat_buf.st_size, PROT_READ,
                    MAP_FILE | MAP_SHARED, src_fd, 0)) == (void *) -1) {
        ui_error (op, "Unable to map source file '%s' for copying (%s)",
                  srcfile, strerror (errno));
        goto done;
    }
    /* 将目标文件映射到内存（读写） */
    if ((dst = mmap(0, stat_buf.st_size, PROT_READ | PROT_WRITE,
                    MAP_FILE | MAP_SHARED, dst_fd, 0)) == (void *) -1) {
        ui_error (op, "Unable to map destination file '%s' for copying (%s)",
                  dstfile, strerror (errno));
        goto done;
    }

    /* 使用 memcpy 进行内存到内存的数据复制 */
    memcpy (dst, src, stat_buf.st_size);

    /* 解除源文件的内存映射 */
    if (munmap (src, stat_buf.st_size) == -1) {
        ui_error (op, "Unable to unmap source file '%s' after copying (%s)",
                 srcfile, strerror (errno));
        goto done;
    }
    /* 解除目标文件的内存映射 */
    if (munmap (dst, stat_buf.st_size) == -1) {
        ui_error (op, "Unable to unmap destination file '%s' after "
                 "copying (%s)", dstfile, strerror (errno));
        goto done;
    }

    success = TRUE;

 done:

    if (success) {
        /*
         * 创建目标文件时使用的 mode 可能已被用户的 umask 修改，
         * 因此这里使用 fchmod 显式地重新设置正确的权限
         */

        fchmod(dst_fd, mode);
    }

    if (src_fd != -1) {
        close (src_fd);
    }
    if (dst_fd != -1) {
        close (dst_fd);
    }

    return success;
}



/*
 * write_temp_file() - 将给定数据写入一个临时文件，并设置指定的权限。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   len  - 要写入的数据长度（字节数）
 *   data - 要写入的数据指针
 *   perm - 临时文件的权限模式（如 0644）
 *
 * 处理流程：
 *   1. 使用 mkstemp() 在临时目录下创建唯一的临时文件
 *   2. 如果 len > 0 且 data 非空：
 *      a. 通过 lseek + write 设置文件大小
 *      b. 使用 mmap 映射文件到内存
 *      c. 使用 memcpy 将数据写入映射区域
 *   3. 使用 fchmod 设置所需权限
 *   4. 清理资源（解除映射、关闭文件描述符）
 *
 * 注意：如果 len 为 0 或 data 为 NULL，则只创建空文件并设置权限。
 *
 * 返回值：成功时返回临时文件的路径（调用者需要释放）；失败时返回 NULL
 */

char *write_temp_file(Options *op, const int len,
                      const void *data, mode_t perm)
{
    unsigned char *dst = (void *) -1;  /* mmap 映射后的内存指针 */
    char *tmpfile = NULL;              /* 临时文件路径 */
    int fd = -1;                       /* 临时文件的文件描述符 */
    int ret = FALSE;                   /* 操作结果标志 */

    /* 在临时目录下创建唯一的临时文件（XXXXXX 会被替换为唯一字符串） */

    tmpfile = nvstrcat(op->tmpdir, "/nv-tmp-XXXXXX", NULL);

    fd = mkstemp(tmpfile);
    if (fd == -1) {
        ui_warn(op, "Unable to create temporary file (%s).",
                strerror(errno));
        goto done;
    }

    /*
     * 如果提供了长度为 0 或数据指针为 NULL，则跳过写入操作，
     * 只创建空文件并设置权限。
     */

    if (len && data) {

        /* 通过 lseek + write 设置临时文件的大小 */

        if (lseek(fd, len - 1, SEEK_SET) == -1) {
            ui_warn(op, "Unable to set file size for temporary file (%s).",
                    strerror(errno));
            goto done;
        }
        if (write(fd, "", 1) != 1) {
            ui_warn(op, "Unable to write file size for temporary file (%s).",
                    strerror(errno));
            goto done;
        }

        /* 将临时文件映射到内存（读写模式） */

        if ((dst = mmap(0, len, PROT_READ | PROT_WRITE,
                    MAP_FILE | MAP_SHARED, fd, 0)) == (void *) -1) {
        ui_warn(op, "Unable to map temporary file (%s).", strerror(errno));
        goto done;

        }

        /* 通过内存拷贝将数据写入映射的文件区域 */

        memcpy(dst, data, len);
    }

    /* 设置临时文件的权限 */

    if (fchmod(fd, perm) == -1) {
        ui_warn(op, "Unable to set permissions %04o on temporary "
                "file (%s)", perm, strerror(errno));
        goto done;
    }

    ret = TRUE;

 done:

    /* 解除临时文件的内存映射 */

    if (dst != (void *) -1) {
        if (munmap(dst, len) == -1) {
            ui_warn(op, "Unable to unmap temporary file (%s).",
                    strerror(errno));
        }
    }

    /* 关闭临时文件的文件描述符 */

    if (fd != -1) close(fd);

    if (ret) {
        return tmpfile;    /* 成功：返回临时文件路径 */
    } else {
        nvfree(tmpfile);   /* 失败：释放路径字符串，返回 NULL */
        return NULL;
    }

} /* write_temp_file() */


/*
 * check_libGLX_indirect_target() - check_libGLX_indirect_links 的辅助函数。
 *
 * 检查安装程序是否应该安装（或覆盖）libGLX_indirect.so.0 符号链接。
 *
 * libGLX_indirect.so.0 是间接渲染（indirect rendering）库的符号链接。
 * 如果系统上已经存在该链接，且指向一个专门的间接渲染库（即链接目标的
 * 基本名称就是 "libGLX_indirect"），则不覆盖；如果它指向其他厂商的库，
 * 则应该覆盖。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   path - 符号链接要安装到的目标路径
 *
 * 返回值：
 *   TRUE  - 应该安装/覆盖此符号链接
 *   FALSE - 不应该覆盖（已存在且指向正确目标）
 */
static int check_libGLX_indirect_target(Options *op, const char *path)
{
    char *target = NULL;       /* 符号链接的最终解析目标路径 */
    char *base = NULL;         /* 目标文件的基本名称（不含目录） */
    char *ext = NULL;          /* 文件扩展名指针 */
    struct stat stat_buf;
    int ret;

    /* 如果文件不存在，则应该创建该符号链接 */
    if (lstat(path, &stat_buf) != 0) {
        return TRUE;
    }

    /* 如果文件存在但不是符号链接（可能是真实文件），则不覆盖 */
    if (!S_ISLNK(stat_buf.st_mode)) {
        return FALSE;
    }

    /* 如果无法解析该链接（断链），则覆盖它 */
    if (stat(path, &stat_buf) != 0) {
        return TRUE;
    }

    /*
     * 解析符号链接，追踪所有中间链接，直到找到最终的非链接文件。
     * 上面的 stat() 调用已经成功，说明链接链是有效的。
     */
    target = get_resolved_symlink_target(op, path);
    while (target != NULL) {
        char *nextTarget = NULL;
        if (lstat(target, &stat_buf) != 0) {
            free(target);
            target = NULL;
            break;
        }

        /* 如果已经不是符号链接，说明到达了最终目标 */
        if (!S_ISLNK(stat_buf.st_mode)) {
            break;
        }

        /* 继续追踪下一层符号链接 */
        nextTarget = get_resolved_symlink_target(op, target);
        free(target);
        target = nextTarget;
    }
    if (target == NULL) {
        /* 理论上不应该发生（因为前面 stat() 已经成功） */
        ui_error(op, "Unable to resolve symbolic link \"%s\"\n", path);
        return FALSE;
    }

    /* 获取链接目标的基本文件名（去掉目录路径部分） */
    base = basename(target);
    /* 去掉文件扩展名（.so.0 等） */
    ext = strchr(base, '.');
    if (ext != NULL) {
        *ext = '\0';
    }

    /*
     * 判断逻辑：
     * - 如果基本名称是 "libGLX_indirect"，说明已有的是专用的间接渲染库，
     *   不应该覆盖（返回 FALSE）
     * - 否则，说明它链接到了其他厂商的库，应该覆盖为 NVIDIA 的版本
     *   （返回 TRUE）
     */
    if (strcmp(base, "libGLX_indirect") == 0) {
        ret = FALSE;
    } else {
        ret = TRUE;
    }
    free(target);
    return ret;
}

/*
 * check_libGLX_indirect_links() - 查找包中所有 "libGLX_indirect.so.0" 符号链接条目，
 * 并决定是否安装它们。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 处理逻辑：
 *   - 如果用户显式指定了安装（NV_OPTIONAL_BOOL_TRUE），则全部保留
 *   - 如果用户显式指定了不安装（NV_OPTIONAL_BOOL_FALSE），则全部移除
 *   - 如果使用默认值（NV_OPTIONAL_BOOL_DEFAULT），则通过
 *     check_libGLX_indirect_target() 逐个检查，根据现有文件的状态决定
 */
static void check_libGLX_indirect_links(Options *op, Package *p)
{
    int i;

    /* 用户显式指定安装 libGLX_indirect 链接，直接返回（保留所有条目） */
    if (op->install_libglx_indirect == NV_OPTIONAL_BOOL_TRUE) {
        return;
    }

    /*
     * 遍历包中所有条目，找到 libGLX_indirect.so.0 的符号链接条目，
     * 根据目标文件的状态决定是保留还是移除该条目
     */
    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].dst != NULL && p->entries[i].type == FILE_TYPE_OPENGL_SYMLINK) {
            if (strcmp(p->entries[i].name, "libGLX_indirect.so.0") == 0) {
                int overwrite = FALSE;
                if (op->install_libglx_indirect == NV_OPTIONAL_BOOL_DEFAULT) {
                    /* 默认模式：检查目标路径上现有文件的状态来决定 */
                    if (check_libGLX_indirect_target(op, p->entries[i].dst)) {
                        overwrite = TRUE;
                    }
                }
                if (!overwrite) {
                    /* 不需要覆盖，将此条目从包中移除 */
                    invalidate_package_entry(&(p->entries[i]));
                }
            }
        }
    }
}

/*
 * set_destinations() - 为安装包中的每个文件条目计算最终的安装目标路径。
 *
 * 参数：
 *   op - 全局选项结构体，包含各类安装前缀和路径配置
 *   p  - 安装包结构体，包含所有要安装的文件条目
 *
 * 处理流程：
 *   1. 如果内核模块源码目录名未设置，生成默认值 "nvidia-<版本号>"
 *   2. 遍历包中的每个文件条目，根据文件类型（type）确定：
 *      - prefix: 安装前缀（如 /usr, /usr/local 等）
 *      - dir: 安装子目录（如 lib, lib64, bin 等）
 *      - path: 相对路径（如 vdpau, xorg/modules 等）
 *   3. 将 prefix + dir + path + name 拼接为完整的目标路径
 *   4. 在 x86_64 架构上，如果是 32 位兼容库且设置了 compat32_chroot，
 *      则在路径前追加 chroot 前缀（主要用于 Debian 系统）
 *
 * 目标路径的构造公式：
 *   dst = prefix/dir/path/name
 *
 * 注意：某些文件类型有特殊处理，例如：
 *   - FILE_TYPE_KERNEL_MODULE: 路径已在 add_kernel_module_to_package() 中设置
 *   - FILE_TYPE_GLVND_EGL_ICD_JSON: 路径稍后在 check_libglvnd_files() 中设置
 *   - 用户通过命令行覆盖的路径优先于默认计算
 *
 * 前提条件：调用此函数前，Options 中的各种前缀必须已经通过
 *   get_default_prefixes_and_paths() 或 get_prefixes() 设置好。
 *
 * 返回值：始终返回 TRUE
 */

int set_destinations(Options *op, Package *p)
{
    char *name;             /* 文件名（不含路径） */
    char *dir, *path;       /* 安装子目录和相对路径 */
    const char *prefix;     /* 安装前缀 */
    int i;

    /* 如果内核模块源码目录未设置，使用 "nvidia-<版本号>" 作为默认值 */
    if (!op->kernel_module_src_dir) {
        op->kernel_module_src_dir = nvstrcat("nvidia-", p->version, NULL);
    }

    for (i = 0; i < p->num_entries; i++) {
        /*
         * 如果用户通过命令行参数覆盖了某个文件类型的安装目标路径，
         * 则直接使用覆盖值，跳过后续的默认路径计算
         */
        if (op->file_type_destination_overrides[p->entries[i].type] != NULL) {
            p->entries[i].dst = nvstrcat(
                op->file_type_destination_overrides[p->entries[i].type], "/",
                p->entries[i].name, NULL);
            collapse_multiple_slashes(p->entries[i].dst);

            continue;
        }

        /*
         * 根据文件类型确定 prefix、dir 和 path 三个路径组成部分。
         * 不同类型的文件会安装到不同的目录结构中。
         */
        switch (p->entries[i].type) {

        /* ---- 内核模块源码和 DKMS 配置文件 ---- */
        case FILE_TYPE_KERNEL_MODULE_SRC:
        case FILE_TYPE_DKMS_CONF:
            if (op->no_kernel_module_source) {
                /* 用户请求不安装内核模块源码，跳过 */
                p->entries[i].dst = NULL;
                continue;
            }
            /* 通常安装到 /usr/src/nvidia-<version>/ 下 */
            prefix = op->kernel_module_src_prefix;
            dir = op->kernel_module_src_dir;
            path = p->entries[i].path;
            break;

        /* ---- OpenGL / GLVND / GLX / EGL 相关库和符号链接 ---- */
        case FILE_TYPE_OPENGL_LIB:
        case FILE_TYPE_OPENGL_SYMLINK:
        case FILE_TYPE_GLVND_LIB:
        case FILE_TYPE_GLVND_SYMLINK:
        case FILE_TYPE_GLX_CLIENT_LIB:
        case FILE_TYPE_GLX_CLIENT_SYMLINK:
        case FILE_TYPE_EGL_CLIENT_LIB:
        case FILE_TYPE_EGL_CLIENT_SYMLINK:
            /*
             * 根据架构兼容性决定安装路径：
             * - 32 位兼容库安装到 compat32 路径（如 /usr/lib32/）
             * - 原生库安装到 opengl 路径（如 /usr/lib64/ 或 /usr/lib/）
             */
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->opengl_libdir;
            }
            path = "";
            break;

        /* ---- Wine 兼容层库 ---- */
        case FILE_TYPE_WINE_LIB:
            prefix = op->wine_prefix;
            dir = op->wine_libdir;
            path = "";
            break;

        /* ---- VDPAU 视频解码加速库 ---- */
        case FILE_TYPE_VDPAU_LIB:
        case FILE_TYPE_VDPAU_SYMLINK:
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->opengl_libdir;
            }
            /* VDPAU 库通常放在 lib/vdpau/ 子目录下 */
            path = p->entries[i].path;
            break;

        /* ---- CUDA 库和 OpenCL 库（含包装器） ---- */
        case FILE_TYPE_CUDA_LIB:
        case FILE_TYPE_CUDA_SYMLINK:
        case FILE_TYPE_OPENCL_LIB:
        case FILE_TYPE_OPENCL_WRAPPER_LIB:
        case FILE_TYPE_OPENCL_LIB_SYMLINK:
        case FILE_TYPE_OPENCL_WRAPPER_SYMLINK:
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->opengl_libdir;
            }
            path = p->entries[i].path;
            break;

        /* ---- CUDA ICD（可安装客户端驱动）配置文件 ---- */
        case FILE_TYPE_CUDA_ICD:
            prefix = DEFAULT_CUDA_ICD_PREFIX;
            dir = DEFAULT_CUDA_ICD_DIR;
            path = "";
            break;

        /* ---- X.Org 模块（包括 GLX 扩展模块） ---- */
        case FILE_TYPE_XMODULE_SHARED_LIB:
        case FILE_TYPE_GLX_MODULE_SHARED_LIB:
        case FILE_TYPE_GLX_MODULE_SYMLINK:
            /* 安装到 X 模块目录，如 /usr/lib/xorg/modules/ */
            prefix = op->x_module_path;
            dir = "";
            path = p->entries[i].path;
            break;

        /* ---- TLS（线程本地存储）库 ---- */
        case FILE_TYPE_TLS_LIB:
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->opengl_libdir;
            }
            /* TLS 库可能在 tls/ 子目录下 */
            path = p->entries[i].path;
            break;

        /* ---- 工具库（如 libnvidia-cfg.so 等） ---- */
        case FILE_TYPE_UTILITY_LIB:
        case FILE_TYPE_UTILITY_LIB_SYMLINK:
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->utility_prefix;
                dir = op->utility_libdir;
            }
            path = "";
            break;

        /* ---- GBM（通用缓冲区管理）后端库符号链接 ---- */
        case FILE_TYPE_GBM_BACKEND_LIB_SYMLINK:
            /*
             * GBM 后端符号链接需要特殊处理：符号链接的目标（target）需要
             * 指向实际安装位置的完整路径，而不是相对路径
             */
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                p->entries[i].target = nvstrcat(op->compat32_prefix, "/", op->compat32_libdir, "/", p->entries[i].target, NULL);
            } else {
                p->entries[i].target = nvstrcat(op->utility_prefix, "/", op->utility_libdir, "/", p->entries[i].target, NULL);
            }
            /* 故意不 break，继续执行 GBM_BACKEND_LIB 的路径设置（fallthrough） */
        /* ---- GBM 后端库 ---- */
        case FILE_TYPE_GBM_BACKEND_LIB:
            /* 安装到 lib/gbm/ 子目录 */
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_gbm_backend_dir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->gbm_backend_dir;
            }
            path = "";
            break;

        /* ---- NVCUVID（CUDA 视频解码）库 ---- */
        case FILE_TYPE_NVCUVID_LIB:
        case FILE_TYPE_NVCUVID_LIB_SYMLINK:
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->opengl_libdir;
            }
            path = "";
            break;

        /* ---- NVENC（NVIDIA 视频编码 API）库 ---- */
        case FILE_TYPE_ENCODEAPI_LIB:
        case FILE_TYPE_ENCODEAPI_LIB_SYMLINK:
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                prefix = op->compat32_prefix;
                dir = op->compat32_libdir;
            } else {
                prefix = op->opengl_prefix;
                dir = op->opengl_libdir;
            }
            path = "";
            break;

        /* ---- VGX（虚拟 GPU）库（仅原生架构） ---- */
        case FILE_TYPE_VGX_LIB:
        case FILE_TYPE_VGX_LIB_SYMLINK:
            prefix = op->opengl_prefix;
            dir = op->opengl_libdir;
            path = "";
            break;

        /* ---- GRID / Flexera 许可库 ---- */
        case FILE_TYPE_GRID_LIB:
        case FILE_TYPE_GRID_LIB_SYMLINK:
        case FILE_TYPE_FLEXERA_LIB:
        case FILE_TYPE_FLEXERA_LIB_SYMLINK:
            prefix = op->opengl_prefix;
            dir = op->opengl_libdir;
            path = p->entries[i].path;
            break;

        /* ---- 安装器自身的二进制文件（nvidia-installer） ---- */
        case FILE_TYPE_INSTALLER_BINARY:
            prefix = op->utility_prefix;
            dir = op->utility_bindir;
            path = "";
            break;

        /* ---- 文档文件 ---- */
        case FILE_TYPE_DOCUMENTATION:
            prefix = op->documentation_prefix;
            dir = op->documentation_docdir;
            path = p->entries[i].path;
            break;

        /* ---- 手册页（man page） ---- */
        case FILE_TYPE_MANPAGE:
        case FILE_TYPE_NVIDIA_MODPROBE_MANPAGE:
            prefix = op->documentation_prefix;
            dir = op->documentation_mandir;
            path = p->entries[i].path;
            break;

        /* ---- 应用程序配置文件（Application Profile） ---- */
        case FILE_TYPE_APPLICATION_PROFILE:
            prefix = op->application_profile_path;
            dir = path = "";
            break;

        /* ---- 工具二进制文件（nvidia-smi, nvidia-settings 等） ---- */
        case FILE_TYPE_UTILITY_BINARY:
        case FILE_TYPE_UTILITY_BIN_SYMLINK:
            prefix = op->utility_prefix;
            dir = op->utility_bindir;
            path = "";
            break;

        /* ---- .desktop 桌面快捷方式文件 ---- */
        case FILE_TYPE_DOT_DESKTOP:
            /* 安装到 XDG 数据目录的 applications 子目录 */
            prefix = op->xdg_data_dir;
            dir = "applications";
            path = "";
            break;

        /* ---- 应用图标文件 ---- */
        case FILE_TYPE_ICON:
            prefix = op->icon_dir;
            dir = "";
            path = p->entries[i].path;
            break;

        /* ---- 内核模块 ---- */
        case FILE_TYPE_KERNEL_MODULE:

            /*
             * 内核模块的 dst 字段已在 add_kernel_module_to_package() 中设置，
             * 这里跳过不再重复计算
             */

            continue;

        /* ---- 内核模块签名密钥 ---- */
        case FILE_TYPE_MODULE_SIGNING_KEY:
            prefix = op->module_signing_key_path;
            dir = path = "";
            break;

        /* ---- 显式路径文件 / nvidia-modprobe / OpenGL 数据文件 ---- */
        case FILE_TYPE_EXPLICIT_PATH:
        case FILE_TYPE_NVIDIA_MODPROBE:
        case FILE_TYPE_OPENGL_DATA:
            /* 这些文件类型的安装路径直接由条目的 path 字段指定 */
            prefix = p->entries[i].path;
            dir = path = "";
            break;

        /* ---- X.Org OutputClass 配置文件 ---- */
        case FILE_TYPE_XORG_OUTPUTCLASS_CONFIG:
            /* 通常为 /usr/share/X11/xorg.conf.d/ */
            prefix = op->x_sysconfig_path;
            dir = path = "";
            break;

        /* ---- Vulkan ICD（可安装客户端驱动）配置 JSON ---- */
        case FILE_TYPE_VULKAN_ICD_JSON:
            /*
             * 由 Vulkan Linux ICD 加载器规范定义的路径
             */
            prefix = "/etc/vulkan/";
            path = p->entries[i].path;
            dir = "";
            break;

        /* ---- VulkanSC ICD 配置 JSON ---- */
        case FILE_TYPE_VULKANSC_ICD_JSON:
            /*
             * 由 VulkanSC Linux ICD 加载器规范定义的路径
             */
            prefix = "/etc/vulkansc/";
            path = p->entries[i].path;
            dir = "";
            break;

        /* ---- libglvnd EGL ICD 配置 JSON ---- */
        case FILE_TYPE_GLVND_EGL_ICD_JSON:
            /*
             * 此路径需要稍后在 check_libglvnd_files() 中设置。
             * 必须等到确定是否安装自带的 libglvnd 库之后，才能知道
             * JSON 文件应该放在哪里。
             */
            p->entries[i].dst = NULL;
            continue;

        /* ---- EGL 外部平台配置 JSON ---- */
        case FILE_TYPE_EGL_EXTERNAL_PLATFORM_JSON:
            prefix = op->external_platform_json_path;
            dir = path = "";
            break;

        /* ---- 内部工具文件（二进制/库/数据） ---- */
        case FILE_TYPE_INTERNAL_UTILITY_BINARY:
        case FILE_TYPE_INTERNAL_UTILITY_LIB:
        case FILE_TYPE_INTERNAL_UTILITY_DATA:
            /* 内部工具文件安装到固定路径 /usr/lib/nvidia/ */
            if (p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
                // TODO: 是否需要避免下面的 chroot 处理？
                prefix = "/usr/lib/nvidia/32";
            } else {
                prefix = "/usr/lib/nvidia";
            }
            dir = path = "";
            break;

        /* ---- GPU 固件文件 ---- */
        case FILE_TYPE_FIRMWARE:
            /* 安装到 /lib/firmware/nvidia/<version>/ 目录 */
            prefix = nvstrcat("/lib/firmware/nvidia/", p->version, "/", NULL);
            path = p->entries[i].path;
            dir = "";
            break;

        /* ---- systemd 服务单元文件 ---- */
        case FILE_TYPE_SYSTEMD_UNIT:
            prefix = op->systemd_unit_prefix;
            dir = path = "";
            break;

        /* ---- systemd 服务单元符号链接 ---- */
        case FILE_TYPE_SYSTEMD_UNIT_SYMLINK:
            /*
             * 从 systemd 系统配置目录创建符号链接，指向 systemd 单元目录中的
             * 服务文件。用于实现服务的自动启用。
             */
            p->entries[i].target = nvstrcat(op->systemd_unit_prefix, "/", p->entries[i].name, NULL);

            prefix = op->systemd_sysconf_prefix;
            path = p->entries[i].path;
            dir = "";
            break;

        /* ---- systemd 休眠/唤醒脚本 ---- */
        case FILE_TYPE_SYSTEMD_SLEEP_SCRIPT:
            prefix = op->systemd_sleep_prefix;
            dir = path = "";
            break;

        /* ---- 沙箱工具文件列表 JSON ---- */
        case FILE_TYPE_SANDBOXUTILS_FILELIST_JSON:
            prefix = "/usr/share/nvidia/files.d";
            dir = path = "";
            break;

        default:

            /*
             * 静默忽略所有不匹配的文件类型。例如，错误 TLS 类别的
             * 库文件可能会落入此处。
             */

            p->entries[i].dst = NULL;
            continue;
        }

        /*
         * 安全检查：如果任何路径组成部分为 NULL，则无法构造有效的目标路径，
         * 跳过此条目
         */
        if ((prefix == NULL) || (dir == NULL) || (path == NULL)) {
            p->entries[i].dst = NULL;
            continue;
        }

        name = p->entries[i].name;

        /* 拼接完整的目标路径：prefix/dir/path/name */
        p->entries[i].dst = nvstrcat(prefix, "/", dir, "/", path, "/", name, NULL);
        /* 清理路径中可能出现的多个连续斜杠（如 /usr//lib -> /usr/lib） */
        collapse_multiple_slashes(p->entries[i].dst);

#if defined(NV_X86_64)
        if ((p->entries[i].compat_arch == FILE_COMPAT_ARCH_COMPAT32) &&
            (op->compat32_chroot != NULL)) {

            /*
             * 对于 32 位兼容库，在路径前追加 chroot 前缀。
             * 这目前主要用于 Debian GNU/Linux x86-64 系统，
             * 未来可能会有更多用途。
             * 例如：/emul/ia32-linux + /usr/lib/libGL.so.1
             */

            char *dst = p->entries[i].dst;
            p->entries[i].dst = nvstrcat(op->compat32_chroot, dst, NULL);

            nvfree(dst);
        }
#endif /* NV_X86_64 */
    }

    return TRUE;

} /* set_destinations() */



/*
 * get_prefixes() - 获取各类文件的安装前缀路径。
 *
 * 在专家模式（--expert）下，逐一询问用户以下安装前缀：
 *   - X 安装前缀（x_prefix）
 *   - X 库安装路径（x_library_path）
 *   - X 模块安装路径（x_module_path）
 *   - OpenGL 安装前缀（opengl_prefix）
 *   - 文档安装前缀（documentation_prefix）
 *   - 工具安装前缀（utility_prefix）
 *   - 内核模块源码前缀和目录
 *   - 32 位兼容库的 chroot 和前缀（仅 x86_64 架构）
 *
 * 在非专家模式下，直接使用 parse_commandline() 中设置的默认值。
 *
 * 参数：
 *   op - 全局选项结构体（包含默认值，可能会被用户输入覆盖）
 *
 * 处理流程：
 *   1. 对于每个前缀，在专家模式下调用 ui_get_input() 让用户输入
 *   2. 使用 confirm_path() 验证路径存在或创建
 *   3. 移除路径末尾的斜杠
 *   4. 在专家模式下记录日志
 *
 * 返回值：成功返回 TRUE，如果用户中止则返回 FALSE
 */

int get_prefixes (Options *op)
{
    char *ret;   /* 用户输入的返回值 */

    /* 如果是专家模式，询问 X 安装前缀 */
    if (op->expert) {
        ret = ui_get_input(op, op->x_prefix,
                           "X installation prefix (only under "
                           "rare circumstances should this be changed "
                           "from the default)");
        if (ret && ret[0]) {
            op->x_prefix = ret; 
            if (!confirm_path(op, op->x_prefix)) return FALSE;
        }
    }

    remove_trailing_slashes(op->x_prefix);
    ui_expert(op, "X installation prefix is: '%s'", op->x_prefix);

    /*
     * 确定 X 模块和库的安装路径。这必须在默认前缀/路径设置之后进行，
     * 因为模块和库路径的计算依赖于 x_prefix 的值。
     * 仅在包中包含 X 相关文件时才需要。
     */

    if (op->x_files_packaged) {
        get_x_library_and_module_paths(op);
    }

    if (op->expert) {
        ret = ui_get_input(op, op->x_library_path,
                           "X library installation path (only under "
                           "rare circumstances should this be changed "
                           "from the default)");
        if (ret && ret[0]) {
            op->x_library_path = ret; 
            if (!confirm_path(op, op->x_library_path)) return FALSE;
        }
    }

    remove_trailing_slashes(op->x_library_path);
    ui_expert(op, "X library installation path is: '%s'", op->x_library_path);

    if (op->expert) {
        ret = ui_get_input(op, op->x_module_path,
                           "X module installation path (only under "
                           "rare circumstances should this be changed "
                           "from the default)");
        if (ret && ret[0]) {
            op->x_module_path = ret; 
            if (!confirm_path(op, op->x_module_path)) return FALSE;
        }
    }

    remove_trailing_slashes(op->x_module_path);
    ui_expert(op, "X module installation path is: '%s'", op->x_module_path);
        
    if (op->expert) {
        ret = ui_get_input(op, op->opengl_prefix,
                           "OpenGL installation prefix (only under "
                           "rare circumstances should this be changed "
                           "from the default)");
        if (ret && ret[0]) {
            op->opengl_prefix = ret;
            if (!confirm_path(op, op->opengl_prefix)) return FALSE;
        }
    }

    remove_trailing_slashes(op->opengl_prefix);
    ui_expert(op, "OpenGL installation prefix is: '%s'", op->opengl_prefix);


    if (op->expert) {
        ret = ui_get_input(op, op->documentation_prefix,
                           "Documentation installation prefix (only under "
                           "rare circumstances should this be changed "
                           "from the default)");
        if (ret && ret[0]) {
            op->documentation_prefix = ret;
            if (!confirm_path(op, op->documentation_prefix)) return FALSE;
        }
    }

    remove_trailing_slashes(op->documentation_prefix);
    ui_expert(op, "Documentation installation prefix is: '%s'", op->documentation_prefix);


    if (op->expert) {
        ret = ui_get_input(op, op->utility_prefix,
                           "Utility installation prefix (only under "
                           "rare circumstances should this be changed "
                           "from the default)");
        if (ret && ret[0]) {
            op->utility_prefix = ret;
            if (!confirm_path(op, op->utility_prefix)) return FALSE;
        }
    }

    remove_trailing_slashes(op->utility_prefix);
    ui_expert(op, "Utility installation prefix is: '%s'", op->utility_prefix);

    /* 在专家模式下，询问用户是否要安装内核模块源码 */
    if (op->expert) {
        op->no_kernel_module_source =
            !ui_yes_no(op, !op->no_kernel_module_source,
                      "Do you want to install kernel module sources?");
    }

    /* 如果需要安装内核模块源码，询问安装前缀和目录 */
    if (!op->no_kernel_module_source) {
        if (op->expert) {
            ret = ui_get_input(op, op->kernel_module_src_prefix,
                               "Kernel module source installation prefix");
            if (ret && ret[0]) {
                op->kernel_module_src_prefix = ret;
                if (!confirm_path(op, op->kernel_module_src_prefix))
                    return FALSE;
            }
        }
    
        remove_trailing_slashes(op->kernel_module_src_prefix);
        ui_expert(op, "Kernel module source installation prefix is: '%s'",
                  op->kernel_module_src_prefix);
    
        if (op->expert) {
            ret = ui_get_input(op, op->kernel_module_src_dir,
                               "Kernel module source installation directory");
            if (ret && ret[0]) {
                op->kernel_module_src_dir = ret;
                if (!confirm_path(op, op->kernel_module_src_dir)) return FALSE;
            }
        }
    
        remove_trailing_slashes(op->kernel_module_src_dir);
        ui_expert(op, "Kernel module source installation directory is: '%s'",
                  op->kernel_module_src_dir);
    }

    /*
     * 以下部分仅在 x86_64 架构上编译：处理 32 位兼容库的安装路径。
     * compat32_chroot 用于某些发行版（如旧版 Debian）将 32 位库放在
     * 独立的 chroot 目录中的情况。
     */
#if defined(NV_X86_64)
    if (op->expert) {
        ret = ui_get_input(op, op->compat32_chroot,
                           "Compat32 installation chroot (only under "
                           "rare circumstances should this be "
                           "changed from the default)");
        if (ret && ret[0]) {
            op->compat32_chroot = ret;
            if (!confirm_path(op, op->compat32_chroot)) return FALSE;
        }
    }

    remove_trailing_slashes(op->compat32_chroot);
    ui_expert(op, "Compat32 installation chroot is: '%s'",
              op->compat32_chroot);

    if (op->expert) {
        ret = ui_get_input(op, op->compat32_prefix,
                           "Compat32 installation prefix (only under "
                           "rare circumstances should this be "
                           "changed from the default)");
        if (ret && ret[0]) {
            op->compat32_prefix = ret;
            if (!confirm_path(op, op->compat32_prefix)) return FALSE;
        }
    }

    remove_trailing_slashes(op->compat32_prefix);
    ui_expert(op, "Compat32 installation prefix is: '%s'",
              op->compat32_prefix);
#endif /* NV_X86_64 */

    return TRUE;
    
} /* get_prefixes() */


/*
 * add_kernel_module_helper() - 将单个内核模块添加到安装包的条目列表中。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   p        - 安装包结构体
 *   filename - 内核模块的文件名（相对于构建目录，如 "nvidia.ko"）
 *
 * 处理流程：
 *   1. 构造源文件的完整路径：kernel_module_build_directory/filename
 *   2. 提取文件名的基本名部分（去掉路径前缀）
 *   3. 构造安装目标路径：kernel_module_installation_path/filename
 *   4. 将文件信息添加到包的条目列表中
 *
 * 注意：内核模块权限固定为 0644（所有者读写，其他人只读）
 */

static void add_kernel_module_helper(Options *op, Package *p,
                                     const char *filename)
{
    char *file, *name, *dst;

    /* 构造内核模块的完整源文件路径 */
    file = nvstrcat(p->kernel_module_build_directory, "/", filename, NULL);

    /* 提取文件名的基本名部分（最后一个 '/' 之后的部分） */
    name = strrchr(file, '/');

    if (name && name[0]) {
        name++;  /* 跳过 '/' 字符本身 */
    }

    if (!name || !name[0]) {
        name = file;  /* 如果没有 '/'，整个字符串就是文件名 */
    }

    /* 构造安装目标路径 */
    dst = nvstrcat(op->kernel_module_installation_path, "/", filename, NULL);

    /* 将内核模块条目添加到安装包中 */
    add_package_entry(p,
                      file,
                      NULL, /* path: 不需要相对路径 */
                      name,
                      NULL, /* target: 不是符号链接，无目标 */
                      dst,
                      FILE_TYPE_KERNEL_MODULE,
                      FILE_COMPAT_ARCH_NONE,
                      0644);
}

/*
 * add_kernel_modules_to_package() - 将所有已编译的内核模块添加到安装包的
 * 条目列表中，使它们在后续的安装过程中被复制到目标位置。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体（其中 kernel_modules[] 包含已编译模块的信息）
 *
 * 此函数遍历 p->kernel_modules[] 数组，对每个模块调用
 * add_kernel_module_helper() 将其添加到包的条目列表中。
 */

void add_kernel_modules_to_package(Options *op, Package *p)
{
    int i;

    for (i = 0; i < p->num_kernel_modules; i++) {
        add_kernel_module_helper(op, p, p->kernel_modules[i].module_filename);
    }
}



/*
 * remove_non_kernel_module_files_from_package() - 从安装包中移除所有
 * 非内核模块相关的文件条目。
 *
 * 此函数用于 --kernel-module-only 模式，该模式下只安装内核模块及其源码，
 * 不安装用户空间的库文件、工具等。
 *
 * 保留的文件类型：
 *   - FILE_TYPE_KERNEL_MODULE: 编译好的内核模块（.ko 文件）
 *   - FILE_TYPE_KERNEL_MODULE_SRC: 内核模块源码
 *   - FILE_TYPE_DKMS_CONF: DKMS 配置文件
 *
 * 参数：
 *   p - 安装包结构体
 */

void remove_non_kernel_module_files_from_package(Package *p)
{
    int i;

    for (i = 0; i < p->num_entries; i++) {
        if ((p->entries[i].type != FILE_TYPE_KERNEL_MODULE) &&
            (p->entries[i].type != FILE_TYPE_KERNEL_MODULE_SRC) &&
            (p->entries[i].type != FILE_TYPE_DKMS_CONF)) {
            invalidate_package_entry(&(p->entries[i]));
        }
    }
}


/*
 * package_entry_is_in_kernel_module_build_directory() - 判断包中第 i 个条目
 * 的源文件路径是否位于内核模块的构建目录下。
 *
 * 参数：
 *   p - 安装包结构体
 *   i - 条目的索引
 *
 * 处理逻辑：
 *   1. 去除构建目录路径和文件路径前面的 "./" 前缀
 *   2. 检查文件路径是否以构建目录路径开头
 *   3. 额外验证匹配不是部分的（即构建目录名不是某个更长目录名的前缀）
 *
 * 返回值：文件在构建目录下返回 TRUE，否则返回 FALSE
 */
static int package_entry_is_in_kernel_module_build_directory(Package *p, int i)
{
    const char *build_dir = p->kernel_module_build_directory;
    const char *file = p->entries[i].file;
    const char *cwd_prefix = "./";

    /* 去除路径开头可能存在的 "./" 前缀（可能有多个） */
    while (strncmp(build_dir, cwd_prefix, strlen(cwd_prefix)) == 0) {
        build_dir += strlen(cwd_prefix);
    }
    while (strncmp(file, cwd_prefix, strlen(cwd_prefix)) == 0) {
        file += strlen(cwd_prefix);
    }

    /* 检查构建目录路径是否是文件路径的前缀 */
    if (strncmp(file, build_dir, strlen(build_dir)) == 0) {
        if (build_dir[strlen(build_dir) - 1] == '/') {
            /*
             * 如果构建目录名以 '/' 结尾，则 strncmp 已经匹配了完整的
             * 目录名（包含目录分隔符），这是一个有效匹配
             */
            return TRUE;
        }
        if (file[strlen(build_dir)] == '/') {
            /*
             * 如果文件路径中匹配部分后面紧跟 '/'，说明不是部分匹配
             * （例如 build_dir="kernel" 不应匹配 file="kernel-extra/foo"，
             *  但应该匹配 file="kernel/foo"）
             */
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * remove_non_installed_kernel_module_source_files_from_package() - 从包中
 * 移除不在内核模块构建目录下的内核模块源文件。
 *
 * 某些内核模块源文件可能属于未被选择编译的模块，这些文件不应该被安装。
 * 此函数遍历所有 FILE_TYPE_KERNEL_MODULE_SRC 类型的条目，移除那些
 * 不在当前构建目录下的源文件。
 *
 * 参数：
 *   p - 安装包结构体
 */
void remove_non_installed_kernel_module_source_files_from_package(Package *p)
{
    int i;

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].type == FILE_TYPE_KERNEL_MODULE_SRC) {
            if (!package_entry_is_in_kernel_module_build_directory(p, i)) {
                invalidate_package_entry(&(p->entries[i]));
            }
        }
    }
}


/*
 * remove_opengl_files_from_package() - 从安装包中移除所有 OpenGL 相关文件。
 *
 * 当用户指定 --no-opengl-files 选项时调用此函数。
 * 通过检查条目的 caps.is_opengl 标志来判断是否为 OpenGL 文件。
 *
 * 参数：
 *   p - 安装包结构体
 */
void remove_opengl_files_from_package(Package *p)
{
    int i;

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].caps.is_opengl) {
            invalidate_package_entry(&(p->entries[i]));
        }
    }
}


/*
 * remove_wine_files_from_package() - 从安装包中移除所有 Wine 兼容层库文件。
 *
 * 当用户指定 --no-wine-files 选项时调用此函数。
 * Wine 库用于在 Linux 上运行 Windows 应用程序时提供 NVIDIA GPU 加速支持。
 *
 * 参数：
 *   p - 安装包结构体
 */
void remove_wine_files_from_package(Package *p)
{
    int i;

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].type == FILE_TYPE_WINE_LIB) {
            invalidate_package_entry(&(p->entries[i]));
        }
    }
}


/*
 * remove_systemd_files_from_package() - 从安装包中移除所有 systemd 相关文件。
 *
 * 当系统未使用 systemd 或用户选择不安装 systemd 服务时调用此函数。
 * 移除的文件类型包括：
 *   - systemd 服务单元文件（.service）
 *   - systemd 服务单元符号链接（用于自动启用服务）
 *   - systemd 休眠/唤醒脚本
 *
 * 参数：
 *   p - 安装包结构体
 */
void remove_systemd_files_from_package(Package *p)
{
    int i;

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].type == FILE_TYPE_SYSTEMD_UNIT ||
            p->entries[i].type == FILE_TYPE_SYSTEMD_UNIT_SYMLINK ||
            p->entries[i].type == FILE_TYPE_SYSTEMD_SLEEP_SCRIPT) {
            invalidate_package_entry(&(p->entries[i]));
        }
    }
}


/*
 * mode_string_to_mode() - 将八进制权限字符串（如 "0755"）转换为 mode_t 类型。
 *
 * 参数：
 *   op   - 全局选项结构体（用于错误消息输出）
 *   s    - 八进制权限字符串
 *   mode - 输出参数，转换后的 mode_t 值
 *
 * 使用 strtol() 以基数 8（八进制）解析字符串。
 * 如果解析失败（溢出或字符串中有非法字符），返回 FALSE。
 *
 * 返回值：成功返回 TRUE，解析失败返回 FALSE
 */

int mode_string_to_mode(Options *op, char *s, mode_t *mode)
{
    char *endptr;
    long int ret;

    ret = strtol(s, &endptr, 8);  /* 以八进制解析 */

    if ((ret == LONG_MIN) || (ret == LONG_MAX) || (*endptr != '\0')) {
        ui_error(op, "Error parsing permission string '%s' (%s)",
                 s, strerror (errno));
        return FALSE;
    }

    *mode = (mode_t) ret;

    return TRUE;

} /* mode_string_to_mode() */



/*
 * mode_to_permission_string() - 将 mode_t 权限位掩码转换为人类可读的
 * 9 字符权限字符串（如 "rwxr-xr-x"）。
 *
 * 参数：
 *   mode - 文件权限位掩码
 *
 * 返回值：新分配的 10 字节字符串（9 字符 + NULL 终止符），
 * 格式为 "rwxrwxrwx"，其中 '-' 表示对应权限未设置。
 *
 * 权限位的对应关系：
 *   位 8-6: 所有者的 读/写/执行 权限
 *   位 5-3: 组的 读/写/执行 权限
 *   位 2-0: 其他用户的 读/写/执行 权限
 */

char *mode_to_permission_string(mode_t mode)
{
    char *s = (char *) nvalloc(10);
    memset (s, '-', 9);  /* 先全部填充为 '-'（无权限） */

    /* 所有者权限 */
    if (mode & (1 << 8)) s[0] = 'r';  /* 读 */
    if (mode & (1 << 7)) s[1] = 'w';  /* 写 */
    if (mode & (1 << 6)) s[2] = 'x';  /* 执行 */

    /* 组权限 */
    if (mode & (1 << 5)) s[3] = 'r';
    if (mode & (1 << 4)) s[4] = 'w';
    if (mode & (1 << 3)) s[5] = 'x';

    /* 其他用户权限 */
    if (mode & (1 << 2)) s[6] = 'r';
    if (mode & (1 << 1)) s[7] = 'w';
    if (mode & (1 << 0)) s[8] = 'x';

    s[9] = '\0';
    return s;

} /* mode_to_permission_string() */



/*
 * confirm_path() - 确认指定路径存在。如果不存在，询问用户是否创建。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   path - 要确认的目录路径
 *
 * 处理流程：
 *   1. 检查路径是否已作为目录存在
 *   2. 如果不存在，提示用户选择：创建目录 或 中止安装
 *   3. 如果用户选择创建，调用 mkdir_with_log() 递归创建目录
 *
 * 设计说明（原作者 XXX 注释）：
 *   最初考虑只询问用户是否要创建目录，但由于 mkdir 可能因各种原因失败，
 *   直接在此处执行 mkdir 能让安装尽早发现失败，而不是延迟到后续步骤。
 *
 * 返回值：路径已存在或创建成功返回 TRUE，用户中止或创建失败返回 FALSE
 */

int confirm_path(Options *op, const char *path)
{
    const char *choices[2] = {
        "Create directory",
        "Abort installation"
    };

    /* return TRUE if the path already exists and is a directory */

    if (directory_exists(path)) return TRUE;
    
    if (ui_multiple_choice(op, choices, 2, 0, "The directory '%s' does not "
                           "exist; would you like to create it, or would you "
                           "prefer to abort installation?", path) == 0) {
        if (mkdir_with_log(op, path, 0755)) {
            return TRUE;
        } else {
            return FALSE;
        }
    }
    
    ui_message(op, "Not creating directory '%s'; aborting installation.",
               path);
    
    return FALSE;

} /* confirm_path() */



/*
 * mkdir_recursive() - 递归创建目录（等价于 `mkdir -p`），包括所有必要的
 * 父目录。如果 log 参数为 TRUE，则将创建的目录记录到备份日志中。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   path - 要创建的目录路径
 *   mode - 目录权限（如 0755）
 *   log  - 是否记录到备份日志（用于卸载时清理）
 *
 * 内部调用 nv_mkdir_recursive()（来自 misc 模块）执行实际的目录创建，
 * 然后处理日志记录和错误输出。
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int mkdir_recursive(Options *op, const char *path, const mode_t mode, int log)
{
    char *error_str = NULL;  /* 错误消息字符串 */
    char *log_str   = NULL;  /* 日志字符串（记录创建了哪些目录） */
    int success = FALSE;

    /* 调用底层函数执行递归目录创建 */
    success = nv_mkdir_recursive(path, mode, &error_str, log ? &log_str : NULL);

    /* 如果有日志信息，记录已创建的目录（用于后续卸载） */
    if (log_str) {
        log_mkdir(op, log_str);
        free(log_str);
    }

    /* 如果有错误信息，显示给用户 */
    if (error_str) {
        ui_error(op, "%s", error_str);
        free(error_str);
    }

    return success;
}



/*
 * mkdir_with_log() - mkdir_recursive() 的便捷封装，始终记录日志。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   path - 要创建的目录路径
 *   mode - 目录权限
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int mkdir_with_log(Options *op, const char *path, const mode_t mode)
{
    return mkdir_recursive(op, path, mode, TRUE);
}



/*
 * get_symlink_target() - 获取符号链接的目标路径。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   filename - 符号链接的路径
 *
 * 处理流程：
 *   1. 使用 lstat() 检查文件是否存在且为符号链接
 *   2. 使用 readlink() 读取链接目标，动态增长缓冲区直到足够大
 *   3. 在目标字符串末尾添加 NULL 终止符
 *
 * 注意：readlink() 不会自动添加 NULL 终止符，所以需要手动添加。
 * 缓冲区以 NV_LINE_LEN 为增量逐步增大，直到能容纳整个目标路径。
 *
 * 返回值：成功返回新分配的目标路径字符串（调用者需要释放）；
 *         失败返回 NULL
 */

char *get_symlink_target(Options *op, const char *filename)
{
    struct stat stat_buf;
    int ret, len = 0;
    char *buf = NULL;

    /* 检查文件是否存在 */
    if (lstat(filename, &stat_buf) == -1) {
        ui_error(op, "Unable to get file properties for '%s' (%s).",
                 filename, strerror(errno));
        return NULL;
    }

    /* 确认文件是符号链接 */
    if (!S_ISLNK(stat_buf.st_mode)) {
        ui_error(op, "File '%s' is not a symbolic link.", filename);
        return NULL;
    }

    /*
     * 动态增长缓冲区并调用 readlink()，直到缓冲区足够大来
     * 容纳完整的链接目标路径。每次增加 NV_LINE_LEN 字节。
     */

    do {
        len += NV_LINE_LEN;
        if (buf) free(buf);
        buf = nvalloc(len);
        ret = readlink(filename, buf, len - 1);
        if (ret == -1) {
            ui_error(op, "Failure while reading target of symbolic "
                     "link %s (%s).", filename, strerror(errno));
            free(buf);
            return NULL;
        }
    } while (ret >= (len - 1));

    /* readlink() 不会自动添加 NULL 终止符，需要手动添加 */
    buf[ret] = '\0';

    return buf;

} /* get_symlink_target() */



/*
 * get_resolved_symlink_target() - 与 get_symlink_target() 类似，但会将
 * 相对路径的链接目标解析为绝对路径。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   filename - 符号链接的路径
 *
 * 处理流程：
 *   1. 调用 get_symlink_target() 获取原始链接目标
 *   2. 如果目标是相对路径（不以 '/' 开头）：
 *      a. 获取符号链接所在目录
 *      b. 将目录与相对目标拼接
 *      c. 使用 realpath() 解析为规范化的绝对路径
 *
 * 注意：dirname(3) 可能会修改传入的字符串，因此先复制一份。
 *
 * 返回值：新分配的绝对路径字符串（调用者需要释放），或出错时返回 NULL
 */
char * get_resolved_symlink_target(Options *op, const char *filename)
{
    char *target = get_symlink_target(op, filename);
    if (target[0] != '/') {
        /* 链接目标是相对路径，需要转换为绝对路径 */
        char *filename_copy, *target_dir, *full_target_path;

        /* dirname(3) 可能修改传入的字符串，所以先复制一份 */
        filename_copy = nvstrdup(filename);
        target_dir = dirname(filename_copy);

        /* 将符号链接所在目录与相对目标路径拼接 */
        full_target_path = nvstrcat(target_dir, "/", target, NULL);

        nvfree(filename_copy);
        nvfree(target);

        /* 使用 realpath() 解析为规范化的绝对路径（消除 ".." 和 "." 等） */
        target = nvalloc(PATH_MAX);
        target = realpath(full_target_path, target);

        nvfree(full_target_path);
    }

    return target;

} /* get_resolved_symlink_target() */



/*
 * install_file() - 安装单个文件：将源文件复制到目标路径。
 *
 * 参数：
 *   op      - 全局选项结构体
 *   srcfile - 源文件路径
 *   dstfile - 目标文件路径
 *   mode    - 目标文件的权限模式
 *
 * 处理流程：
 *   1. 提取目标文件路径的目录部分
 *   2. 如果目录不存在，递归创建（并记录到备份日志）
 *   3. 调用 copy_file() 执行文件复制
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int install_file(Options *op, const char *srcfile,
                 const char *dstfile, mode_t mode)
{
    int retval;
    char *dirc, *dname;

    /* dirname() 可能修改传入的字符串，先复制一份 */
    dirc = nvstrdup(dstfile);
    dname = dirname(dirc);

    /* 确保目标目录存在 */
    if (!mkdir_with_log(op, dname, 0755)) {
        free(dirc);
        return FALSE;
    }

    /* 复制文件 */
    retval = copy_file(op, srcfile, dstfile, mode);
    free(dirc);

    return retval;

} /* install_file() */


/*
 * install_symlink() - 创建一个符号链接。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   linkname - 符号链接的目标（即链接指向的路径）
 *   dstfile  - 要创建的符号链接文件路径
 *
 * 处理流程：
 *   1. 提取目标路径的目录部分
 *   2. 如果目录不存在，递归创建
 *   3. 调用 symlink() 创建符号链接
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int install_symlink(Options *op, const char *linkname, const char *dstfile)
{
    char *dirc, *dname;

    /* dirname() 可能修改传入的字符串，先复制一份 */
    dirc = nvstrdup(dstfile);
    dname = dirname(dirc);

    /* 确保目标目录存在 */
    if (!mkdir_with_log(op, dname, 0755)) {
        free(dirc);
        return FALSE;
    }

    /* 创建符号链接 */
    if (symlink(linkname, dstfile)) {
        free(dirc);
        return FALSE;
    }

    free(dirc);
    return TRUE;

} /* install_symlink() */



/*
 * get_file_size() - 获取指定文件的大小（字节数）。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   filename - 文件路径
 *
 * 返回值：文件大小（字节），出错时返回 0
 */
size_t get_file_size(Options *op, const char *filename)
{
    struct stat stat_buf;

    if (stat(filename, &stat_buf) == -1) {
        ui_error(op, "Unable to determine file size of '%s' (%s).",
                 filename, strerror(errno));
        return 0;
    }

    return stat_buf.st_size;

} /* get_file_size() */



/*
 * fget_file_size() - 通过文件描述符获取文件大小（字节数）。
 *
 * 参数：
 *   op - 全局选项结构体
 *   fd - 已打开文件的文件描述符
 *
 * 返回值：文件大小（字节），出错时返回 0
 */
size_t fget_file_size(Options *op, const int fd)
{
    struct stat stat_buf;

    if (fstat(fd, &stat_buf) == -1) {
        ui_error(op, "Unable to determine file size of file "
                 "descriptor %d (%s).", fd, strerror(errno));
        return 0;
    }

    return stat_buf.st_size;

} /* fget_file_size() */



/*
 * get_tmpdir() - 获取系统临时目录路径。
 *
 * 按照以下优先级顺序查找可用的临时目录：
 *   1. TMPDIR 环境变量指定的目录
 *   2. /tmp
 *   3. 当前工作目录 "."
 *   4. HOME 环境变量指定的目录
 *
 * 参数：
 *   op - 全局选项结构体（此函数中未使用，保持接口一致性）
 *
 * 返回值：可用的临时目录路径，或 NULL（如果没有找到可用目录）
 */
char *get_tmpdir(Options *op)
{
    char *tmpdirs[] = { NULL, "/tmp", ".", NULL };
    int i;

    tmpdirs[0] = getenv("TMPDIR");
    tmpdirs[3] = getenv("HOME");

    for (i = 0; i < 4; i++) {
        if (tmpdirs[i] && directory_exists(tmpdirs[i])) {
            return (tmpdirs[i]);
        }
    }

    return NULL;

} /* get_tmpdir() */



/*
 * make_tmpdir() - 创建一个以进程 PID 命名的临时目录。
 *
 * 参数：
 *   op - 全局选项结构体（op->tmpdir 为基础临时目录路径）
 *
 * 创建的目录名格式：<tmpdir>/nvidia-<pid>
 * 例如：/tmp/nvidia-12345
 *
 * 如果该目录已存在，会先递归删除再重新创建。
 *
 * XXX 原作者注：应该使用 mkdtemp()，但并非所有系统都支持。
 *
 * 返回值：新创建的临时目录路径（调用者需要释放），或 NULL（创建失败）
 */

char *make_tmpdir(Options *op)
{
    char tmp[32], *tmpdir;

    snprintf(tmp, 32, "%d", getpid());

    tmpdir = nvstrcat(op->tmpdir, "/nvidia-", tmp, NULL);

    /* 如果目录已存在（可能是之前运行残留的），先删除 */
    if (directory_exists(tmpdir)) {
        remove_directory(op, tmpdir);
    }

    /* 创建临时目录（不记录到备份日志，因为是临时的） */
    if (!mkdir_recursive(op, tmpdir, 0755, FALSE)) {
        return NULL;
    }

    return tmpdir;

} /* make_tmpdir() */



/*
 * nvrename() - rename(2) 的替代实现，支持跨文件系统移动文件。
 *
 * 标准的 rename(2) 系统调用不能跨越文件系统边界（会返回 EXDEV 错误）。
 * 此函数通过 "复制 + 删除" 的方式实现跨文件系统的文件移动。
 *
 * 参数：
 *   op  - 全局选项结构体
 *   src - 源文件路径
 *   dst - 目标文件路径
 *
 * 处理流程：
 *   1. 获取源文件的属性（权限、时间戳等）
 *   2. 使用 copy_file() 将源文件复制到目标位置（保持原权限）
 *   3. 使用 utime() 将源文件的时间戳复制到目标文件
 *   4. 删除源文件
 *
 * 注意：时间戳传输失败只产生警告（不中止操作），但源文件删除失败会返回错误。
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int nvrename(Options *op, const char *src, const char *dst)
{
    struct stat stat_buf;
    struct utimbuf utime_buf;

    /* 获取源文件的属性 */
    if (stat(src, &stat_buf) == -1) {
        ui_error(op, "Unable to determine file attributes of file "
                 "%s (%s).", src, strerror(errno));
        return FALSE;
    }

    /* 复制源文件到目标位置 */
    if (!copy_file(op, src, dst, stat_buf.st_mode)) return FALSE;

    /* 将源文件的时间戳复制到目标文件 */
    utime_buf.actime = stat_buf.st_atime;   /* 访问时间 */
    utime_buf.modtime = stat_buf.st_mtime;   /* 修改时间 */

    if (utime(dst, &utime_buf) == -1) {
        /* 时间戳传输失败只是警告，不影响整体操作 */
        ui_warn(op, "Unable to transfer timestamp from '%s' to '%s' (%s).",
                   src, dst, strerror(errno));
    }

    /* 删除源文件，完成"移动"操作 */
    if (unlink(src) == -1) {
        ui_error(op, "Unable to delete '%s' (%s).", src, strerror(errno));
        return FALSE;
    }

    return TRUE;

} /* nvrename() */



/*
 * check_for_existing_rpms() - 检查系统上是否安装了旧版 NVIDIA RPM 包，
 * 如果发现冲突的 RPM 包，询问用户是否卸载它们。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 检查的 RPM 包（按依赖顺序排列）：
 *   - NVIDIA_GLX: NVIDIA OpenGL/GLX 库的 RPM 包
 *   - NVIDIA_kernel: NVIDIA 内核模块的 RPM 包
 *
 * 处理流程：
 *   1. 如果用户指定了 --no-rpms，跳过检查
 *   2. 使用 `rpm --query` 检查每个 RPM 包是否已安装
 *   3. 如果已安装，询问用户是否继续（继续则卸载旧包）
 *   4. 使用 `rpm --erase --nodeps` 卸载冲突的 RPM 包
 *
 * 注意：设置 LD_KERNEL_ASSUME=2.2.5 是为了兼容某些旧的 RPM 版本。
 *
 * 返回值：检查通过（或成功卸载旧包）返回 TRUE，用户中止返回 FALSE
 */

int check_for_existing_rpms(Options *op)
{
    /* 要检查和可能移除的 RPM 包列表，按依赖顺序排列 */

    const char *rpms[2] = { "NVIDIA_GLX", "NVIDIA_kernel" };

    char *data;
    int i, ret;

    if (op->no_rpms) {
        ui_log(op, "Skipping check for conflicting rpms.");
        return TRUE;
    }

    for (i = 0; i < 2; i++) {
        /* 查询 RPM 包是否已安装 */
        ret = run_command(op, NULL, FALSE, NULL, TRUE,
                          "env LD_KERNEL_ASSUME=2.2.5 rpm --query ",
                          rpms[i], NULL);

        if (ret == 0) {
            /* RPM 包已安装，询问用户是否继续（将卸载旧包） */
            if (ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                                   NUM_CONTINUE_ABORT_CHOICES,
                                   CONTINUE_CHOICE, /* 默认选择：继续 */
                                   "An %s rpm appears to already be installed "
                                   "on your system.  As part of installing the "
                                   "new driver, this %s rpm will be "
                                   "uninstalled. Are you sure you want to "
                                   "continue?",
                                   rpms[i], rpms[i]) == ABORT_CHOICE) {
                ui_log(op, "Installation aborted.");
                return FALSE;
            }

            /* 使用 --nodeps 忽略依赖关系强制卸载 */
            ret = run_command(op, &data, op->expert, NULL, TRUE,
                              "rpm --erase --nodeps ", rpms[i], NULL);

            if (ret == 0) {
                ui_log(op, "Removed %s.", rpms[i]);
            } else {
                ui_warn(op, "Unable to erase %s rpm: %s", rpms[i], data);
            }

            nvfree(data);
        }
    }

    return TRUE;

} /* check_for_existing_rpms() */



/*
 * copy_directory_contents() - 递归复制源目录中的所有内容到目标目录。
 *
 * 参数：
 *   op  - 全局选项结构体
 *   src - 源目录路径
 *   dst - 目标目录路径
 *
 * 处理流程：
 *   1. 打开源目录并遍历其中的每个条目
 *   2. 对于子目录：在目标位置创建对应目录，然后递归复制内容
 *   3. 对于普通文件：使用 copy_file() 复制
 *   4. 特殊文件（设备文件、管道等）被静默忽略
 *
 * 注意：目标目录中的子目录不会被记录到备份日志中（log 参数为 FALSE），
 * 因为此函数通常用于复制临时数据。
 *
 * 返回值：成功返回 TRUE，任何条目处理失败返回 FALSE
 */

int copy_directory_contents(Options *op, const char *src, const char *dst)
{
    DIR *dir;
    struct dirent *ent;
    int status = FALSE;

    if ((dir = opendir(src)) == NULL) {
        ui_error(op, "Unable to open directory '%s' (%s).",
                 src, strerror(errno));
        return FALSE;
    }

    while ((ent = readdir(dir)) != NULL) {
        struct stat stat_buf;
        char *srcfile, *dstfile;
        int ret;

        /* 跳过 "." 和 ".." */
        if (((strcmp(ent->d_name, ".")) == 0) ||
            ((strcmp(ent->d_name, "..")) == 0)) continue;

        /* 构造源和目标的完整路径 */
        srcfile = nvstrcat(src, "/", ent->d_name, NULL);
        dstfile = nvstrcat(dst, "/", ent->d_name, NULL);

        ret = (stat(srcfile, &stat_buf) != -1);

        if (ret) {
            if (S_ISDIR(stat_buf.st_mode)) {
                /* 子目录：先创建目标目录，然后递归复制内容 */
                ret = mkdir_recursive(op, dstfile, stat_buf.st_mode, FALSE) &&
                      copy_directory_contents(op, srcfile, dstfile);
            } else if (S_ISREG(stat_buf.st_mode)) {
                /* 普通文件：直接复制 */
                ret = copy_file(op, srcfile, dstfile, stat_buf.st_mode);
            }
            /* 其他类型（设备文件、管道等）被静默跳过 */
        }

        nvfree(srcfile);
        nvfree(dstfile);

        if (!ret) {
            goto done;
        }
    }

    status = TRUE;

  done:

    if (closedir(dir) != 0) {
        ui_error(op, "Failure while closing directory '%s' (%s).",
                 src, strerror(errno));

        return FALSE;
    }

    return status;

}



/*
 * pack_precompiled_files() - 创建预编译内核接口的打包文件并保存到磁盘。
 *
 * 预编译内核接口是一种优化机制：将已经为特定内核版本编译好的内核模块
 * 打包保存，这样在相同内核版本上重新安装时就不需要重新编译。
 *
 * 参数：
 *   op        - 全局选项结构体
 *   p         - 安装包结构体
 *   num_files - 要打包的预编译文件数量
 *   files     - 预编译文件信息数组
 *
 * 处理流程：
 *   1. 确保预编译内核接口目录存在
 *   2. 使用当前时间戳生成唯一的输出文件名
 *   3. 使用 uname() 获取系统信息作为描述
 *   4. 读取 /proc/version 字符串用于内核版本匹配
 *   5. 构建 PrecompiledInfo 结构体
 *   6. 调用 precompiled_pack() 执行实际的打包操作
 *
 * 输出文件名格式：<precompiled_dir>/precompiled-<version>.<timestamp>
 *
 * 返回值：成功返回 TRUE，失败返回 FALSE
 */

int pack_precompiled_files(Options *op, Package *p, int num_files,
                           PrecompiledFileInfo *files)
{
    char time_str[256], *proc_version_string;
    char *outfile = NULL, *descr = NULL;
    char *precompiled_dir = precompiled_kernel_interface_path(p);
    time_t t;
    struct utsname buf;
    int ret = FALSE;
    PrecompiledInfo *info = NULL;

    ui_log(op, "Packaging precompiled kernel interface.");

    /* 确保预编译内核接口的存储目录存在 */

    if (!mkdir_recursive(op, precompiled_dir, 0755, FALSE)) {
        ui_error(op, "Failed to create the directory '%s'!", precompiled_dir);
        goto done;
    }

    /* 使用当前时间戳作为输出文件名的一部分，确保唯一性 */

    t = time(NULL);
    snprintf(time_str, 256, "%lu", t);

    /* 获取系统信息（uname）作为预编译包的描述 */

    if (uname(&buf) != 0) {
        ui_error(op, "Failed to retrieve uname identifiers from the kernel!");
        return FALSE;
    }
    descr = nvstrcat(buf.sysname, " ",
                     buf.release, " ",
                     buf.version, " ",
                     buf.machine, NULL);

    /* 读取 /proc/version 字符串，用于匹配内核版本 */

    proc_version_string = read_proc_version(op, op->proc_mount_point);

    /* 构建预编译信息结构体 */

    info = nvalloc(sizeof(PrecompiledInfo));

    outfile = nvstrcat(precompiled_dir, "/",
                       PRECOMPILED_PACKAGE_FILENAME, "-", p->version,
                       ".", time_str, NULL);

    info->version = nvstrdup(p->version);
    info->proc_version_string = proc_version_string;
    info->description = descr;
    info->num_files = num_files;
    info->files = files;

    /* 调用预编译模块执行实际的打包和写入操作 */
    ret = precompiled_pack(info, outfile);

done:

    nvfree(precompiled_dir);
    nvfree(outfile);
    free_precompiled(info);

    if (!ret) {
        ui_error(op, "Unable to package precompiled kernel interface.");
    }

    return ret;
}



/*
 * nv_strreplace() - 字符串查找替换函数。替代 sed 的简单实现。
 *
 * 由于不能假设用户系统上安装了 sed，因此使用此函数执行简单的
 * 字符串搜索和替换操作。
 *
 * 参数：
 *   src     - 源字符串
 *   orig    - 要搜索的原始子串
 *   replace - 替换字符串
 *
 * 处理流程：
 *   1. 从源字符串开头开始搜索 orig
 *   2. 将 orig 之前的部分复制到结果字符串
 *   3. 将 replace 追加到结果字符串
 *   4. 跳过 orig，继续搜索下一个匹配
 *   5. 重复直到没有更多匹配
 *
 * 返回值：新分配的字符串，是 src 的副本，其中所有 orig 被替换为 replace。
 *         调用者需要释放返回的字符串。
 */

char *nv_strreplace(const char *src, const char *orig, const char *replace)
{
    const char *prev_s, *end_s, *s;   /* 源字符串中的指针 */
    char *d, *dst;                     /* 目标字符串中的指针 */
    int len, dst_len, orig_len, replace_len;
    int done = 0;                      /* 是否完成搜索的标志 */

    prev_s = s = src;
    end_s = src + strlen(src) + 1;    /* 源字符串末尾（含 NULL 终止符之后） */

    dst = NULL;
    dst_len = 0;

    orig_len = strlen(orig);
    replace_len = strlen(replace);

    do {
        /* 在源字符串中查找下一个 orig 的出现位置 */

        s = strstr(prev_s, orig);

        /*
         * 如果没有找到匹配，将 s 指向字符串末尾，
         * 并标记为已完成（下一次循环后退出）
         */

        if (!s) {
            s = end_s;
            done = 1;
        }

        /* 将 prev_s 和 s 之间的字符（即匹配之前的部分）复制到 dst */

        len = s - prev_s;
        dst = realloc(dst, dst_len + len + 1);
        d = dst + dst_len;
        strncpy(d, prev_s, len);
        d[len] = '\0';
        dst_len += len;

        /* 如果还没有完成，将替换字符串追加到 dst */

        if (!done) {
            dst = realloc(dst, dst_len + replace_len + 1);
            d = dst + dst_len;
            memcpy(d, replace, replace_len);
            d[replace_len] = '\0';
            dst_len += replace_len;
        }

        /* 跳过匹配到的 orig 字符串，继续搜索 */

        if (!done) prev_s = s + orig_len;

    } while (!done);

    return dst;

} /* nv_strreplace() */



/*
 * process_template_file() - 处理模板文件：将指定的模板文件复制到临时文件，
 * 并将其中的占位符 token 替换为实际值。
 *
 * 参数：
 *   op           - 全局选项结构体
 *   pe           - 模板文件的包条目（pe->file 为模板文件路径）
 *   tokens       - 以 NULL 结尾的占位符字符串数组
 *   replacements - 以 NULL 结尾的替换值字符串数组（与 tokens 一一对应）
 *
 * 处理流程：
 *   1. 打开并 mmap 模板文件到内存
 *   2. 将内容复制到一个可修改的字符串缓冲区
 *   3. 遍历 tokens/replacements 数组，逐个执行字符串替换
 *   4. 创建临时文件，将处理后的内容 mmap 写入
 *   5. 清理所有资源
 *
 * 用途：用于处理 .desktop 文件和 dkms.conf 等需要运行时替换占位符的文件。
 *
 * 返回值：成功时返回临时文件路径（调用者需要释放）；失败返回 NULL
 */

char *process_template_file(Options *op, PackageEntry *pe,
                            char **tokens, char **replacements)
{
    int failed, src_fd, dst_fd, len;
    struct stat stat_buf;
    char *src, *dst, *tmp, *tmp0, *tmpfile = NULL;
    char *token, *replacement;

    failed = FALSE;
    src_fd = dst_fd = -1;
    tmp = tmp0 = src = dst = tmpfile = NULL;
    len = 0;

    /* 打开模板文件 */

    if ((src_fd = open(pe->file, O_RDONLY)) == -1) {
        ui_error(op, "Unable to open '%s' for copying (%s)",
                 pe->file, strerror(errno));
        return NULL;
    }

    /* 获取模板文件大小 */

    if (fstat(src_fd, &stat_buf) == -1) {
        ui_error(op, "Unable to determine size of '%s' (%s)",
                 pe->file, strerror(errno));
        failed = TRUE; goto done;
    }

    /* 将模板文件映射到内存（只读） */

    if ((src = mmap(0, stat_buf.st_size, PROT_READ,
                    MAP_FILE|MAP_SHARED, src_fd, 0)) == MAP_FAILED) {
        ui_error (op, "Unable to map source file '%s' for "
                  "copying (%s)", pe->file, strerror(errno));
        src = NULL;
        failed = TRUE; goto done;
    }

    /* 空文件无需处理 */
    if (!src) {
        ui_log(op, "%s is empty; skipping.", pe->file);
        failed = TRUE; goto done;
    }

    /*
     * 将 mmap 的文件内容复制到一个可修改的字符串缓冲区中
     * （mmap 的区域是只读的，不能直接在其上做字符串替换）
     * 额外分配 1 字节用于 NULL 终止符
     */

    tmp = nvalloc(stat_buf.st_size + 1);
    memcpy(tmp, src, stat_buf.st_size);
    tmp[stat_buf.st_size] = '\0';

    /* 初始化 token 和 replacement 指针，准备遍历替换数组 */

    token = *tokens;
    replacement = *replacements;

    /* 逐个执行 token -> replacement 的替换 */
    while (token != NULL && replacement != NULL) {
        /*
         * 在模板内容中将所有 'token' 替换为 'replacement'。
         * nv_strreplace() 返回新分配的字符串，释放旧的。
         */
        tmp0 = nv_strreplace(tmp, token, replacement);
        nvfree(tmp);
        tmp = tmp0;
        token = *(++tokens);
        replacement = *(++replacements);
    }

    /* 创建临时文件来存储处理后的模板内容 */

    tmpfile = nvstrcat(op->tmpdir, "/template-XXXXXX", NULL);
    if ((dst_fd = mkstemp(tmpfile)) == -1) {
        ui_error(op, "Unable to create temporary file (%s)",
                 strerror(errno));
        failed = TRUE; goto done;
    }

    /* 设置临时文件的大小（等于处理后内容的长度） */

    len = strlen(tmp);

    if (lseek(dst_fd, len - 1, SEEK_SET) == -1) {
        ui_error(op, "Unable to set file size for '%s' (%s)",
                  tmpfile, strerror(errno));
        failed = TRUE; goto done;
    }
    if (write(dst_fd, "", 1) != 1) {
        ui_error(op, "Unable to write file size for '%s' (%s)",
                 tmpfile, strerror(errno));
        failed = TRUE; goto done;
    }

    /* 将临时文件映射到内存（读写模式） */

    if ((dst = mmap(0, len, PROT_READ | PROT_WRITE,
                    MAP_FILE|MAP_SHARED, dst_fd, 0)) == MAP_FAILED) {
        ui_error(op, "Unable to map destination file '%s' for "
                 "copying (%s)", tmpfile, strerror(errno));
        dst = NULL;
        failed = TRUE; goto done;
    }

    /* 将处理后的内容写入临时文件 */

    memcpy(dst, tmp, len);

done:

    /* 清理：解除源文件的内存映射 */
    if (src) {
        if (munmap(src, stat_buf.st_size) == -1) {
            ui_error(op, "Unable to unmap source file '%s' after "
                     "copying (%s)", pe->file,
                     strerror(errno));
        }
    }

    /* 清理：解除目标文件的内存映射 */
    if (dst) {
        if (munmap(dst, len) == -1) {
            ui_error (op, "Unable to unmap destination file '%s' "
                      "after copying (%s)", tmpfile, strerror(errno));
        }
    }

    if (src_fd != -1) close(src_fd);
    if (dst_fd != -1) {
        close(dst_fd);
        /* 如果发生错误，删除已创建的临时文件 */
        if (failed) unlink(tmpfile);
    }

    if (failed) {
        nvfree(tmpfile); tmpfile = NULL;
    }

    nvfree(tmp);

    return tmpfile;

} /* process_template_files() */



/*
 * process_dot_desktop_files() - 处理包中的 .desktop 桌面快捷方式文件。
 *
 * .desktop 文件是 Linux 桌面环境中的应用程序快捷方式配置文件，
 * 遵循 FreeDesktop.org 的 Desktop Entry 规范。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 处理流程：
 *   1. 构造 __UTILS_PATH__ 的替换值（工具二进制文件的实际安装路径）
 *   2. 遍历包中所有 FILE_TYPE_DOT_DESKTOP 类型的条目
 *   3. 对每个 .desktop 模板文件：
 *      a. 使原始模板条目无效
 *      b. 调用 process_template_file() 执行占位符替换
 *      c. 将处理后的临时文件作为新条目添加到包中
 *
 * 替换规则：
 *   __UTILS_PATH__ -> <utility_prefix>/<utility_bindir>
 *   例如：__UTILS_PATH__ -> /usr/bin
 *
 * 注意事项（XXX）：
 *   由于处理后的文件路径是由 mkstemp() 生成的，其文件名与原始模板不同，
 *   因此 'name' 字段需要从原始条目中复制（strdup），这会造成少量内存泄漏。
 */

void process_dot_desktop_files(Options *op, Package *p)
{
    int i;
    char *tmpfile;

    /* 定义 token/replacement 数组：将 __UTILS_PATH__ 替换为实际路径 */
    char *tokens[2] = { "__UTILS_PATH__", NULL };
    char *replacements[2] = { NULL, NULL };

    /* 记录当前条目数（因为循环中会添加新条目） */
    int package_num_entries = p->num_entries;

    /* 构造工具二进制文件的完整安装路径 */
    replacements[0] = nvstrcat(op->utility_prefix,
                               "/", op->utility_bindir, NULL);

    remove_trailing_slashes(replacements[0]);
    collapse_multiple_slashes(replacements[0]);

    for (i = 0; i < package_num_entries; i++) {
        if ((p->entries[i].type == FILE_TYPE_DOT_DESKTOP)) {

            /* 使原始模板文件条目无效（不再安装原始模板） */

            invalidate_package_entry(&(p->entries[i]));

            /* 处理模板文件，生成替换后的临时文件 */
            tmpfile = process_template_file(op, &p->entries[i], tokens,
                                            replacements);
            if (tmpfile != NULL) {
                /*
                 * 将处理后的文件作为新条目添加到包中。
                 *
                 * XXX 注意：'name' 是文件的基本名部分（如 "nvidia-settings.desktop"）。
                 * 通常 'name' 直接指向 'file' 字符串内部，但这里 'file' 是
                 * mkstemp() 生成的临时路径，basename 不同于原始文件名。
                 * 因此需要从原始条目中 strdup 出来。这个 'name' 字符串会泄漏。
                 */

                add_package_entry(p,
                                  tmpfile,
                                  nvstrdup(p->entries[i].path),
                                  nvstrdup(p->entries[i].name),
                                  NULL, /* target: 不是符号链接 */
                                  NULL, /* dst: 稍后由 set_destinations() 设置 */
                                  FILE_TYPE_DOT_DESKTOP,
                                  p->entries[i].compat_arch,
                                  p->entries[i].mode);
            }
        }
    }

    nvfree(replacements[0]);

} /* process_dot_desktop_files() */



/*
 * process_dkms_conf() - 处理 DKMS（Dynamic Kernel Module Support）配置文件模板。
 *
 * DKMS 是一个内核模块管理框架，允许在内核升级时自动重新编译内核模块。
 * dkms.conf 是 DKMS 的配置文件，需要根据当前安装的内核模块列表进行定制。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 替换规则：
 *   __VERSION_STRING    -> 驱动版本号（如 "535.129.03"）
 *   __DKMS_MODULES      -> DKMS 模块声明列表（BUILT_MODULE_NAME 和
 *                          DEST_MODULE_LOCATION 数组）
 *   __JOBS              -> 编译并行度（-j 参数值）
 *   __EXCLUDE_MODULES   -> 排除的模块列表
 *   "will be generated" -> "was generated"（修改注释中的时态）
 *
 * 处理流程：
 *   1. 构建 DKMS 模块列表字符串（遍历所有内核模块）
 *   2. 遍历包中所有 DKMS_CONF 类型的条目
 *   3. 仅处理位于内核模块构建目录中的 dkms.conf 文件
 *   4. 执行模板替换，生成临时文件，添加到包中
 */

void process_dkms_conf(Options *op, Package *p)
{
    int i;
    char *tmpfile;

    /* 定义要替换的占位符和对应的替换值 */
    char *tokens[] = { "__VERSION_STRING", "__DKMS_MODULES", "__JOBS",
                       "__EXCLUDE_MODULES", "will be generated", NULL };
    char *replacements[] = { p->version, NULL, NULL, p->excluded_kernel_modules,
                             "was generated", NULL };

    int package_num_entries = p->num_entries;

    replacements[1] = nvstrdup("");  /* DKMS 模块列表（下面构建） */
    replacements[2] = nvasprintf("%d", op->concurrency_level);  /* 并行编译数 */

    /*
     * 构建 DKMS 模块列表字符串。
     * 为每个要安装的内核模块生成 BUILT_MODULE_NAME 和
     * DEST_MODULE_LOCATION 配置行。
     */
    for (i = 0; i < p->num_kernel_modules; i++) {
        char *old_modules = replacements[1];
        char *index = nvasprintf("%d", i);

        /* 拼接模块名和安装位置的 DKMS 配置行 */
        replacements[1] = nvstrcat(old_modules,
                                   "BUILT_MODULE_NAME[", index, "]=\"",
                                   p->kernel_modules[i].module_name, "\"\n",
                                   "DEST_MODULE_LOCATION[", index, "]=",
                                   "\"/kernel/drivers/video\"\n", NULL);

        nvfree(index);
        nvfree(old_modules);
    }

    for (i = 0; i < package_num_entries; i++) {
        if ((p->entries[i].type == FILE_TYPE_DKMS_CONF)) {

            /* 使原始模板条目无效 */

            invalidate_package_entry(&(p->entries[i]));

            /* 仅处理位于内核模块构建目录下的 dkms.conf 模板 */

            if (package_entry_is_in_kernel_module_build_directory(p, i)) {
                tmpfile = process_template_file(op, &p->entries[i], tokens,
                                                replacements);
                if (tmpfile != NULL) {
                    /*
                     * 将处理后的文件添加到包中。
                     * XXX 与 process_dot_desktop_files() 中相同的 'name' 泄漏问题。
                     */

                    add_package_entry(p,
                                      tmpfile,
                                      nvstrdup(p->entries[i].path),
                                      nvstrdup(p->entries[i].name),
                                      NULL, /* target */
                                      NULL, /* dst */
                                      FILE_TYPE_DKMS_CONF,
                                      p->entries[i].compat_arch,
                                      p->entries[i].mode);
                }
            }
        }
    }

    nvfree(replacements[2]);
    nvfree(replacements[1]);

}

/*
 * set_security_context() - 设置文件的 SELinux 安全上下文。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   filename - 要设置安全上下文的文件路径
 *   type     - SELinux 类型标签（如 "textrel_shlib_t"）
 *
 * 如果 SELinux 未启用，直接返回 TRUE（视为成功）。
 * 使用 chcon(1) 命令设置安全上下文，而不是直接操作 xattr API。
 *
 * 实现说明（见 bug 3876232）：
 *   虽然可以直接使用 xattr(7) API 操作 "security.selinux" 扩展属性，
 *   但 chcon(1) 更好地抽象了 xattr 格式的细节。不过在某些特殊系统上
 *   chcon 可能不可用。
 *
 * 返回值：成功或 SELinux 未启用返回 TRUE，失败返回 FALSE
 */
int set_security_context(Options *op, const char *filename, const char *type)
{
    int ret = FALSE;

    /* SELinux 未启用，无需设置安全上下文 */
    if (op->selinux_enabled == FALSE) {
        return TRUE;
    }

    /* 使用 chcon 命令设置文件的 SELinux 类型标签 */
    ret = run_command(op, NULL, FALSE, NULL, TRUE,
                      op->utils[CHCON], " -t ", type, " ", filename, NULL);

    return ret == 0;
}


/*
 * native_libdirs - 当前原生架构的候选库目录列表。
 *
 * 按优先级排列，用于自动检测系统上的库安装目录。
 * 不同架构使用不同的目录列表：
 *   - x86_64: x86_64-linux-gnu, lib64, lib
 *   - i386:   i386-linux-gnu, lib32, lib
 *   - ARM:    对应的 triplet 目录
 *   - AArch64: aarch64-linux-gnu, lib64, lib
 *   - PPC64LE: powerpc64le-linux-gnu, lib64, lib
 *
 * 列表以 NULL 结尾。
 */
static char * const native_libdirs[] = {
#if defined(NV_X86_64)
    DEFAULT_AMD64_TRIPLET_LIBDIR,
#elif defined(NV_X86)
    DEFAULT_IA32_TRIPLET_LIBDIR,
#elif defined(NV_ARMV7)
#if defined(NV_GNUEABIHF)
    DEFAULT_ARMV7HF_TRIPLET_LIBDIR,
#else
    DEFAULT_ARMV7_TRIPLET_LIBDIR,
#endif /* GNUEABIHF */
#elif defined(NV_AARCH64)
    DEFAULT_AARCH64_TRIPLET_LIBDIR,
#elif defined(NV_PPC64LE)
    DEFAULT_PPC64LE_TRIPLET_LIBDIR,
#else
#error Unknown architecture! Please update utils.mk to add support for this \
TARGET_ARCH, and make sure that an architecture-specific NV_$ARCH macro gets \
defined, and that NV_ARCH_BITS gets defined to the correct word size in bits.
#endif

#if NV_ARCH_BITS == 32
    DEFAULT_32BIT_LIBDIR,
#elif NV_ARCH_BITS == 64
    DEFAULT_64BIT_LIBDIR,
#endif

    DEFAULT_LIBDIR,
    NULL
};


/*
 * compat_libdirs - 32 位兼容库的候选目录列表（仅 x86_64 架构）。
 *
 * 用于在 64 位系统上查找 32 位库的安装目录。
 * 候选目录：i386-linux-gnu, lib32, lib
 */
#if defined(NV_X86_64)
static char * const compat_libdirs[] = {
    DEFAULT_IA32_TRIPLET_LIBDIR,
    DEFAULT_32BIT_LIBDIR,
    DEFAULT_LIBDIR,
    NULL
};
#endif



/*
 * get_ldconfig_cache() - 获取 ldconfig(8) 的库缓存内容。
 *
 * 通过运行 `ldconfig -p` 命令获取系统动态链接库的缓存列表。
 * 这个缓存用于判断哪些库目录正在被系统使用。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：缓存内容字符串（调用者需要释放），或出错时返回 NULL
 */

static char *get_ldconfig_cache(Options *op)
{
    char *data;
    int ret;

    ret = run_command(op, &data, FALSE, NULL, FALSE,
                      op->utils[LDCONFIG], " -p", NULL);

    if (ret != 0) {
        nvfree(data);
        return NULL;
    }

    return data;
}


/*
 * find_libdir() - 在给定的前缀路径下搜索候选库目录。
 *
 * 参数：
 *   list            - 以 NULL 结尾的候选目录名数组
 *   prefix          - 安装前缀（如 "/usr"）
 *   ldconfig_cache  - ldconfig 缓存内容（如果非 NULL，在缓存中搜索；否则检查文件系统）
 *   chroot          - 可选的 chroot 前缀（如 "/emul/ia32-linux"）
 *
 * 搜索策略：
 *   - 如果提供了 ldconfig_cache，在缓存中搜索路径字符串
 *   - 否则在文件系统上检查目录是否存在
 *
 * 返回值：找到的第一个目录名（来自 list 数组的指针），或 NULL
 */

static char *find_libdir(char * const * list, const char *prefix,
                         const char *ldconfig_cache, const char *chroot)
{
    int i;
    char *path = NULL;

    for (i = 0; list[i]; i++) {
        nvfree(path);

        /* 构造完整路径：[chroot]/prefix/list[i]/ */
        path = nvstrcat(chroot ? chroot : "",
                        "/", prefix, "/", list[i], "/", NULL);
        collapse_multiple_slashes(path);

        if (ldconfig_cache) {
            /* 在 ldconfig 缓存中搜索该路径 */
            if (strstr(ldconfig_cache, path)) {
                break;
            }
        } else {
            /* 检查文件系统上该目录是否存在 */
            if (directory_exists(path)) {
                break;
            }
        }
    }

    nvfree(path);
    return list[i];  /* 如果遍历完都没找到，list[i] 为 NULL */
}


/*
 * find_libdir_and_fall_back() - 在前缀路径下搜索可用的库目录，
 * 如果找不到则回退到默认值。
 *
 * 参数：
 *   op              - 全局选项结构体
 *   list            - 候选目录名列表
 *   prefix          - 安装前缀
 *   ldconfig_cache  - ldconfig 缓存内容
 *   name            - 目录类型的描述性名称（用于警告消息，如 "library"）
 *
 * 搜索策略（按优先级）：
 *   1. 在 ldconfig 缓存中搜索
 *   2. 在文件系统上搜索
 *   3. 回退到 DEFAULT_LIBDIR 并打印警告
 *
 * 返回值：找到的目录名，或 DEFAULT_LIBDIR（回退值）
 */
static char * find_libdir_and_fall_back(Options *op, char * const * list,
                                        const char *prefix,
                                        const char *ldconfig_cache,
                                        const char *name)
{
    /* 第一次尝试：在 ldconfig 缓存中搜索 */
    char *libdir = find_libdir(list, prefix, ldconfig_cache, NULL);
    if (!libdir) {
        /* 第二次尝试：在文件系统上搜索 */
        libdir = find_libdir(list, prefix, NULL, NULL);
    }
    if (!libdir) {
        /* 回退：使用默认值，并警告用户 */
        libdir = DEFAULT_LIBDIR;
        ui_warn(op, "Unable to determine the default %s path. The path %s/%s "
                    "will be used, but this path was not detected in the "
                    "ldconfig(8) cache, and no directory exists at this path, "
                    "so it is likely that libraries installed there will not "
                    "be found by the loader.", name, prefix, libdir);
    }

    return libdir;
}


/*
 * get_default_prefixes_and_paths() - 根据系统架构、Linux 发行版和已安装的
 * X.Org 版本，设置所有默认的安装前缀和路径。
 *
 * 参数：
 *   op - 全局选项结构体（各路径字段如果已被命令行参数设置，则不会被覆盖）
 *
 * 设置的路径包括：
 *   - opengl_prefix/opengl_libdir: OpenGL 库的安装路径
 *   - gbm_backend_dir: GBM 后端库目录
 *   - x_prefix/x_libdir/x_moddir: X.Org 相关路径
 *   - compat32_prefix: 32 位兼容库前缀（仅 x86_64）
 *   - utility_prefix/utility_libdir/utility_bindir: 工具文件路径
 *   - xdg_data_dir/icon_dir: 桌面数据和图标路径
 *   - documentation_prefix/documentation_docdir/documentation_mandir: 文档路径
 *   - application_profile_path: 应用程序配置文件路径
 *   - kernel_module_src_prefix: 内核模块源码前缀
 *   - module_signing_key_path: 模块签名密钥路径
 *   - systemd_unit_prefix/systemd_sleep_prefix/systemd_sysconf_prefix: systemd 路径
 *   - wine_prefix/wine_libdir: Wine 兼容库路径
 *
 * 库目录的检测策略：
 *   1. 首先查询 ldconfig 缓存确定系统使用的库目录
 *   2. 如果缓存中未找到，检查文件系统
 *   3. 如果都未找到，使用默认值并警告用户
 */

void get_default_prefixes_and_paths(Options *op)
{
    char *default_libdir, *ldconfig_cache;

    /* 设置 OpenGL 前缀的默认值（通常为 /usr） */
    if (!op->opengl_prefix)
        op->opengl_prefix = DEFAULT_OPENGL_PREFIX;

    /* 获取 ldconfig 缓存用于检测库目录 */
    ldconfig_cache = get_ldconfig_cache(op);

    /* 检测原生架构的默认库目录（如 lib64, lib/x86_64-linux-gnu 等） */
    default_libdir = find_libdir_and_fall_back(op, native_libdirs,
                                               op->opengl_prefix,
                                               ldconfig_cache, "library");

    /* 设置 OpenGL 库目录 */
    if (!op->opengl_libdir)
        op->opengl_libdir = default_libdir;

    /* GBM 后端库目录通常在库目录的 gbm 子目录下 */
    if (!op->gbm_backend_dir)
        op->gbm_backend_dir = nvstrcat(default_libdir, "/", "gbm", NULL);

    /* 设置 X.Org 安装前缀 */
    if (!op->x_prefix) {
        if (op->modular_xorg) {
            /* 模块化 X.Org 7.x: 通常使用 /usr */
            op->x_prefix = XORG7_DEFAULT_X_PREFIX;
        } else {
            /* 传统 XFree86/X.Org 6.x: 通常使用 /usr/X11R6 */
            op->x_prefix = DEFAULT_X_PREFIX;
        }
    }
    if (!op->x_libdir) {
        /*
         * XXX 对于 X 库目录，跳过候选列表中的第一个条目（triplet 格式），
         * 因为在实际中未见过使用 triplet 路径（如 x86_64-linux-gnu）的
         * X 模块路径。如果使用它，可能会导致类似
         * /usr/lib/x86_64-linux-gnu/xorg/modules 这样不正确的路径。
         */
        op->x_libdir = find_libdir_and_fall_back(op, &native_libdirs[1],
                                                 op->x_prefix, ldconfig_cache,
                                                 "X library");
    }

    nvfree(ldconfig_cache);

    /* 设置 X 模块目录 */
    if (!op->x_moddir) {
        if (op->modular_xorg) {
            op->x_moddir = XORG7_DEFAULT_X_MODULEDIR ;
        } else {
            op->x_moddir = DEFAULT_X_MODULEDIR;
        }
    }

    /* 32 位兼容库的前缀（仅 x86_64 架构） */
#if defined(NV_X86_64)
    if (!op->compat32_prefix)
        op->compat32_prefix = DEFAULT_OPENGL_PREFIX;
#endif

    /* ===== 工具文件路径 ===== */
    if (!op->utility_prefix)
        op->utility_prefix = DEFAULT_UTILITY_PREFIX;      /* 通常为 /usr */
    if (!op->utility_libdir)
        op->utility_libdir = default_libdir;               /* 与 OpenGL 库使用相同目录 */
    if (!op->utility_bindir)
        op->utility_bindir = DEFAULT_BINDIR;               /* 通常为 bin */

    /* ===== 桌面数据和图标路径 ===== */
    if (!op->xdg_data_dir)
        op->xdg_data_dir = nvstrcat(op->utility_prefix, "/",
                                    DEFAULT_XDG_DATA_DIR, NULL);
    op->icon_dir = nvstrcat(op->xdg_data_dir, "/icons/hicolor", NULL);

    /* ===== 文档路径 ===== */
    if (!op->documentation_prefix)
        op->documentation_prefix = DEFAULT_DOCUMENTATION_PREFIX;
    if (!op->documentation_docdir)
        op->documentation_docdir = DEFAULT_DOCDIR;
    if (!op->documentation_mandir)
        op->documentation_mandir = DEFAULT_MANDIR;

    /* ===== 应用程序配置文件路径 ===== */
    if (!op->application_profile_path)
        op->application_profile_path = DEFAULT_APPLICATION_PROFILE_PATH;

    /* ===== 内核模块源码路径 ===== */
    if (!op->kernel_module_src_prefix)
        op->kernel_module_src_prefix = DEFAULT_KERNEL_MODULE_SRC_PREFIX;

    /* ===== 模块签名密钥路径 ===== */
    if (!op->module_signing_key_path)
        op->module_signing_key_path = DEFAULT_MODULE_SIGNING_KEY_PATH;
    /* 注意：kernel_module_src_dir 的默认值在 set_destinations() 中设置 */

    /* ===== systemd 相关路径 ===== */
    if (!op->systemd_unit_prefix)
        op->systemd_unit_prefix = DEFAULT_SYSTEMD_UNIT_PREFIX;
    if (!op->systemd_sleep_prefix)
        op->systemd_sleep_prefix = DEFAULT_SYSTEMD_SLEEP_PREFIX;
    if (!op->systemd_sysconf_prefix)
        op->systemd_sysconf_prefix = DEFAULT_SYSTEMD_SYSCONF_PREFIX;

    /* ===== Wine 兼容库路径 ===== */
    if (!op->wine_prefix)
        op->wine_prefix = DEFAULT_WINE_PREFIX;
    if (!op->wine_libdir)
    {
        /* Wine 库目录通常在 OpenGL 库目录下的特定子目录 */
        op->wine_libdir = nvstrcat(op->opengl_libdir, "/",
                                   DEFAULT_WINE_LIBDIR_SUFFIX, NULL);
        collapse_multiple_slashes(op->wine_libdir);
    }

} /* get_default_prefixes_and_paths() */


#if defined NV_X86_64
/*
 * compat32_conflict() - 检查 32 位兼容库目录与原生库目录是否存在冲突。
 *
 * 当 32 位兼容库目录是原生 64 位库目录的父目录时，会产生冲突。
 * 例如：
 *   - 原生路径: /usr/lib/x86_64-linux-gnu
 *   - 兼容路径: /usr/lib  <-- 这会冲突！因为 /usr/lib 是原生路径的父目录
 *   - 兼容路径: /usr/lib/i386-linux-gnu  <-- 这不冲突
 *
 * 比较是单向的：只检查原生目录是否是兼容目录的子目录。
 *
 * 参数：
 *   op            - 全局选项结构体
 *   compat_libdir - 提议的 32 位兼容库目录名
 *
 * XXX 假设原生目录永远不会是兼容目录的子目录，但这可能不总是成立。
 *
 * 返回值：存在冲突返回 TRUE（非零），不冲突返回 FALSE
 */
static int compat32_conflict(Options *op, const char *compat_libdir)
{
    char *native_path, *compat_path;
    int ret, is_subdir;

    /* 构造原生库的完整路径和 32 位兼容库的完整路径 */
    native_path = nvstrcat(op->opengl_prefix, "/", op->opengl_libdir, NULL);
    compat_path = nvstrcat(op->compat32_chroot ? op->compat32_chroot : "",
                           "/", op->compat32_prefix, "/", compat_libdir, NULL);

    /* 检查原生路径是否是兼容路径的子目录（如果是，说明冲突） */
    ret = is_subdirectory(compat_path, native_path, &is_subdir);

    if (!ret) {
        /* 检查失败，保守处理：视为冲突 */
        ui_error(op, "Failed to determine whether '%s' is a subdirectory of "
                 "'%s'; '%s' will not be considered as a candidate location "
                 "for installing 32-bit compatibility libraries.",
                 native_path, compat_path, compat_path);
        is_subdir = TRUE;
    }

    nvfree(native_path);
    nvfree(compat_path);

    return is_subdir;
}
#endif


/*
 * get_compat32_path() - 检测 32 位兼容库的安装路径。
 *
 * 此函数必须在 parse_manifest() 之后、set_destinations() 之前调用。
 * 仅在 x86_64 架构上有效。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 搜索策略（按优先级递减）：
 *   1. 在 ldconfig 缓存中搜索不与原生路径冲突的 32 位库目录
 *   2. 在文件系统上搜索
 *   3. 尝试旧版 Debian 的 32 位 chroot 路径（/emul/ia32-linux）
 *   4. 如果都失败，禁用 32 位兼容库安装并警告用户
 *
 * 同时设置 compat32_gbm_backend_dir（32 位 GBM 后端目录）。
 */
void get_compat32_path(Options *op)
{
#if defined(NV_X86_64)
    char *ldconfig_cache = get_ldconfig_cache(op);

    if (!op->compat32_prefix)
        op->compat32_prefix = DEFAULT_OPENGL_PREFIX;

    if (!op->compat32_libdir) {
        char *compat_libdir;

        /* 第一次尝试：在 ldconfig 缓存中搜索 */
        compat_libdir = find_libdir(compat_libdirs, op->compat32_prefix,
                                    ldconfig_cache, op->compat32_chroot);

        /* 如果未找到或与原生路径冲突，在文件系统上搜索 */
        if (!compat_libdir || compat32_conflict(op, compat_libdir)) {
            compat_libdir = find_libdir(compat_libdirs, op->compat32_prefix,
                                        NULL, op->compat32_chroot);
        }

        /*
         * 如果仍未找到合适的目录，且用户未指定显式的 chroot，
         * 尝试旧版 Debian 的 32 位 chroot 路径（/emul/ia32-linux）
         */
        if ((!compat_libdir || compat32_conflict(op, compat_libdir)) &&
            !op->compat32_chroot) {
            op->compat32_chroot = DEBIAN_DEFAULT_COMPAT32_CHROOT;

            compat_libdir = find_libdir(compat_libdirs, op->compat32_prefix,
                                        ldconfig_cache, op->compat32_chroot);

            if (!compat_libdir || compat32_conflict(op, compat_libdir)) {
                compat_libdir = find_libdir(compat_libdirs, op->compat32_prefix,
                                            NULL, op->compat32_chroot);
            }

            /*
             * 如果在旧版 Debian chroot 中仍然找不到合适的路径，
             * 重置 chroot 路径和检测到的目录
             */
            if (!compat_libdir || compat32_conflict(op, compat_libdir)) {
                op->compat32_chroot = NULL;
                compat_libdir = NULL;
            }
        }

        /* 如果所有尝试都失败了，禁用 32 位兼容库安装 */
        if (op->install_compat32_libs != NV_OPTIONAL_BOOL_FALSE &&
            (!compat_libdir || compat32_conflict(op, compat_libdir))) {
            ui_warn(op, "Unable to find a suitable destination to install "
                    "32-bit compatibility libraries. Your system may not "
                    "be set up for 32-bit compatibility. 32-bit "
                    "compatibility files will not be installed; if you "
                    "wish to install them, re-run the installation and set "
                    "a valid directory with the --compat32-libdir option.");
            op->install_compat32_libs = NV_OPTIONAL_BOOL_FALSE;
        }

        if (op->install_compat32_libs != NV_OPTIONAL_BOOL_FALSE) {
            op->compat32_libdir = compat_libdir;
        }
    }

    /* 设置 32 位 GBM 后端库目录 */
    if (!op->compat32_gbm_backend_dir)
        op->compat32_gbm_backend_dir =
            nvstrcat(op->compat32_libdir, "/", "gbm", NULL);

    nvfree(ldconfig_cache);
#endif
}


/*
 * extract_x_path() - 从逗号分隔的目录列表中提取下一个目录路径。
 *
 * 这是一个类似于 strtok() 的迭代器函数，用于解析 X.Org 服务器返回的
 * 逗号分隔路径列表。
 *
 * 参数：
 *   str  - 逗号分隔的目录列表字符串
 *   next - 输入/输出参数：指向上次调用后应继续搜索的位置
 *
 * 用法：
 *   char *next = NULL;
 *   char *dir = extract_x_path(str, &next);
 *   while (dir) {
 *       // 处理 dir
 *       dir = extract_x_path(str, &next);
 *   }
 *
 * 注意：此函数会修改原始字符串，将逗号替换为 NULL 终止符。
 *
 * 返回值：下一个目录路径，或 NULL（列表已耗尽）
 */

static char *extract_x_path(char *str, char **next)
{
    char *start;
    
    /*
     * 确定从字符串的哪里开始：第一次调用从字符串开头开始，
     * 后续调用从上次找到逗号的位置之后开始
     */

    start = str;

    if (*next) start = *next;

    /* 跳过开头的所有逗号 */

    while (*start == ',') start++;

    /* 如果已到达字符串末尾，返回 NULL 表示没有更多路径 */

    if (*start == '\0') return NULL;

    /*
     * 在字符串中查找下一个逗号：
     * - 如果找到逗号，将其替换为 NULL 终止符，并将 'next' 指向逗号后面
     * - 如果没有找到逗号，将 'next' 指向字符串末尾，
     *   这样下次调用时会立即返回 NULL
     */

    *next = strchr(start, ',');

    if (*next) {
        **next = '\0';    /* 将逗号替换为 NULL 终止符 */
        (*next)++;        /* 'next' 指向逗号后面的字符 */
    } else {
        *next = strchr(start, '\0');  /* 'next' 指向字符串末尾 */
    }

    return start;

} /* extract_x_path() */


/*
 * XPathType - X.Org 路径类型枚举。
 * 用于 get_x_paths_helper() 中区分正在检测的路径类型。
 */
enum XPathType {
    XPathLibrary,    /* X 库路径 */
    XPathModule,     /* X 模块路径 */
    XPathSysConfig   /* X 系统配置路径（xorg.conf.d） */
};

/*
 * get_x_paths_helper() - 用于检测 X.Org 库、模块和系统配置路径的辅助函数。
 *
 * 参数：
 *   op                         - 全局选项结构体
 *   pathType                   - 路径类型（库/模块/配置）
 *   xserver_cmd                - X 服务器查询参数（如 "-showDefaultLibPath"）
 *   pkg_config_cmd             - pkg-config 查询命令
 *   name                       - 路径类型的人类可读名称（用于日志/警告消息）
 *   path                       - 输出参数：检测到的路径
 *   require_existing_directory  - 是否要求目录在文件系统上实际存在
 *
 * 查询机制（按优先级）：
 *   1. X 服务器命令行选项（推荐，X.Org 7.2+ 支持）
 *      例如：Xorg -showDefaultLibPath
 *   2. pkg-config（X.Org 7.0 至 7.2 之间的推荐方式）
 *      例如：pkg-config --variable=libdir xorg-server
 *   3. 根据前缀手动构造路径（回退方案）
 *
 * 返回值：如果不得不猜测路径（即前两种机制都未成功），返回 TRUE；
 *         否则返回 FALSE
 */

static int get_x_paths_helper(Options *op,
                              enum XPathType pathType,
                              char *xserver_cmd,
                              char *pkg_config_cmd,
                              char *name,
                              char **path,
                              int require_existing_directory)
{
    char *dirs, *dir, *next;
    int ret, guessed = 0;    /* guessed: 是否不得不猜测路径 */

    /*
     * 如果路径已通过命令行选项指定，无需自动检测
     */

    if (*path != NULL) {
        return FALSE;
    }

    /*
     * 通过各种查询机制尝试自动检测路径
     */

    /*
     * 优先级 1：使用 X 服务器命令行选项查询。
     * 这是 X.Org 7.2 及以后版本推荐的查询机制。
     * 例如：Xorg -showDefaultLibPath 返回 "/usr/lib/x86_64-linux-gnu"
     */
    if (op->utils[XSERVER] && xserver_cmd) {

        dirs = NULL;
        ret = run_command(op, &dirs, FALSE, NULL, TRUE,
                          op->utils[XSERVER], " ", xserver_cmd, NULL);

        if ((ret == 0) && dirs) {
            
            next = NULL;

            dir = extract_x_path(dirs, &next);
            
            while (dir) {
                
                if (!require_existing_directory || directory_exists(dir)) {

                    ui_expert(op, "X %s path '%s' determined from `%s %s`",
                              name, dir, op->utils[XSERVER], xserver_cmd);
                    
                    *path = nvstrdup(dir);
                    
                    nvfree(dirs);
                    
                    return FALSE;
                    
                } else {
                    ui_warn(op, "You appear to be using a modular X.Org "
                            "release, but the X %s installation "
                            "path, '%s', reported by `%s %s` does not exist.  "
                            "Please check your X.Org installation.",
                            name, dir, op->utils[XSERVER], xserver_cmd);
                }

                dir = extract_x_path(dirs, &next);
            }
        }

        nvfree(dirs);
    }

    /*
     * 优先级 2：使用 pkg-config 查询。
     * 这是 X.Org 7.0 至 X.Org 7.2 之间的半推荐查询机制。
     * 例如：pkg-config --variable=libdir xorg-server
     */
    if (op->utils[PKG_CONFIG]) {

        dirs = NULL;
        ret = run_command(op, &dirs, FALSE, NULL, TRUE,
                          op->utils[PKG_CONFIG], " ", pkg_config_cmd, NULL);

        if ((ret == 0) && dirs) {

            next = NULL;
            
            dir = extract_x_path(dirs, &next);
 
            while (dir) {
                
                if (!require_existing_directory || directory_exists(dir)) {

                    ui_expert(op, "X %s path '%s' determined from `%s %s`",
                              name, dir, op->utils[PKG_CONFIG],
                              pkg_config_cmd);

                    *path = nvstrdup(dir);
                    
                    nvfree(dirs);
                    
                    return FALSE;
                
                } else {
                    ui_warn(op, "You appear to be using a modular X.Org "
                            "release, but the X %s installation "
                            "path, '%s', reported by `%s %s` does not exist.  "
                            "Please check your X.Org installation.",
                            name, dir, op->utils[PKG_CONFIG], pkg_config_cmd);
                }

                dir = extract_x_path(dirs, &next);
            }
        }

        nvfree(dirs);
    }

    /*
     * 优先级 3（回退方案）：上述查询机制都未能得到可用路径，
     * 手动根据前缀构造路径。
     *
     * 如果是模块化 X 服务器（X.Org 7.x），标记为"猜测"以便后续打印警告。
     * 对于非模块化 X（传统 XFree86），/usr/X11R6/lib 是标准路径，无需警告。
     */

    if (op->modular_xorg)
        guessed = TRUE;

    /* 根据路径类型手动构造路径 */

    switch (pathType) {
        case XPathLibrary:
            /* 库路径 = x_prefix/x_libdir，如 /usr/lib */
            *path = nvstrcat(op->x_prefix, "/", op->x_libdir, NULL);
            break;

        case XPathModule:
            /* 模块路径 = x_library_path/x_moddir，如 /usr/lib/xorg/modules */
            *path = nvstrcat(op->x_library_path, "/", op->x_moddir, NULL);
            break;

        case XPathSysConfig:
            /* 系统配置路径，如 /usr/share/X11/xorg.conf.d */
            *path = nvstrcat(DEFAULT_X_DATAROOT_PATH, "/", DEFAULT_CONFDIR, NULL);
            break;
    }

    remove_trailing_slashes(*path);
    collapse_multiple_slashes(*path);

    return guessed;
}


/*
 * get_x_library_and_module_paths() - 确定并设置 X.Org 库路径和模块路径。
 *
 * 此函数设置三个路径：
 *   1. op->x_library_path: X 库路径（如 /usr/lib/x86_64-linux-gnu）
 *   2. op->x_module_path: X 模块路径（如 /usr/lib/xorg/modules）
 *   3. op->x_sysconfig_path: X 系统配置路径（如 /usr/share/X11/xorg.conf.d）
 *
 * 注意：模块路径的检测依赖于库路径，因此必须先确定库路径。
 *
 * 如果某个路径不得不通过猜测得到（而非通过 X 服务器或 pkg-config 查询），
 * 会打印警告信息，建议用户安装 pkg-config 和 X.Org SDK。
 *
 * 此函数不会失败（即使猜测也会给出一个路径值）。
 *
 * 参数：
 *   op - 全局选项结构体
 */

static void get_x_library_and_module_paths(Options *op)
{
    int guessed = FALSE;   /* 是否有路径是通过猜测得到的 */

    /*
     * 先获取库路径，再获取模块路径。
     * 注意：模块路径的构造依赖于已确定的库路径。
     */

    /* 检测 X 库路径 */
    guessed |= get_x_paths_helper(op,
                                  XPathLibrary,
                                  "-showDefaultLibPath",
                                  "--variable=libdir xorg-server",
                                  "library",
                                  &op->x_library_path,
                                  TRUE);
    
    /* 检测 X 模块路径 */
    guessed |= get_x_paths_helper(op,
                                  XPathModule,
                                  "-showDefaultModulePath",
                                  "--variable=moduledir xorg-server",
                                  "module",
                                  &op->x_module_path,
                                  TRUE);

    /*
     * 检测系统配置路径（通常为 /usr/share/X11/xorg.conf.d）。
     * 仅在需要安装 nvidia.conf OutputClass 配置文件片段时才需要。
     * 如果不得不猜测该路径，不打印警告；因为即使没有它，
     * X 服务器仍可通过 xorg.conf 正常工作。
     */
    get_x_paths_helper(op,
                       XPathSysConfig,
                       NULL,
                       "--variable=sysconfigdir xorg-server",
                       "sysconfig",
                       &op->x_sysconfig_path,
                       FALSE);

    /*
     * 路径设置完成。如果有任何路径是通过猜测得到的，
     * 打印警告建议用户安装 pkg-config 和 X.Org SDK
     */

    if (guessed) {
        ui_warn(op, "nvidia-installer was forced to guess the X library "
                "path '%s' and X module path '%s'; these paths were not "
                "queryable from the system.  If X fails to find the "
                "NVIDIA X driver module, please install the `pkg-config` "
                "utility and the X.Org SDK/development package for your "
                "distribution and reinstall the driver.",
                op->x_library_path, op->x_module_path);
    }
    
} /* get_x_library_and_module_paths() */



/*
 * get_filename() - 提示用户输入文件路径，并验证该路径指向有效的文件。
 *
 * 参数：
 *   op  - 全局选项结构体
 *   def - 默认文件路径（作为用户输入的默认值）
 *   msg - 提示消息
 *
 * 如果用户输入的路径不存在或不是普通文件/符号链接，会重复提示直到
 * 用户输入有效路径。这是 ui_get_input() 的简单封装。
 *
 * 注意：如果 op->no_questions 为 TRUE（非交互模式），直接返回默认值，
 * 避免无限循环。
 *
 * 返回值：用户输入的有效文件路径（新分配的字符串）
 */
char *get_filename(Options *op, const char *def, const char *msg)
{
    char *oldfile = nvstrdup(def);

    /* XXX This function should never be called if op->no_questions is set,
     * but just in case that happens by accident, do something besides looping
     * infinitely if def is a filename that doesn't exist. */
    if (op->no_questions) {
        return oldfile;
    }

    while (TRUE) {
        struct stat stat_buf;
        char *file = ui_get_input(op, oldfile, "%s", msg);

        nvfree(oldfile);

        if (file && stat(file, &stat_buf) != -1 &&
           (S_ISREG(stat_buf.st_mode) || S_ISLNK(stat_buf.st_mode))) {
            return file;
        }

        ui_message(op, "File \"%s\" does not exist, or is not a regular "
                   "file. Please enter another filename.", file ?
                   file : "(null)");

        oldfile = file;
    }
}



/*
 * secure_delete() - 安全删除文件，使数据难以恢复。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   file - 要安全删除的文件路径
 *
 * 使用 `shred -u` 命令覆写文件内容后删除。如果系统上没有安装 shred，
 * 回退为普通的 unlink 删除，但会打印警告（文件内容可能仍可恢复）。
 *
 * 用途：主要用于删除内核模块签名密钥等敏感文件。
 *
 * 返回值：安全删除成功返回 TRUE，使用普通删除或失败返回 FALSE
 */
int secure_delete(Options *op, const char *file)
{
    char *cmd;

    cmd = find_system_util("shred");

    if (cmd) {
        int ret;
        char *cmdline = nvstrcat(cmd, " -u \"", file, "\"", NULL);

        ret = run_command(op, NULL, FALSE, NULL, TRUE, cmdline, NULL);
        log_printf(op, NULL, "%s: %s", cmdline, ret == 0 ? "" : "failed!");

        nvfree(cmd);
        nvfree(cmdline);

        return ret == 0;
    } else {
        ui_warn(op, "`shred` was not found on the system. The file %s will "
                "be deleted, but not securely. It may be possible to recover "
                "the file after deletion.", file);
        unlink(file);
        return FALSE;
    }
} /* secure_delete() */


/*
 * invalidate_package_entry() - 使包条目无效（从安装列表中逻辑删除）。
 *
 * 参数：
 *   entry - 要无效化的包条目
 *
 * 将条目的类型设置为 FILE_TYPE_NONE，清除目标路径和能力标志。
 * 安装过程中会跳过类型为 FILE_TYPE_NONE 的条目。
 *
 * XXX 注意：不尝试释放 dst 字符串的内存。这是为了避免在某些
 * Slackware 10.0 安装上观察到的崩溃（该问题一直未能复现和定位根因）。
 */

void invalidate_package_entry(PackageEntry *entry)
{
    entry->type = FILE_TYPE_NONE;     /* 标记为无效类型 */
    entry->dst = NULL;                 /* 清除目标路径（不 free，见上述注释） */
    memset(&(entry->caps), 0, sizeof(entry->caps));  /* 清除能力标志 */
}


/*
 * is_subdirectory() - 检测 subdir 是否是 dir 的子目录。
 *
 * 参数：
 *   dir       - 父目录路径
 *   subdir    - 要检查的子目录路径
 *   is_subdir - 输出参数：如果 subdir 是 dir 的子目录则设为 TRUE
 *
 * 检测方法（两步）：
 *   1. inode 比较法：从 subdir 开始，逐层向上遍历（追加 "/.."),
 *      在每一层检查设备号和 inode 号是否与 dir 匹配
 *   2. 字符串比较法：如果方法 1 未找到匹配（可能因为 subdir 是指向
 *      dir 外部的符号链接），则对规范化路径进行字符串前缀比较
 *
 * 返回值：
 *   TRUE  - 检测成功（*is_subdir 包含结果）
 *   FALSE - 检测过程中出错（stat 失败等）
 */

int is_subdirectory(const char *dir, const char *subdir, int *is_subdir)
{
    struct stat root_st, dir_st;

    /* 获取根目录和 dir 的 inode 信息 */
    if (stat("/", &root_st) != 0 || stat(dir, &dir_st) != 0) {
        return FALSE;
    } else {
        struct stat testdir_st;
        char *testdir = nvstrdup(subdir);

        *is_subdir = FALSE;

        /*
         * 方法 1：从 subdir 开始，逐层向上遍历目录树。
         * 在每一层比较设备号和 inode 号是否与 dir 匹配。
         * 直到到达根目录（"/"）为止。
         */
        do {
            char *oldtestdir;

            if (stat(testdir, &testdir_st) != 0) {
                nvfree(testdir);
                return FALSE;
            }

            /* 检查当前层级是否与 dir 是同一个目录 */
            if (testdir_st.st_dev == dir_st.st_dev &&
                testdir_st.st_ino == dir_st.st_ino) {
                *is_subdir = TRUE;
                break;
            }

            /* 向上移动一层 */
            oldtestdir = testdir;
            testdir = nvstrcat(oldtestdir, "/..", NULL);
            nvfree(oldtestdir);
        } while (testdir_st.st_dev != root_st.st_dev ||
                 testdir_st.st_ino != root_st.st_ino);

        nvfree(testdir);
    }

    /*
     * 方法 2：如果 inode 比较法未找到匹配，使用字符串前缀比较。
     * 这是为了处理 subdir 是符号链接的情况（链接位于 dir 内部，
     * 但链接目标在 dir 外部）。
     */
    if (!*is_subdir) {
        char *dir_with_slash, *subdir_with_slash;

        /* 确保路径以 "/" 结尾，避免 "/usr/lib" 错误匹配 "/usr/lib64" */
        dir_with_slash = nvstrcat(dir, "/", NULL);
        collapse_multiple_slashes(dir_with_slash);
        subdir_with_slash = nvstrcat(subdir, "/", NULL);
        collapse_multiple_slashes(subdir_with_slash);

        /* 检查 dir（含尾斜杠）是否是 subdir（含尾斜杠）的前缀 */
        *is_subdir = (strncmp(dir_with_slash, subdir_with_slash,
                              strlen(dir_with_slash)) == 0);

        nvfree(dir_with_slash);
        nvfree(subdir_with_slash);
    }

    return TRUE;
}

/*
 * directory_equals() - 判断两个目录路径是否指向同一个目录。
 *
 * 判断逻辑：如果 a 是 b 的子目录，同时 b 也是 a 的子目录，
 * 那么 a 和 b 必定是同一个目录。
 *
 * 使用 is_subdirectory() 进行检查，利用其基于 inode 的比较，
 * 可以正确处理符号链接的情况。
 *
 * 参数：
 *   a - 第一个目录路径
 *   b - 第二个目录路径
 *
 * 返回值：是同一目录返回 TRUE（非零），否则返回 FALSE
 */
static int directory_equals(const char *a, const char *b)
{
    int a_b, b_a;

    if (is_subdirectory(a, b, &a_b) && is_subdirectory(b, a, &b_a)) {
        return a_b && b_a;
    }

    return FALSE;
}

/*
 * get_opengl_libdir() - 获取 OpenGL 库的安装目录完整路径。
 *
 * 返回值：新分配的路径字符串（如 "/usr/lib64/"），调用者需要释放
 */
static char *get_opengl_libdir(const Options *op)
{
    return nvstrcat(op->opengl_prefix, "/", op->opengl_libdir, "/", NULL);
}

/*
 * get_compat32_libdir() - 获取 32 位兼容库的安装目录完整路径。
 *
 * 仅在 x86_64 架构上有意义。在其他架构上返回 NULL。
 *
 * 返回值：新分配的路径字符串（如 "/usr/lib32/"），或 NULL（非 x86_64 架构）
 */
static char *get_compat32_libdir(const Options *op)
{
#if defined NV_X86_64
    return nvstrcat(op->compat32_chroot ? op->compat32_chroot : "", "/",
                    op->compat32_prefix, "/", op->compat32_libdir, "/", NULL);
#else
    return NULL;
#endif
}

/*
 * add_libgl_abi_symlink() - 添加符合 OpenGL ABI 规范的 libGL.so.1 符号链接。
 *
 * OpenGL ABI 规范要求 libGL.so.1 必须存在于 /usr/lib/ 目录下。
 * 如果原生库和 32 位兼容库都没有安装到 /usr/lib/，则需要在 /usr/lib/
 * 下创建一个符号链接指向实际安装位置的 libGL.so.1。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 注意：此函数必须在 set_destinations() 之后调用，因为它使用了硬编码的
 * "/usr/lib" 目标路径，如果在 set_destinations() 之前调用会被覆盖。
 */
void add_libgl_abi_symlink(Options *op, Package *p)
{
    static const char *usrlib = "/usr/lib/";
    char *libgl = nvstrdup("libGL.so.1");
    char *opengl_path = get_opengl_libdir(op);
    char *opengl32_path = get_compat32_libdir(op);

    /*
     * 如果 /usr/lib/ 既不是原生库目录也不是 32 位兼容库目录，
     * 则需要在 /usr/lib/ 下创建指向原生 libGL.so.1 的符号链接
     */
    if (!directory_equals(usrlib, opengl_path)
#if defined (NV_X86_64)
        && !directory_equals(usrlib, opengl32_path)
#endif
       ) {
        /* 创建 /usr/lib/libGL.so.1 -> <opengl_path>/libGL.so.1 符号链接 */
        char *target = nvstrcat(opengl_path, "libGL.so.1", NULL);
        add_package_entry(p,
                          libgl,
                          NULL,
                          libgl,
                          target,
                          nvstrcat(usrlib, libgl, NULL),
                          FILE_TYPE_OPENGL_SYMLINK,
                          FILE_COMPAT_ARCH_NATIVE,
                          0000);
    } else {
        /* 库已在 /usr/lib/ 中，不需要额外的符号链接 */
        nvfree(libgl);
    }

    nvfree(opengl_path);
    nvfree(opengl32_path);
}

/*
 * LibglvndInstallCheckResult - libglvnd 库安装状态检查结果枚举。
 *
 * libglvnd（GL Vendor-Neutral Dispatch）是一个厂商中立的 OpenGL 调度库，
 * 允许多个 GPU 厂商的驱动在同一系统上共存。
 */
typedef enum {
    LIBGLVND_CHECK_RESULT_INSTALLED = 0,      /* 已完整安装 */
    LIBGLVND_CHECK_RESULT_NOT_INSTALLED = 1,  /* 未安装 */
    LIBGLVND_CHECK_RESULT_PARTIAL = 2,        /* 部分安装 */
    LIBGLVND_CHECK_RESULT_ERROR = 3,          /* 检查过程中发生错误 */
} LibglvndInstallCheckResult;

/*
 * run_libglvnd_script() - 查找并运行 libglvnd 安装检查脚本。
 *
 * 运行一个辅助脚本来判断系统上 libglvnd 库的安装状态：
 * 已安装、未安装还是部分安装。
 *
 * 参数：
 *   op           - 全局选项结构体
 *   p            - 安装包结构体
 *   missing_libs - 输出参数：如果是部分安装，包含缺失的库列表
 *
 * 如果安装包中不包含检查脚本，默认返回 NOT_INSTALLED。
 *
 * 返回值：LibglvndInstallCheckResult 枚举值
 */
static LibglvndInstallCheckResult run_libglvnd_script(Options *op, Package *p,
                                                      char **missing_libs)
{
    const char *scriptPath = "./libglvnd_install_checker/check-libglvnd-install.sh";
    char *output = NULL;
    int status;
    LibglvndInstallCheckResult result = LIBGLVND_CHECK_RESULT_ERROR;

    if (missing_libs) {
        *missing_libs = "";
    }

    /* 检查安装包中是否包含 libglvnd 检查脚本 */
    log_printf(op, NULL, "Looking for install checker script at %s", scriptPath);
    if (access(scriptPath, R_OK) != 0) {
        /* 脚本不存在，假设 libglvnd 未安装 */
        log_printf(op, NULL, "No libglvnd install checker script, assuming not installed.");
        result = LIBGLVND_CHECK_RESULT_NOT_INSTALLED;
        goto done;
    }

    /* 运行检查脚本，通过退出码获取安装状态 */
    status = run_command(op, &output, TRUE, NULL, FALSE,
                         "/bin/sh ", scriptPath, NULL);
    if (WIFEXITED(status)) {
        result = WEXITSTATUS(status);  /* 退出码即为检查结果 */
    } else {
        result = LIBGLVND_CHECK_RESULT_ERROR;
    }

    /*
     * 如果检测到部分安装，从脚本输出中解析缺失的库列表。
     * 脚本输出格式为："Missing libglvnd libraries: libGL.so libEGL.so ..."
     */
    if (result == LIBGLVND_CHECK_RESULT_PARTIAL && missing_libs) {
        char *line, *end, *buf;
        int len = strlen(output);
        static const char *missing_label = "Missing libglvnd libraries: ";

        /* 逐行扫描脚本输出，查找缺失库列表 */
        for (buf = output;
             (line = get_next_line(buf, &end, output, len));
             buf = end) {
            int found = FALSE;

            if (strncmp(line, missing_label, strlen(missing_label)) == 0) {
                *missing_libs = nvstrdup(line + strlen(missing_label));
                found = TRUE;
            }

            free(line);

            if (found) {
                break;
            }
        }
    }

done:
    nvfree(output);
    return result;
}

/*
 * set_libglvnd_egl_json_path() - 确定 libglvnd EGL 厂商库配置 JSON 文件
 * 的安装路径。
 *
 * 这仅在使用系统现有的 libglvnd 库（而非安装自带版本）时需要。
 * 如果安装自带的 libglvnd 库，已知 libEGL 期望的配置路径。
 *
 * 通过 pkg-config 查询 libglvnd 的 datadir 变量来确定路径。
 * 如果查询失败，使用默认路径并警告用户。
 *
 * 参数：
 *   op - 全局选项结构体
 */
static void set_libglvnd_egl_json_path(Options *op)
{
    if (op->libglvnd_json_path == NULL) {
        char *path = get_pkg_config_variable(op, "libglvnd", "datadir");
        if (path != NULL) {
            op->libglvnd_json_path = nvstrcat(path, "/glvnd/egl_vendor.d", NULL);
            collapse_multiple_slashes(op->libglvnd_json_path);
            nvfree(path);
        }
    }

    if (op->libglvnd_json_path == NULL) {
        ui_warn(op, "Unable to determine the path to install the "
                "libglvnd EGL vendor library config files. Check that "
                "you have pkg-config and the libglvnd development "
                "libraries installed, or specify a path with "
                "--glvnd-egl-config-path.");
        op->libglvnd_json_path = nvstrdup(DEFAULT_GLVND_EGL_JSON_PATH);
    }
}

/*
 * library_is_optional() - 判断给定的库是否是 libglvnd 的可选组件。
 *
 * 参数：
 *   library - 库文件名（如 "libOpenGL.so.0"）
 *
 * 当前，libOpenGL.so 被视为可选组件，因为传统的 GLX/EGL ABI
 * 不需要它。
 *
 * 返回值：可选库返回 TRUE，必需库返回 FALSE
 */
static int library_is_optional(const char *library)
{
    /* 可选库列表 */
    const char * const optional_libs[] = {
        "libOpenGL.so", /* libOpenGL.so 对传统 GLX/EGL ABI 不是必需的 */
    };
    int i;

    for (i = 0; i < ARRAY_LEN(optional_libs); i++) {
        /* 使用部分匹配，允许匹配带版本号和不带版本号的 SONAME */
        if (strncmp(library, optional_libs[i], strlen(optional_libs[i])) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * list_includes_essential_library() - 检查以空格分隔的库列表中是否包含
 * libglvnd 堆栈中的必需（非可选）库。
 *
 * 参数：
 *   libraries - 以空格分隔的库名列表
 *
 * 返回值：列表中包含必需库返回 TRUE，仅包含可选库返回 FALSE
 */
static int list_includes_essential_library(const char *libraries)
{
    int essential_library_found = FALSE;
    char *libs = nvstrdup(libraries);
    char *lib;

    for (lib = strtok(libs, " "); lib; lib = strtok(NULL, " ")) {
        if (!library_is_optional(lib)) {
            essential_library_found = TRUE;
            break;
        }
    }

    nvfree(libs);
    return essential_library_found;
}

/*
 * check_libglvnd_files() - 检查是否应该安装 libglvnd 库，并据此更新包条目。
 *
 * libglvnd（GL Vendor-Neutral Dispatch）的安装策略：
 *   - 如果系统已安装 libglvnd，保持不动（不覆盖）
 *   - 如果系统未安装 libglvnd，安装 NVIDIA 自带的副本
 *   - 如果部分安装，询问用户如何处理
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 处理流程：
 *   1. 检查 libGLX_indirect.so.0 符号链接
 *   2. 检查包中是否有 libglvnd 文件
 *   3. 通过检查脚本判断系统上 libglvnd 的安装状态
 *   4. 根据状态决定：
 *      - 已安装：从包中移除 GLVND/GLX/EGL 客户端库
 *      - 未安装：保留所有 libglvnd 文件
 *      - 部分安装：询问用户是覆盖、跳过还是中止
 *   5. 设置 EGL ICD JSON 文件的安装路径
 *
 * 注意：只检查原生库的安装状态，结果同时应用于原生和 32 位兼容库。
 * 因为冲突文件列表无法区分同名的 32 位和 64 位文件。
 *
 * 返回值：成功返回 TRUE，用户中止或出错返回 FALSE
 */
int check_libglvnd_files(Options *op, Package *p)
{
    int shouldInstall = op->install_libglvnd_libraries;  /* 用户的安装意向 */
    int foundAnyFiles = FALSE;   /* 包中是否存在 libglvnd 文件 */
    int foundJSONFile = FALSE;   /* 包中是否存在 EGL ICD JSON 文件 */
    int i;

    /* 首先处理 libGLX_indirect.so.0 符号链接 */
    check_libGLX_indirect_links(op, p);

    /* 检查包中是否包含任何 libglvnd 相关文件 */
    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].type == FILE_TYPE_GLVND_LIB ||
            p->entries[i].type == FILE_TYPE_GLVND_SYMLINK) {
            foundAnyFiles = TRUE;
        }

        if (p->entries[i].type == FILE_TYPE_GLVND_EGL_ICD_JSON) {
            foundAnyFiles = TRUE;
            foundJSONFile = TRUE;
        }
    }
    /* 如果包中没有 libglvnd 文件，无需处理 */
    if (!foundAnyFiles) {
        return TRUE;
    }

    if (shouldInstall == NV_OPTIONAL_BOOL_DEFAULT) {
        char *missing_libs;

        /*
         * 用户未显式指定是否安装 libglvnd，需要自动检测系统状态。
         * 委托给检查脚本来判断 libglvnd 是否已安装。
         */

        LibglvndInstallCheckResult result = run_libglvnd_script(op, p, &missing_libs);
        if (result == LIBGLVND_CHECK_RESULT_INSTALLED) {
            /* libglvnd 已完整安装，不覆盖现有库 */
            shouldInstall = NV_OPTIONAL_BOOL_FALSE;
        } else if (result == LIBGLVND_CHECK_RESULT_NOT_INSTALLED) {
            /* libglvnd 未安装，安装 NVIDIA 自带的副本 */
            shouldInstall = NV_OPTIONAL_BOOL_TRUE;
        } else if (result == LIBGLVND_CHECK_RESULT_PARTIAL) {
            /* libglvnd 部分安装，需要询问用户如何处理 */
            static int partialAction = -1;  /* 缓存用户选择，避免重复询问 */
            if (partialAction < 0) {
                static const char *ANSWERS[] = {
                    "Don't install libglvnd files",
                    "Install and overwrite existing files",
                    "Abort installation."
                };

                int default_choice;
                const char *optional_only;

                /*
                 * 根据缺失库是否包含必需组件来决定默认选择：
                 * - 缺失必需库：默认中止安装（以免运行时出错）
                 * - 仅缺失可选库：默认跳过安装（系统仍可正常工作）
                 */
                if (list_includes_essential_library(missing_libs)) {
                    default_choice = 2;  /* 默认：中止安装 */
                    optional_only = "";
                } else {
                    default_choice = 0;  /* 默认：不安装 libglvnd */
                    optional_only = "All of the essential libglvnd libraries "
                                    "are present, but one or more optional "
                                    "components are missing. ";
                }

                partialAction = ui_multiple_choice(op, ANSWERS, 3, default_choice,
                        "An incomplete installation of libglvnd was found. %s"
                        "Do you want to install a full copy of libglvnd? "
                        "This will overwrite any existing libglvnd libraries.",
                        optional_only);
            }
            if (partialAction == 0) {
                /* 用户选择：不安装 libglvnd */
                shouldInstall = NV_OPTIONAL_BOOL_FALSE;
            } else if (partialAction == 1) {
                /* 用户选择：安装并覆盖现有文件 */
                shouldInstall = NV_OPTIONAL_BOOL_TRUE;
            } else {
                /* 用户选择：中止安装 */
                return FALSE;
            }
        } else {
            /* 检查脚本执行出错 */
            return FALSE;
        }
    }

    /* 健全性检查：到此处 shouldInstall 应该已经确定为 TRUE 或 FALSE */
    if (shouldInstall != NV_OPTIONAL_BOOL_FALSE && shouldInstall != NV_OPTIONAL_BOOL_TRUE) {
        ui_error(op, "Internal error: Could not determine whether to install libglvnd");
        return FALSE;
    }

    if (shouldInstall != NV_OPTIONAL_BOOL_TRUE) {
        /*
         * 不安装 libglvnd：从包中移除所有 GLVND 核心库、GLX 和 EGL 客户端库。
         * 这些库将由系统现有的 libglvnd 安装提供。
         */
        log_printf(op, NULL, "Will not install libglvnd libraries.");
        for (i = 0; i < p->num_entries; i++) {
            if (p->entries[i].type == FILE_TYPE_GLVND_LIB ||
                p->entries[i].type == FILE_TYPE_GLVND_SYMLINK ||
                p->entries[i].type == FILE_TYPE_GLX_CLIENT_LIB ||
                p->entries[i].type == FILE_TYPE_GLX_CLIENT_SYMLINK ||
                p->entries[i].type == FILE_TYPE_EGL_CLIENT_LIB ||
                p->entries[i].type == FILE_TYPE_EGL_CLIENT_SYMLINK) {
                ui_log(op, "Skipping GLVND file: \"%s\"", p->entries[i].file);
                invalidate_package_entry(&(p->entries[i]));
            }
        }

        /* 使用系统的 libglvnd，需要从 pkg-config 查询 JSON 路径 */
        if (foundJSONFile) {
            set_libglvnd_egl_json_path(op);
        }
    } else {
        /* 安装 NVIDIA 自带的 libglvnd 库 */
        log_printf(op, NULL, "Will install libglvnd libraries.");

        /* 使用默认的 JSON 路径 */
        if (foundJSONFile && op->libglvnd_json_path == NULL) {
            op->libglvnd_json_path = nvstrdup(DEFAULT_GLVND_EGL_JSON_PATH);
        }
    }

    /* 设置 EGL ICD JSON 文件的安装目标路径 */
    if (foundJSONFile) {
        log_printf(op, NULL,
                "Will install libEGL vendor library config file to %s",
                op->libglvnd_json_path);
        for (i = 0; i < p->num_entries; i++) {
            if (p->entries[i].type == FILE_TYPE_GLVND_EGL_ICD_JSON) {
                p->entries[i].dst = nvstrcat(op->libglvnd_json_path, "/", p->entries[i].name, NULL);
                collapse_multiple_slashes(p->entries[i].dst);
            }
        }
    }
    return TRUE;
}
