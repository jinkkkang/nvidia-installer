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
 * command_list.c - this source file contains functions for building
 * and executing a commandlist (the list of operations to perform to
 * actually do an install).
 *
 * 【文件说明】命令列表子系统实现文件。
 *
 * 本文件实现了 nvidia-installer 的"命令列表"（CommandList）模式，
 * 这是安装器执行安装操作的核心机制。整个安装流程分为三个阶段：
 *
 *   1. 构建阶段（build_command_list）：
 *      - 扫描文件系统，查找与待安装文件冲突的现有文件
 *      - 扫描内核模块目录，查找冲突的内核模块
 *      - 将所有操作（安装文件、创建符号链接、备份/删除冲突文件、
 *        运行后处理命令、调用函数等）编排为有序的命令列表
 *
 *   2. 审核阶段（可选）：
 *      - 命令列表中每条命令都有人类可读的描述文本
 *      - 可以通过 UI 让用户在执行前审核所有操作
 *
 *   3. 执行阶段（execute_command_list）：
 *      - 按顺序逐条执行命令列表中的操作
 *      - 显示进度条和状态信息
 *      - 任何操作失败时询问用户是否继续
 *
 * 命令类型包括：
 *   - INSTALL_CMD   ：安装文件到目标路径，设置权限，可选执行后处理命令
 *   - BACKUP_CMD    ：将冲突文件移动到备份目录
 *   - DELETE_CMD    ：删除冲突文件（当 --no-backup 时使用）
 *   - SYMLINK_CMD   ：创建符号链接
 *   - RUN_CMD       ：执行 shell 命令（如 ldconfig）
 *   - RUN_CMD_LONG  ：执行耗时较长的 shell 命令（显示不确定进度条）
 *   - TOUCH_CMD     ：更新文件的修改时间
 *   - FUNCTION_CMD  ：调用 C 函数指针（如重建 initramfs）
 *
 * 冲突文件搜索机制：
 *   - 使用 FTS（File Tree Walk）API 递归遍历文件系统
 *   - 搜索范围包括 X.Org 库目录、OpenGL 库目录、内核模块目录等
 *   - 支持跳过指定目录（如 "source"、"build"、"nvidia-cg-toolkit"）
 *   - 通过文件名前缀匹配和可选的文件内容字符串匹配来识别冲突文件
 */

/* ===== 系统头文件 ===== */
#include <sys/types.h>   /* mode_t, dev_t, ino_t 等 POSIX 类型 */
#include <sys/stat.h>    /* stat/lstat/fstat 及 struct stat */
#include <fcntl.h>       /* open() 及文件打开标志（O_RDONLY 等） */
#include <sys/mman.h>    /* mmap/munmap 内存映射 */
#include <fts.h>         /* fts_open/fts_read/fts_close 文件树遍历 API */
#include <unistd.h>      /* unlink/close 等 POSIX 函数 */
#include <dirent.h>      /* 目录操作（本文件中未直接使用，可能被间接头文件依赖） */
#include <string.h>      /* strlen/strncmp/strcmp/strstr/memset 等字符串函数 */
#include <stdlib.h>      /* malloc/free/realloc 等内存管理 */
#include <stdio.h>       /* snprintf/fprintf/fopen/fclose 等 I/O 函数 */
#include <stdarg.h>      /* va_list/va_start/va_arg/va_end 变参函数支持 */
#include <errno.h>       /* errno 及错误码（ENOENT 等） */
#include <utime.h>       /* utime() 修改文件时间戳 */

/* ===== 项目内部头文件 ===== */
#include "nvidia-installer.h"         /* 核心数据结构：Options, Package, PackageEntry 等 */
#include "command-list.h"             /* CommandList, FileList 结构体及本文件的公开函数声明 */
#include "user-interface.h"           /* UI 函数：ui_status_begin/update/end, ui_error, ui_expert 等 */
#include "backup.h"                   /* 备份函数：do_backup, log_install_file, log_create_symlink 等 */
#include "misc.h"                     /* 杂项工具：directory_exists, is_subdirectory, nvstrcat, nvalloc 等 */
#include "files.h"                    /* 文件操作：install_file, install_symlink, mode_to_permission_string 等 */
#include "kernel.h"                   /* 内核工具：get_kernel_name, dkms_module_installed 等 */
#include "manifest.h"                 /* 清单文件工具：get_installable_file_type_list 等 */
#include "conflicting-kernel-modules.h" /* 冲突内核模块列表：conflicting_kernel_modules 数组 */
#include "initramfs.h"                /* initramfs 重建：update_initramfs 函数 */


/*
 * 命令类型说明：
 *
 * INSTALL    - 将 'path' 指定的源文件安装到 'target' 路径，
 *              设置 'mode' 指定的权限，安装后可选执行 'command' 字符串
 *              作为后处理步骤（如 execstack 清除可执行栈标记）
 *
 * BACKUP     - 将 'path' 指定的文件移动到备份目录，
 *              并在备份日志中记录相关信息，以便卸载时还原
 *
 * RUN        - 执行 'command' 字符串指定的 shell 命令
 *
 * RUN_CMD_LONG - 执行 'command' 字符串指定的 shell 命令，
 *              并在执行期间显示不确定进度条（用于耗时较长的命令，如 depmod）
 *
 * SYMLINK    - 创建名为 'path' 的符号链接，指向 'target' 指定的文件
 *
 * DELETE     - 删除 'path' 指定的文件
 *
 * TOUCH_CMD  - 更新 'path' 指定路径的修改时间（mtime），
 *              用于触发桌面环境刷新图标缓存等
 *
 * FUNCTION_CMD - 调用 'function' 指针指向的函数，
 *              传入 Options 结构体指针作为参数
 */

/*
 * CommandID 枚举：定义命令列表中所有支持的命令类型。
 * INVALID_CMD 为默认值（值为 0），确保 nvalloc()/memset 初始化
 * 的 Command 结构体处于无效状态，避免意外执行。
 */
typedef enum {
    INVALID_CMD = 0, /* 无效命令，nvalloc() 或 memset(..., 0, ...) 后的默认值 */
    INSTALL_CMD,     /* 安装文件命令 */
    BACKUP_CMD,      /* 备份文件命令 */
    RUN_CMD,         /* 运行 shell 命令 */
    RUN_CMD_LONG,    /* 运行耗时命令（带不确定进度条） */
    SYMLINK_CMD,     /* 创建符号链接命令 */
    DELETE_CMD,      /* 删除文件命令 */
    TOUCH_CMD,       /* 更新文件时间戳命令 */
    FUNCTION_CMD,    /* 调用函数指针命令 */
} CommandID;


/*
 * CommandFunction：命令函数指针类型。
 * 用于 FUNCTION_CMD 命令类型，指向一个接受 Options* 参数并返回 int 的函数。
 * 返回值：成功返回 TRUE（非零），失败返回 FALSE（零）。
 * 目前用于调用 update_initramfs() 等需要在安装过程中执行的回调函数。
 */
typedef int (*CommandFunction)(Options *);


/*
 * Command 结构体：描述单条安装操作的数据类型。
 * 根据 cmd 字段的值，不同的字段可能被使用或未被设置：
 *   - INSTALL_CMD ：使用 path（源文件）、target（目标路径）、command（后处理命令）、mode（权限）
 *   - BACKUP_CMD  ：使用 path（待备份文件路径）
 *   - DELETE_CMD  ：使用 path（待删除文件路径）
 *   - TOUCH_CMD   ：使用 path（待更新时间戳的路径）
 *   - SYMLINK_CMD ：使用 path（链接名）、target（链接目标）
 *   - RUN_CMD / RUN_CMD_LONG ：使用 command（shell 命令字符串）
 *   - FUNCTION_CMD：使用 function（函数指针）
 */

typedef struct __command {
    CommandID cmd;             /* 命令类型标识 */
    char *path;                /* 源文件路径或操作目标路径 */
    char *target;              /* 安装目标路径或符号链接目标 */
    char *command;             /* 待执行的 shell 命令字符串 */
    mode_t mode;               /* 文件权限模式（仅 INSTALL_CMD 使用） */
    CommandFunction function;  /* 函数指针（仅 FUNCTION_CMD 使用） */
} Command;

/* ===== 前向声明：本文件内部使用的静态函数 ===== */

/* 释放 FileList 结构体及其包含的所有文件名字符串 */
static void free_file_list(FileList* l);

/* 搜索内核模块安装目录中与 NVIDIA 驱动冲突的内核模块 */
static void find_conflicting_kernel_modules(Options *op, FileList *l);

/* 检查 Package 中待安装文件是否已存在于目标路径，将已存在的添加到冲突列表 */
static void find_existing_files(Package *p, FileList *l,
                                PackageEntryFileTypeList *file_type_list);

/* 去除文件列表中的重复条目（通过 inode+device 比较去重） */
static void condense_file_list(Package *p, FileList *l);

/* 向命令列表追加一条命令（使用变参传递命令的各字段） */
static void add_command (CommandList *c, CommandID cmd, ...);

/* 向文件列表追加一个文件路径（可选拼接目录前缀） */
static void add_file_to_list(const char*, const char*, FileList*);

/* 将已安装文件的路径和权限追加到 RPM 文件列表（用于 RPM 包管理器集成） */
static void append_to_rpm_file_list(Options *op, Command *c);

/* 根据 Package 条目构建冲突文件信息数组（用于文件系统搜索时的匹配） */
static ConflictingFileInfo *build_conflicting_file_list(Options *op, Package *p);
/* 根据文件名填充 ConflictingFileInfo 结构体（计算匹配长度和所需字符串） */
static void get_conflicting_file_info(const char *file, ConflictingFileInfo *cfi);

/*
 * NoRecursionDirectory：目录递归控制结构。
 * find_conflicting_files() 接受一个此结构的数组，
 * 用于指定在递归搜索文件树时应跳过哪些目录。
 * 数组以 name == NULL 的条目作为终止标记。
 */

typedef struct {
    int level;   /* 最大搜索深度限制：负值表示不限制深度（任意深度都跳过） */
    char *name;  /* 要跳过的目录名：NULL 表示数组结束 */
} NoRecursionDirectory;

/*
 * find_conflicting_files() - 在指定路径下递归搜索与待安装文件冲突的现有文件。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   path     - 搜索起始路径
 *   files    - 冲突文件信息数组（以 name==NULL 结尾），定义了要搜索的文件名模式
 *   l        - 输出参数，找到的冲突文件路径追加到此列表
 *   skipdirs - 要跳过的目录数组（以 name==NULL 结尾），NULL 表示不跳过任何目录
 */
static void find_conflicting_files(Options *op,
                                   char *path,
                                   ConflictingFileInfo *files,
                                   FileList *l,
                                   const NoRecursionDirectory *skipdirs);


/*
 * path_already_exists() - 检查给定路径是否已被路径列表覆盖。
 *
 * 判断条件：path 是否是路径列表中某个已有路径的子目录
 * （或相同路径、或通过符号链接等价的路径）。
 * 这用于避免在搜索冲突文件时重复搜索已经被覆盖的目录。
 *
 * 参数：
 *   paths - 指向路径数组指针的指针（路径数组的间接引用）
 *   count - 路径数组中的路径数量
 *   path  - 要检查的路径
 *
 * 返回值：
 *   TRUE  - 该路径已被覆盖（是某个已有路径的子目录）
 *   FALSE - 该路径未被覆盖（需要添加到搜索列表）
 */
static int path_already_exists(char ***paths, int count, const char *path)
{
    int i;

    /* 遍历已有路径，检查新路径是否为其子目录 */
    for (i = 0; i < count; i++) {
        int is_subdir = FALSE;

        /* is_subdirectory() 会同时处理符号链接解析 */
        is_subdirectory((*paths)[i], path, &is_subdir);

        if (is_subdir) {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * add_search_path() - 将一个新路径添加到搜索路径列表。
 *
 * 仅在以下条件都满足时才添加：
 *   1. 该路径在文件系统上确实存在（directory_exists 检查）
 *   2. 该路径不是已有搜索路径的子目录（避免重复搜索）
 *
 * XXX 已知限制：仅检查新目录是否为已有目录的子目录，
 *     不检查已有目录是否为新目录的子目录。
 *     即：如果先添加了 /usr/lib，再添加 /usr，
 *     /usr 仍会被添加，但 /usr/lib 不会被移除。
 *
 * 参数：
 *   paths - 指向搜索路径数组指针的指针（动态增长数组）
 *   count - 指向路径计数器的指针
 *   path  - 要添加的路径
 */
static void add_search_path(char ***paths, int *count, const char *path)
{
    if (directory_exists(path) && !path_already_exists(paths, *count, path)) {
        /* 动态扩展数组，追加新路径的副本 */
        *paths = nvrealloc(*paths, sizeof(char *) * (*count + 1));
        (*paths)[*count] = nvstrdup(path);
        (*count)++;
    }
}

/*
 * add_search_paths() - 给定一个基础路径，将其下的库目录子路径
 * "/lib"、"/lib32"、"/lib64" 分别添加到搜索路径列表。
 *
 * 这些子目录是 Linux 系统上共享库的标准存放位置：
 *   - /lib    ：默认库目录（32 位系统或部分 64 位系统）
 *   - /lib32  ：明确的 32 位库目录（在 64 位系统上）
 *   - /lib64  ：明确的 64 位库目录
 *
 * 参数：
 *   paths    - 指向搜索路径数组指针的指针（动态增长数组）
 *   count    - 指向路径计数器的指针
 *   pathbase - 基础路径前缀（如 "/usr"、"/usr/X11R6"）
 */
static void add_search_paths(char ***paths, int *count, const char *pathbase)
{
    int i;
    const char *subdirs[] = {
        "/lib",
        "/lib32",
        "/lib64",
    };

    for (i = 0; i < ARRAY_LEN(subdirs); i++) {
        /* 拼接完整路径（如 "/usr/lib64"），尝试添加到搜索列表 */
        char *path = nvstrcat(pathbase, subdirs[i], NULL);
        add_search_path(paths, count, path);
        nvfree(path);
    }
}

/*
 * get_conflicting_search_paths() - 构建冲突文件搜索路径列表。
 *
 * 收集所有可能包含 NVIDIA 驱动相关文件的系统目录，用于后续搜索
 * 与待安装文件冲突的现有文件。搜索范围包括：
 *   - X.Org 默认库路径（/usr/X11R6/lib*）
 *   - Xorg 7.x 默认库路径（/usr/lib*）
 *   - 用户自定义的 X 前缀路径
 *   - OpenGL 库路径
 *   - X 模块路径和 X 库路径
 *   - 在 x86_64 系统上，还包括 32 位兼容库路径
 *
 * 参数：
 *   op    - 全局选项结构体（包含各种安装路径配置）
 *   paths - 输出参数，指向搜索路径数组指针的指针
 *
 * 返回值：
 *   添加到搜索列表中的路径数量
 */
static int get_conflicting_search_paths(const Options *op, char ***paths)
{
    int ret = 0;

    *paths = NULL;

    /* 添加各种标准库目录的 /lib、/lib32、/lib64 子路径 */
    add_search_paths(paths, &ret, DEFAULT_X_PREFIX);       /* /usr/X11R6/lib* */
    add_search_paths(paths, &ret, XORG7_DEFAULT_X_PREFIX); /* /usr/lib* */
    add_search_paths(paths, &ret, op->x_prefix);           /* 用户指定的 X 前缀/lib* */
    add_search_paths(paths, &ret, DEFAULT_OPENGL_PREFIX);  /* /usr/lib*（OpenGL 默认） */
    add_search_paths(paths, &ret, op->opengl_prefix);      /* 用户指定的 OpenGL 前缀/lib* */

    /* 单独添加 X 模块路径和 X 库路径（这些可能不在上述前缀下） */
    add_search_path(paths, &ret, op->x_module_path);
    add_search_path(paths, &ret, op->x_library_path);

#if defined(NV_X86_64)
    /*
     * 在 x86_64 系统上，如果配置了 32 位兼容 chroot 前缀
     * （旧版 Debian 的 /emul/ia32-linux 等），
     * 还需要在 chroot 前缀下搜索 32 位兼容库
     */
    if (op->compat32_chroot != NULL) {
        int i;
        char *subdirs[] = {
            DEFAULT_X_PREFIX,
            op->x_prefix,
            DEFAULT_OPENGL_PREFIX,
            op->opengl_prefix,
            op->compat32_prefix,
        };

        for (i = 0; i < ARRAY_LEN(subdirs); i++) {
            /* 拼接 chroot 前缀 + 各安装前缀，搜索其下的库目录 */
            char *path = nvstrcat(op->compat32_chroot, "/", subdirs[i], NULL);
            add_search_paths(paths, &ret, path);
            nvfree(path);
        }
    }
#endif

    return ret;
}


/*
 * build_command_list() - 构建安装命令列表。
 *
 * 这是安装流程的核心函数，负责构建一个有序的命令列表，
 * 描述安装过程中需要执行的所有操作。命令列表按以下顺序组织：
 *
 *   1. 备份或删除冲突文件（旧版本的库/模块/链接）
 *   2. 重建 initramfs（如果安装内核模块）
 *   3. 安装所有可安装文件（复制文件、设置权限、执行后处理）
 *   4. 创建符号链接
 *   5. 处理 ABI 注释（可选）
 *   6. 运行 ldconfig 更新共享库缓存
 *   7. 运行 depmod 更新模块依赖（如果安装内核模块）
 *   8. 重载 systemd 配置（如果安装了 systemd 单元文件）
 *
 * 如果仅安装内核模块（--kernel-modules-only），则跳过
 * 用户空间库和符号链接的冲突检测。
 *
 * 参数：
 *   op - 全局选项结构体（包含安装配置和路径信息）
 *   p  - 安装包描述结构体（包含所有待安装的文件条目）
 *
 * 返回值：
 *   成功时返回新分配的 CommandList 指针，调用者负责通过
 *   free_command_list() 释放。
 *   失败时返回 NULL（如检测到冲突的已安装模块）。
 */

CommandList *build_command_list(Options *op, Package *p)
{
    FileList *l;             /* 冲突文件列表，收集所有需要备份/删除的现有文件 */
    CommandList *c;          /* 要构建的命令列表 */
    CommandID cmd;           /* 临时变量，存储备份或删除命令类型 */
    int i;
    PackageEntryFileTypeList installable_files;     /* 当前配置下可安装的文件类型集合 */
    PackageEntryFileTypeList tmp_installable_files;  /* 临时副本，用于查找已存在文件时使用 */
    char *tmp;               /* 临时字符串指针，用于构造命令字符串 */

    /* 根据当前选项（如 --no-opengl-files 等）确定哪些文件类型需要安装 */
    get_installable_file_type_list(op, &installable_files);

    /* 分配冲突文件列表和命令列表结构体（nvalloc 会将内存清零） */
    l = (FileList *) nvalloc(sizeof(FileList));
    c = (CommandList *) nvalloc(sizeof(CommandList));

    /*
     * 第一步：查找冲突的内核模块。
     * 在内核模块安装路径下搜索与 NVIDIA 驱动冲突的旧模块
     * （如 nouveau.ko、nvidia.ko 旧版本等）。
     * 如果使用 DKMS 管理或明确不安装内核模块，则跳过此步。
     */

    if (!op->no_kernel_modules && !op->dkms_registered) {
        find_conflicting_kernel_modules(op, l);
    }

    /*
     * 仅安装内核模块模式（--kernel-modules-only）的额外检查：
     * 确保不会覆盖同版本的已有安装。
     */

    if (op->kernel_modules_only) {
        const char *kernel = get_kernel_name(op);

        /* 检查 DKMS 是否已注册了同版本的模块 */
        if (dkms_module_installed(op, p->version, kernel)) {
            ui_error(op, "DKMS kernel modules with version %s are already "
                     "installed for the %s kernel.", p->version, kernel);
            free_file_list(l);
            free_command_list(op,c);
            return NULL;
        }

        /* 检查冲突文件列表中是否有属于当前安装的文件 */
        for (i = 0; i < l->num; i++) {
            if (find_installed_file(op, l->filename[i])) {
                ui_error(op, "The file '%s' already exists as part of this "
                         "driver installation.", l->filename[i]);
                free_file_list(l);
                free_command_list(op, c);
                return NULL;
            }
        }

        /* XXX 已知问题：如果以 --kernel-modules-only 安装在一个已有内核模块
         * 源码但没有编译好的模块或 DKMS 模块的系统上，源文件条目会被
         * 重复添加到备份日志中，导致卸载时出现冗余删除尝试的错误消息。 */

    }

    /*
     * 第二步：搜索用户空间冲突文件（仅在非 --kernel-modules-only 模式下）。
     * 在各个库目录中搜索与待安装共享库同名的文件。
     */

    if (!op->kernel_modules_only) {
        char **paths;
        int numpaths, i;
        ConflictingFileInfo *conflicting_files;

        /*
         * 定义搜索时要跳过的目录：
         * - "source" 和 "build"：内核源码/构建树的符号链接目标，
         *   某些发行版（如 Fedora 21）会在 /usr/lib/modules/ 下放置
         *   指向这些目录的链接（参见 bug 1646361）
         * - "nvidia-cg-toolkit"：NVIDIA Cg 工具包目录，
         *   其中的 libGL.so.1 不应被删除（参见 bug 843595）
         * level = -1 表示在任意深度都跳过这些目录
         */
        static const NoRecursionDirectory skipdirs[] = {
            { -1, "source" },
            { -1, "build" },
            { -1, "nvidia-cg-toolkit" },
            {  0, NULL }               /* 数组终止标记 */
        };

        /* 获取所有需要搜索的路径 */
        numpaths = get_conflicting_search_paths(op, &paths);

        ui_status_begin(op, "Searching for conflicting files:", "Searching");

        /* 构建冲突文件匹配信息数组（文件名模式、匹配长度等） */
        conflicting_files = build_conflicting_file_list(op, p);

        /* 遍历所有搜索路径，在每个路径下递归搜索冲突文件 */
        for (i = 0; i < numpaths; i++) {
            ui_status_update(op, (i + 1.0f) / numpaths, "Searching: %s", paths[i]);
            find_conflicting_files(op, paths[i], conflicting_files, l,
                                   skipdirs);
        }
        nvfree(conflicting_files);

        ui_status_end(op, "done.");
    }

    /*
     * 第三步：查找与待安装文件目标路径相同的已存在文件。
     * 即使没有在冲突搜索中找到，目标路径上已存在的文件也需要被处理。
     */

    /* 复制可安装文件类型列表，添加符号链接类型 */
    tmp_installable_files = installable_files;
    add_symlinks_to_file_type_list(&tmp_installable_files);

    if (op->dkms_registered) {
        /*
         * 如果模块已通过 DKMS 注册，则 DKMS 配置文件和内核模块源码
         * 是从 DKMS tarball 导入的，DKMS 会在 `dkms remove` 时自动删除。
         * 因此 nvidia-installer 不应在已有副本上重复安装这些文件。
         */
        remove_file_type_from_file_type_list(&tmp_installable_files,
                                             FILE_TYPE_DKMS_CONF);
        remove_file_type_from_file_type_list(&tmp_installable_files,
                                             FILE_TYPE_KERNEL_MODULE_SRC);
    }

    /* 检查哪些待安装文件的目标路径已存在，将其加入冲突列表 */
    find_existing_files(p, l, &tmp_installable_files);

    /* 去除冲突文件列表中的重复条目（通过 inode+device 判断同一文件） */

    condense_file_list(p, l);

    /*
     * 第四步：为冲突文件列表中的所有文件生成备份或删除命令。
     * 如果用户指定了 --no-backup，则直接删除；否则备份到备份目录。
     */

    cmd = op->no_backup ? DELETE_CMD : BACKUP_CMD;

    for (i = 0; i < l->num; i++)
        add_command(c, cmd, l->filename[i], NULL, 0);

    /* 如果安装了内核模块，添加重建 initramfs 的函数调用命令 */
    if (!op->no_kernel_modules) {
        add_command(c, FUNCTION_CMD, update_initramfs, "Rebuilding initramfs");
    }

    /*
     * 第五步：添加所有可安装文件的安装命令。
     * 遍历包中的每个文件条目，为可安装类型生成 INSTALL_CMD。
     */

    for (i = 0; i < p->num_entries; i++) {
        /*
         * SELinux 环境下共享库的后处理逻辑：
         *
         * 需要在文件安装到目标文件系统之后、计算 CRC 并记录到备份日志之前
         * 运行 execstack -c 清除可执行栈标记。原因：
         *   1. 目标文件系统基本一定支持 SELinux 属性，
         *      但临时文件系统（如 NFS）可能不支持（参见 bug 530083）
         *   2. execstack 会修改文件内容，必须在 CRC 计算之前完成
         *      （参见 bug 611327）
         *
         * 通过 INSTALL_CMD 的 post-install 命令机制实现。
         */
        if (op->selinux_enabled &&
            (op->utils[EXECSTACK] != NULL) &&
            (p->entries[i].caps.is_shared_lib)) {
            /* 构造 "execstack -c /path/to/lib.so" 命令字符串 */
            tmp = nvstrcat(op->utils[EXECSTACK], " -c ",
                           p->entries[i].dst, NULL);
        } else {
            tmp = NULL;
        }

        /* 如果该文件类型在可安装列表中，生成安装命令 */
        if (installable_files.types[p->entries[i].type]) {
            add_command(c, INSTALL_CMD,
                        p->entries[i].file,   /* 源文件（包内路径） */
                        p->entries[i].dst,    /* 目标路径（系统上的绝对路径） */
                        tmp,                  /* 后处理命令（可为 NULL） */
                        p->entries[i].mode);  /* 文件权限 */
        }

        nvfree(tmp);

        /*
         * 对于图标文件，更新图标主题顶层目录的修改时间（mtime）。
         * 根据 freedesktop.org 图标主题规范，桌面环境通过检测
         * 主题目录的 mtime 变化来决定是否需要刷新图标缓存。
         */
        if (p->entries[i].type == FILE_TYPE_ICON) {
            add_command(c, TOUCH_CMD, op->icon_dir);
        }

        /*
         * 如果文件条目标记为临时文件（is_temporary），
         * 安装完成后删除源文件（包内的临时生成文件）。
         */

        if (p->entries[i].caps.is_temporary) {
            add_command(c, DELETE_CMD,
                        p->entries[i].file);
        }

        /*
         * 在 SELinux 环境下，为已安装的共享库设置正确的安全上下文。
         * 使用 chcon 命令将文件的 SELinux 类型设为 selinux_chcon_type
         * （通常为 textrel_shlib_t，允许包含文本重定位的共享库）。
         */
        if (op->selinux_enabled &&
            (p->entries[i].caps.is_shared_lib)) {
            tmp = nvstrcat(op->utils[CHCON], " -t ", op->selinux_chcon_type,
                           " ", p->entries[i].dst, NULL);
            add_command(c, RUN_CMD, tmp);
            nvfree(tmp);
        }
    }


    /*
     * 第六步：创建符号链接。
     * 遍历所有标记为符号链接的包条目，生成 SYMLINK_CMD 命令。
     * 符号链接在文件安装之后创建，确保链接目标已存在。
     */

    for (i = 0; i < p->num_entries; i++) {
        if (p->entries[i].caps.is_symlink) {
            add_command(c, SYMLINK_CMD, p->entries[i].dst,
                        p->entries[i].target);
        }
    }

    /*
     * 第七步：处理 --no-abi-note 选项。
     * 如果用户请求移除 ABI 标签，使用 objcopy 从所有 OpenGL 库中
     * 删除 .note.ABI-tag ELF section。
     * 命令格式：objcopy --remove-section=.note.ABI-tag <文件> 2>/dev/null ; true
     * 末尾的 "; true" 确保即使 objcopy 失败也不影响整体安装。
     */

    if (op->no_abi_note) {

        if (op->utils[OBJCOPY]) {
            for (i = 0; i < p->num_entries; i++) {
                if (p->entries[i].type == FILE_TYPE_OPENGL_LIB) {
                    tmp = nvstrcat(op->utils[OBJCOPY],
                            " --remove-section=.note.ABI-tag ",
                            p->entries[i].dst,
                            " 2> /dev/null ; true", NULL);
                    add_command(c, RUN_CMD, tmp);
                    nvfree(tmp);
                }
            }
        } else {
            ui_warn(op, "--no-abi-note option was specified but the system "
                    "utility `objcopy` (package 'binutils') was not found; this "
                    "operation will be skipped.");
        }
    }

    /*
     * 第八步：运行 ldconfig 更新共享库缓存。
     * 安装新的共享库后必须运行 ldconfig，否则动态链接器找不到新库。
     */

    add_command(c, RUN_CMD, op->utils[LDCONFIG]);

    /*
     * 第九步：运行 depmod 更新内核模块依赖关系。
     * 使用 RUN_CMD_LONG 因为 depmod 可能耗时较长（需要扫描所有模块）。
     * 如果为非当前运行的内核构建模块（通过 --kernel-name 指定），
     * 需要在 depmod 命令行中指定目标内核名。
     * 补丁由 Nigel Spowage <Nigel.Spowage@energis.com> 提供。
     */

    if (!op->no_kernel_modules && !op->skip_depmod) {
        tmp = nvstrcat(op->utils[DEPMOD], " -a ", op->kernel_name, NULL);
        add_command(c, RUN_CMD_LONG, tmp);
        nvfree(tmp);
    }

    /*
     * 第十步：如果安装了 systemd 单元文件，运行 `systemctl daemon-reload`
     * 通知 systemd 重新加载配置。
     */
    if (op->use_systemd == NV_OPTIONAL_BOOL_TRUE) {
        tmp = nvstrcat(op->utils[SYSTEMCTL], " daemon-reload", NULL);
        add_command(c, RUN_CMD, tmp);
        nvfree(tmp);
    }

    /* 释放冲突文件列表（命令列表中已复制了所需的文件名） */
    free_file_list(l);

    return c;

} /* build_command_list() */



/*
 * free_file_list() - 释放文件列表结构体及其所有内容。
 *
 * 依次释放每个文件名字符串、文件名指针数组，最后释放 FileList 结构体本身。
 *
 * 参数：
 *   l - 要释放的文件列表指针，NULL 安全（直接返回）
 */

static void free_file_list(FileList* l)
{
    int i;

    if (!l) return;

    /* 释放每个文件名字符串 */
    for (i = 0; i < l->num; i++) {
        nvfree(l->filename[i]);
    }

    /* 释放文件名指针数组和结构体本身 */
    nvfree((char *) l->filename);
    nvfree((char *) l);

} /* free_file_list() */



/*
 * free_command_list() - 释放命令列表结构体及其所有内容。
 *
 * 遍历每条命令，释放其 path、target、command 字符串以及描述文本，
 * 然后释放命令数组、描述数组和结构体本身。
 *
 * 参数：
 *   op - 全局选项结构体（当前实现中未使用，保留以备将来扩展）
 *   cl - 要释放的命令列表指针，NULL 安全（直接返回）
 */

void free_command_list(Options *op, CommandList *cl)
{
    int i;
    Command *c;

    if (!cl) return;

    /* 释放每条命令的所有动态分配字段 */
    for (i = 0; i < cl->num; i++) {
        c = &cl->cmds[i];
        free(c->path);
        free(c->target);
        free(c->command);
        free(cl->descriptions[i]);
    }

    /* 释放命令数组和描述数组 */
    free(cl->cmds);
    free(cl->descriptions);

    /* 释放命令列表结构体本身 */
    free(cl);

} /* free_command_list() */

/*
 * execute_run_command() - 执行命令列表中的一条 RUN_CMD 或 RUN_CMD_LONG 命令。
 *
 * 根据 percent 参数决定进度显示方式：
 *   - percent >= 0 ：显示确定性进度条，percent 表示当前完成百分比（0.0~1.0）
 *   - percent < 0  ：显示不确定进度条（不断旋转/移动的动画，表示正在执行）
 *
 * 命令执行失败时会弹出错误提示，并询问用户是否继续安装。
 *
 * 参数：
 *   op      - 全局选项结构体
 *   percent - 当前进度百分比（负值表示使用不确定进度条）
 *   cmd     - 要执行的 shell 命令字符串
 *
 * 返回值：
 *   TRUE  - 命令执行成功，或失败后用户选择继续
 *   FALSE - 命令执行失败且用户选择中止
 */

/* 执行命令时的 UI 格式化字符串 */
#define __RUN_CMD_FORMAT_STRING__ "Executing: %s (this may take a moment...)"

static inline int execute_run_command(Options *op, float percent, const char *cmd)
{
    int indeterminate = percent < 0;  /* 是否使用不确定进度条 */
    int ret;
    char *data;   /* 命令的标准输出/错误输出内容 */

    /* 在专家模式下记录正在执行的命令 */
    ui_expert(op, "Executing: %s", cmd);

    /* 根据进度类型更新 UI 显示 */
    if (indeterminate) {
        ui_indeterminate_begin(op, __RUN_CMD_FORMAT_STRING__, cmd);
    } else {
        ui_status_update(op, percent, __RUN_CMD_FORMAT_STRING__, cmd);
    }

    /* 执行命令并捕获输出 */
    ret = run_command(op, &data, TRUE, NULL, TRUE, cmd, NULL);

    /* 结束不确定进度条显示 */
    if (indeterminate) {
        ui_indeterminate_end(op);
    }

    /* 处理命令执行失败的情况 */
    if (ret != 0) {
        ui_error(op, "Failed to execute `%s`: %s", cmd, data);
        /* 询问用户是否继续（在 --no-questions 模式下自动继续） */
        ret = continue_after_error(op, "Failed to execute `%s`", cmd);
        if (!ret) {
            nvfree(data);
            return FALSE;
        }
    }
    if (data) free(data);
    return TRUE;
} /* execute_run_command() */

/*
 * execute_command_list() - 执行命令列表中的所有命令。
 *
 * 按照命令列表的顺序逐条执行每个操作，并通过 UI 显示实时进度。
 * 任何操作失败时，调用 continue_after_error() 询问用户是否继续。
 * 如果用户选择不继续，立即返回 FALSE 中止安装。
 *
 * 参数：
 *   op    - 全局选项结构体
 *   c     - 要执行的命令列表
 *   title - 进度条标题文本（如 "Installing"）
 *   msg   - 进度条消息文本（如 "Installing files..."）
 *
 * 返回值：
 *   TRUE  - 所有命令执行成功（或失败后用户选择继续）
 *   FALSE - 某条命令执行失败且用户选择中止
 */

int execute_command_list(Options *op, CommandList *c,
                         const char *title, const char *msg)
{
    int i, ret;
    float percent;   /* 当前完成百分比（0.0 ~ 1.0） */

    /* 开始显示进度条 */
    ui_status_begin(op, title, "%s", msg);

    for (i = 0; i < c->num; i++) {

        /* 计算当前进度百分比 */
        percent = (float) i / (float) c->num;

        /* 根据命令类型分派执行 */
        switch (c->cmds[i].cmd) {

        case INSTALL_CMD:
            /* 安装文件：将源文件复制到目标路径并设置权限 */
            ui_expert(op, "Installing: %s --> %s",
                      c->cmds[i].path, c->cmds[i].target);
            ui_status_update(op, percent, "Installing: %s", c->cmds[i].target);

            ret = install_file(op, c->cmds[i].path, c->cmds[i].target,
                               c->cmds[i].mode);
            if (!ret) {
                ret = continue_after_error(op, "Cannot install %s",
                                           c->cmds[i].target);
                if (!ret) return FALSE;
            } else {
                /*
                 * 安装成功后，先执行后处理命令（如 execstack -c），
                 * 再记录到备份日志。这确保日志中记录的 CRC 是最终状态。
                 */
                if (c->cmds[i].command &&
                    !execute_run_command(op, percent, c->cmds[i].command)) {
                    return FALSE;
                }

                /* 在备份日志中记录已安装的文件 */
                log_install_file(op, c->cmds[i].target);
                /* 追加到 RPM 文件列表（如果配置了） */
                append_to_rpm_file_list(op, &c->cmds[i]);
            }
            break;

        case RUN_CMD:
            /* 运行 shell 命令（显示确定性进度条） */
            if (!execute_run_command(op, percent, c->cmds[i].command)) {
                return FALSE;
            }
            break;
        case RUN_CMD_LONG:
            /* 运行耗时较长的 shell 命令（显示不确定性进度条，percent 传 -1） */
            if (!execute_run_command(op, c->cmds[i].cmd == RUN_CMD_LONG ? -1 : percent, c->cmds[i].command)) {
                return FALSE;
            }
            break;

        case SYMLINK_CMD:
            /* 创建符号链接：path 为链接名，target 为链接目标 */
            ui_expert(op, "Creating symlink: %s -> %s",
                      c->cmds[i].path, c->cmds[i].target);
            ui_status_update(op, percent, "Creating symlink: %s",
                             c->cmds[i].target);

            ret = install_symlink(op, c->cmds[i].target, c->cmds[i].path);

            if (!ret) {
                ret = continue_after_error(op, "Cannot create symlink %s (%s)",
                                           c->cmds[i].path, strerror(errno));
                if (!ret) return FALSE;
            } else {
                /* 在备份日志中记录已创建的符号链接 */
                log_create_symlink(op, c->cmds[i].path, c->cmds[i].target);
            }
            break;

        case BACKUP_CMD:
            /* 备份文件：将文件移动到备份目录，并记录到备份日志 */
            ui_expert(op, "Backing up: %s", c->cmds[i].path);
            ui_status_update(op, percent, "Backing up: %s", c->cmds[i].path);

            ret = do_backup(op, c->cmds[i].path);
            if (!ret) {
                ret = continue_after_error(op, "Cannot backup %s",
                                           c->cmds[i].path);
                if (!ret) return FALSE;
            }
            break;

        case DELETE_CMD:
            /* 删除文件：直接使用 unlink() 删除 */
            ui_expert(op, "Deleting: %s", c->cmds[i].path);
            ret = unlink(c->cmds[i].path);
            if (ret == -1) {
                ret = continue_after_error(op, "Cannot delete %s",
                                           c->cmds[i].path);
                if (!ret) return FALSE;
            }
            break;

        case TOUCH_CMD:
            /* 更新文件修改时间：使用 utime(path, NULL) 设为当前时间 */
            ui_expert(op, "Updating mtime: %s", c->cmds[i].path);
            ret = utime(c->cmds[i].path, NULL);
            if (ret == -1) {
                ret = continue_after_error(op, "Cannot touch %s",
                                           c->cmds[i].path);
                if (!ret) return FALSE;
            }
            break;
        case FUNCTION_CMD:
            /* 调用函数指针：执行 C 函数回调（如 update_initramfs） */
            ui_expert(op, "%s:", c->descriptions[i]);
            ret = c->cmds[i].function(op);
            if (!ret) {
                ret = continue_after_error(op, "%s failed", c->descriptions[i]);
                if (!ret) return FALSE;
            }
            break;
        default:
            /* XXX 不应执行到此处——表示出现了未知的命令类型 */
            return FALSE;
            break;
        }
    }

    /* 所有命令执行完毕，结束进度显示 */
    ui_status_end(op, "done.");

    return TRUE;

} /* execute_command_list() */


/*
 ***************************************************************************
 * 以下为本文件的内部静态函数实现
 ***************************************************************************
 */



/*
 * find_conflicting_kernel_modules() - 搜索与 NVIDIA 驱动冲突的内核模块。
 *
 * 在内核模块安装路径（如 /lib/modules/<kernel>/kernel/drivers/video）
 * 和标准内核模块目录（/lib/modules/<kernel>）下递归搜索冲突模块。
 * 冲突模块列表定义在 conflicting-kernel-modules.h 中，
 * 包括 nouveau.ko、nvidia.ko 旧版本等。
 *
 * 参数：
 *   op - 全局选项结构体
 *   l  - 输出参数，找到的冲突模块路径追加到此文件列表
 */

static void find_conflicting_kernel_modules(Options *op, FileList *l)
{
    int i = 0;
    ConflictingFileInfo *files;   /* 冲突模块匹配信息数组 */
    char *paths[3];               /* 搜索路径数组（最多 2 个路径 + NULL 终止） */
    char *tmp = get_kernel_name(op);  /* 获取目标内核版本名 */
    char **filenames;             /* 临时数组，存储拼接了 ".ko" 后缀的模块文件名 */

    /*
     * 跳过 "build" 和 "source" 目录：
     * 这些目录不包含编译好的模块，且可能是指向实际内核源码树的符号链接，
     * 递归进入会导致搜索范围意外扩大。
     * level = 1 表示仅在深度 >= 1 时跳过（即搜索根目录的直接子目录）。
     */
    static const NoRecursionDirectory skipdirs[] = {
        { 1, "build" },
        { 1, "source" },
        { 0, NULL }
    };

    /* 构建搜索路径列表 */
    if (op->kernel_module_installation_path) {
        paths[i++] = op->kernel_module_installation_path;  /* 用户指定的安装路径 */
    }

    if (tmp) {
        paths[i++] = nvstrcat("/lib/modules/", tmp, NULL);  /* 标准内核模块目录 */
    }

    paths[i] = NULL;  /* NULL 终止 */

    /* 构建冲突内核模块文件名列表（在模块名后追加 ".ko" 扩展名） */

    files = nvalloc((num_conflicting_kernel_modules + 1) * sizeof(files[0]));
    filenames = nvalloc(num_conflicting_kernel_modules * sizeof(filenames[0]));

    for (i = 0; i < num_conflicting_kernel_modules; i++) {
        filenames[i] = nvstrcat(conflicting_kernel_modules[i], ".ko", NULL);
        files[i].name = filenames[i];
        files[i].len = strlen(filenames[i]);
        /* 注意：内核模块不设置 requiredString，默认为 NULL */
    }

    /* 在每个搜索路径下递归搜索冲突模块 */
    for (i = 0; paths[i]; i++) {
        find_conflicting_files(op, paths[i], files, l, skipdirs);
    }

    /* 释放通过 nvstrcat() 动态分配的路径（跳过 paths[0]，它指向 op 中的静态字段） */

    for (i = 1; paths[i]; i++) {
        nvfree(paths[i]);
    }

    /* 释放临时的模块文件名数组 */

    for (i = 0; i < num_conflicting_kernel_modules; i++) {
        nvfree(filenames[i]);
    }
    nvfree(filenames);
    nvfree(files);
}



/*
 * find_existing_files() - 查找与待安装文件目标路径相同的已存在文件。
 *
 * 遍历安装包中的所有文件条目，对于指定文件类型列表中的类型，
 * 检查其目标安装路径（dst）是否已在文件系统上存在。
 * 如果存在，将该路径添加到冲突文件列表中，以便后续备份或删除。
 *
 * 使用 lstat() 而非 stat()，因为我们需要检测符号链接本身是否存在，
 * 而不是解析链接后的目标文件。
 *
 * 参数：
 *   p              - 安装包描述结构体
 *   l              - 输出参数，已存在文件的路径追加到此文件列表
 *   file_type_list - 要检查的文件类型集合（布尔数组）
 */

static void find_existing_files(Package *p, FileList *l,
                                PackageEntryFileTypeList *file_type_list)
{
    int i;
    struct stat stat_buf;

    for (i = 0; i < p->num_entries; i++) {
        /* 仅检查指定类型列表中的文件类型 */
        if (file_type_list->types[p->entries[i].type]) {
            /* 使用 lstat 检查目标路径是否存在（不跟随符号链接） */
            if (lstat(p->entries[i].dst, &stat_buf) == 0) {
                add_file_to_list(NULL, p->entries[i].dst, l);
            }
        }
    }
} /* find_existing_files() */



/*
 * ignore_conflicting_file() - 判断是否应忽略（不备份/不删除）某个冲突文件。
 *
 * 当 ConflictingFileInfo 中的 requiredString 非 NULL 时，
 * 需要在文件内容中搜索该字符串。如果找不到该字符串，
 * 说明该文件不是我们要替换的 NVIDIA 版本，应该被忽略。
 *
 * 典型用例：libglx.so 文件可能来自 Xorg 或 NVIDIA，
 * 只有包含 "glxModuleData" 字符串的才是 NVIDIA 版本的 GLX 模块，
 * 需要被识别为冲突文件。非 NVIDIA 的 libglx.so 应被忽略，
 * 避免误删系统的 X.Org GLX 模块（参见 bug 489316）。
 *
 * 实现方式：使用 mmap 将文件映射到内存，然后线性扫描搜索
 * requiredString。搜索时要求字符串后跟 '\0' 或在文件末尾，
 * 确保匹配的是完整的嵌入字符串而非偶然的字节序列。
 *
 * 参数：
 *   op       - 全局选项结构体（用于错误输出）
 *   filename - 要检查的文件路径
 *   info     - 冲突文件信息（包含 requiredString）
 *
 * 返回值：
 *   TRUE  - 应忽略该文件（不将其加入冲突列表）
 *   FALSE - 不应忽略（该文件确实是冲突文件，需要备份/删除）
 */

static int ignore_conflicting_file(Options *op,
                                   const char *filename,
                                   const ConflictingFileInfo info)
{
    int fd = -1;
    struct stat stat_buf;
    char *file = MAP_FAILED;   /* mmap 映射地址，初始化为 MAP_FAILED */
    int ret = FALSE;
    int i, len, size = 0;

    /* 如果没有 requiredString，不需要内容检查，直接返回"不忽略" */

    if (!info.requiredString) return ret;

    ret = FALSE;

    /* 以只读模式打开文件 */
    if ((fd = open(filename, O_RDONLY)) == -1) {
        ui_error(op, "Unable to open '%s' for reading (%s)",
                 filename, strerror(errno));
        goto cleanup;
    }

    /* 获取文件大小 */
    if (fstat(fd, &stat_buf) == -1) {
        ui_error(op, "Unable to determine size of '%s' (%s)",
                 filename, strerror(errno));
        goto cleanup;
    }

    size = stat_buf.st_size;

    /* 空文件直接跳过（不忽略，交给调用者处理） */
    if (!size) {
        goto cleanup;
    }

    /* 将文件内容映射到内存（只读共享映射） */
    if ((file = mmap(0, size, PROT_READ,
                     MAP_FILE | MAP_SHARED, fd, 0)) == MAP_FAILED) {
        ui_error(op, "Unable to map file '%s' for reading (%s)",
                 filename, strerror(errno));
        goto cleanup;
    }

    /*
     * 在文件内容中搜索 requiredString。
     * 默认假设"应忽略"（ret = TRUE），如果找到了字符串则改为"不忽略"。
     *
     * 匹配条件：
     *   1. 字符串内容匹配（strncmp）
     *   2. 字符串后紧跟 '\0'，或字符串恰好在文件末尾
     *      （确保匹配的是完整的以 NULL 结尾的 C 字符串）
     */

    ret = TRUE;  /* 假设找不到，应忽略 */

    len = strlen(info.requiredString);

    for (i = 0; (i + len) <= size; i++) {
        if ((strncmp(&file[i], info.requiredString, len) == 0) &&
            (((i + len) == size) || (file[i+len] == '\0'))) {
            ret = FALSE;  /* 找到了 requiredString，不忽略此文件 */
            break;
        }
    }

    /* 清理资源 */

 cleanup:

    if (file != MAP_FAILED) {
        munmap(file, size);
    }

    if (fd != -1) {
        close(fd);
    }

    return ret;

} /* ignore_conflicting_file() */




/*
 * find_conflicting_files() - 在指定路径下递归搜索与待安装文件冲突的现有文件。
 *
 * 使用 POSIX FTS（File Tree Walk）API 逻辑遍历（FTS_LOGICAL，
 * 跟随符号链接）文件树。对于每个找到的普通文件或悬空符号链接，
 * 用其文件名与冲突文件列表中的模式进行前缀匹配。
 *
 * 匹配规则：使用 strncmp 只比较 files[i].len 个字符，
 * 这样 "libGL." 可以匹配 "libGL.so.1"、"libGL.so.535.129.03" 等。
 * 匹配后还会调用 ignore_conflicting_file() 进行内容检查，
 * 确保不会误删非 NVIDIA 版本的同名文件。
 *
 * 参数：
 *   op       - 全局选项结构体
 *   path     - 搜索起始路径（如 "/usr/lib64"）
 *   files    - 冲突文件信息数组（以 name==NULL 结尾）
 *   l        - 输出参数，找到的冲突文件路径追加到此列表
 *   skipdirs - 要跳过的目录名数组（以 name==NULL 结尾）
 */

static void find_conflicting_files(Options *op,
                                   char *path,
                                   ConflictingFileInfo *files,
                                   FileList *l,
                                   const NoRecursionDirectory *skipdirs)
{
    int i;
    char *paths[2];
    FTS *fts;
    FTSENT *ent;

    paths[0] = path; /* FTS 搜索根路径 */
    paths[1] = NULL; /* NULL 终止（fts_open 要求） */

    /*
     * 打开文件树遍历：
     * FTS_LOGICAL - 逻辑遍历，跟随符号链接（简化路径中包含符号链接到目录时的处理）
     * FTS_NOSTAT  - 不对每个文件调用 stat()（提高性能，我们不需要 stat 信息）
     */
    fts = fts_open(paths, FTS_LOGICAL | FTS_NOSTAT, NULL);
    if (!fts) return;

    /* 逐一读取文件树中的条目 */
    while ((ent = fts_read(fts)) != NULL) {
        switch (ent->fts_info) {
        case FTS_F:       /* 普通文件 */
        case FTS_SLNONE:  /* 悬空的符号链接（指向不存在的目标） */
            /*
             * 对于文件，与冲突文件列表中的每个模式进行前缀匹配。
             * 使用 strncmp 仅比较 len 个字符，
             * 例如 "libGL." (len=6) 可以匹配 "libGL.so.1"。
             */
            for (i = 0; files[i].name; i++) {
                if (!strncmp(ent->fts_name, files[i].name, files[i].len) &&
                    !ignore_conflicting_file(op, ent->fts_path,
                                             files[i])) {
                    /* 文件名匹配且不应被忽略，添加到冲突列表 */
                    add_file_to_list(NULL, ent->fts_path, l);
                }
            }
            break;

        case FTS_DP:  /* 目录（后序遍历，即子项已处理完毕） */
        case FTS_D:   /* 目录（前序遍历，即即将进入） */
            if (op->no_recursion) {
                /* --no-recursion 模式：跳过所有子目录，只搜索顶层 */
                fts_set(fts, ent, FTS_SKIP);
            } else if (skipdirs) {
                /*
                 * 检查当前目录是否在跳过列表中。
                 * 跳过条件：
                 *   1. 目录名匹配 skipdirs 中的某个条目
                 *   2. 且当前深度在该条目允许的范围内
                 *      （level < 0 表示任意深度都跳过，
                 *       level >= fts_level 表示仅在浅于指定深度时跳过）
                 */
                const NoRecursionDirectory *dir;
                for (dir = skipdirs; dir->name; dir++) {
                    if ((dir->level < 0 || dir->level >= ent->fts_level) &&
                        strcmp(ent->fts_name, dir->name) == 0) {
                        fts_set(fts, ent, FTS_SKIP);
                    }
                }
            }
            break;

        default:
            /*
             * 只关心普通文件、符号链接和目录。
             * 由于使用 FTS_LOGICAL 逻辑遍历（跟随符号链接），
             * 只需处理悬空链接（FTS_SLNONE）和目录（如果未禁用递归）。
             * 其他类型（设备文件、FIFO 等）直接忽略。
             */
            break;
        }
    }

    fts_close(fts);

} /* find_conflicting_files() */

/*
 * get_conflicting_file_info() - 根据文件名填充冲突文件信息结构体。
 *
 * 主要完成两件事：
 *   1. 计算匹配长度（len）：如果文件名中包含 ".so."，
 *      则 len 设置为 ".so" 之后的位置，这样匹配时只比较到 ".so" 为止。
 *      例如 "libGL.so.1" 的 len 为 9（"libGL.so."中"."之前），
 *      这使得 "libGL.so" 可以匹配 "libGL.so.1"、"libGL.so.535.129.03" 等
 *      所有版本的 DSO 文件。
 *
 *   2. 设置 requiredString：对于特殊文件（如 libglx.so），
 *      需要检查文件内容中是否包含特定字符串才能确认是 NVIDIA 版本。
 *
 * 参数：
 *   file - 文件名（不含路径，如 "libGL.so.1"）
 *   cfi  - 输出参数，填充后的冲突文件信息结构体
 */
void get_conflicting_file_info(const char *file, ConflictingFileInfo *cfi)
{
    char *c;

    cfi->name = file;

    /*
     * 对于 DSO（动态共享对象）文件名，在 ".so." 处截断匹配长度。
     * 例如：对于 "libGL.so.535.129.03"，找到 ".so." 后，
     * len = (指向".so."的指针 - 文件名起始指针) + 3 = "libGL.so" 的长度。
     * 这样 strncmp(filename, "libGL.so", 8) 就能匹配所有版本的 libGL.so.*。
     */

    c = strstr(file, ".so.");
    if (c) {
        cfi->len = (c - file) + 3;  /* +3 跳过 ".so" 三个字符 */
    } else {
        cfi->len = strlen(file);     /* 无 ".so." 则使用完整文件名长度 */
    }

    /*
     * XXX 特殊处理 libglx.so：
     * 系统中可能同时存在 NVIDIA 版本和 Xorg 原版的 libglx.so。
     * 只有包含 "glxModuleData" 符号的才是 NVIDIA 版本，
     * 需要被替换。设置 requiredString 后，ignore_conflicting_file()
     * 会打开文件搜索此字符串，避免误删 Xorg 的 libglx.so。
     * （参见 bug 489316）
     */

    if (strncmp(file, "libglx.so", cfi->len) == 0) {
        cfi->requiredString = "glxModuleData";
    } else {
        cfi->requiredString = NULL;
    }
}

/*
 * build_conflicting_file_list() - 构建冲突文件匹配信息数组。
 *
 * 遍历安装包中的所有文件条目，将所有标记为"共享库"且"可冲突"的
 * 文件（is_shared_lib && is_conflicting）提取出来，构建一个
 * ConflictingFileInfo 数组。这个数组后续被 find_conflicting_files()
 * 用于在文件系统中搜索同名的旧文件。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包描述结构体
 *
 * 返回值：
 *   动态分配的 ConflictingFileInfo 数组，以 name==NULL 结尾。
 *   调用者负责使用 nvfree() 释放。
 */
ConflictingFileInfo *build_conflicting_file_list(Options *op, Package *p)
{
    ConflictingFileInfo *cfList = NULL;
    int index = 0;
    int i;

    /*
     * 分配足够的空间：包条目数 + 2 个额外条目 + 1 个 NULL 终止。
     * 额外条目用于追加固定的冲突文件（如 libGLwrapper.so）。
     */
    cfList = nvalloc((p->num_entries + 3) * sizeof(ConflictingFileInfo));

    /* 遍历包条目，提取可冲突的共享库文件名 */
    for (i = 0; i < p->num_entries; i++) {
        PackageEntry *entry = &p->entries[i];
        if (entry->caps.is_shared_lib && entry->caps.is_conflicting) {
            get_conflicting_file_info(entry->name, &cfList[index++]);
        }
    }

    /*
     * XXX 如果要安装 OpenGL 文件，始终将 libGLwrapper.so 加入冲突列表。
     * libGLwrapper.so 的 SONAME 为 libGL.so.1，会与 NVIDIA 的 libGL
     * 冲突。如果不删除它，动态链接器可能加载错误的 libGL。
     * （参见 bug 74761）
     */
    if (!op->no_opengl_files) {
        get_conflicting_file_info("libGLwrapper.so", &cfList[index++]);
    }

    /* 以 name==NULL 终止数组 */
    cfList[index].name = NULL;

    return cfList;
}



/*
 * condense_file_list() - 去除文件列表中的重复条目。
 *
 * 由于冲突文件搜索可能在多个路径下找到同一个文件（通过符号链接
 * 或硬链接），文件列表中可能存在指向同一个 inode 的多个路径名。
 * 此函数通过比较 (device, inode) 对来识别并去除重复条目。
 *
 * 同时，此函数还会过滤掉属于当前安装包本身的文件。
 * 这种情况可能发生在符号链接欺骗的场景下——搜索冲突文件时，
 * 符号链接可能引导搜索进入了我们自己解包的 .run 文件目录，
 * 导致包内文件被误判为冲突文件。
 *
 * 算法：暴力 O(n^2) 遍历（作者自称 "brain dead brute-force"），
 * 对于每个文件，检查它是否与已选中的文件或包内文件重复。
 *
 * 参数：
 *   p - 安装包描述（用于排除包内文件）
 *   l - 输入/输出参数，文件列表将被原地精简
 */

static void condense_file_list(Package *p, FileList *l)
{
    char **s = NULL;             /* 精简后的文件名数组（新列表） */
    int n = 0, i, j, keep;      /* n = 新列表中的文件数量 */

    struct stat stat_buf, *stat_bufs;  /* 当前文件的 stat 信息和新列表中文件的 stat 数组 */

    /* 为临时 stat 数组分配足够的空间（最多与原列表等长） */

    if (l->num) {
        stat_bufs  = nvalloc(sizeof(struct stat) * l->num);
    } else {
        stat_bufs  = NULL;
    }

    /*
     * 遍历原始文件列表，将唯一文件移动到新列表。
     * 对于每个文件，先获取其文件系统信息（inode + device），
     * 然后与新列表中已有的文件和包内文件进行比较。
     * 仅当文件不与任何已有条目重复时，才将其加入新列表。
     */

    for (i = 0; i < l->num; i++) {
        keep = TRUE;

        /* 获取文件的 inode 和 device 信息（lstat 不跟随符号链接） */
        if (lstat(l->filename[i], &stat_buf) == -1)
            continue;  /* 无法 stat 的文件直接跳过（可能已被删除） */

        /*
         * 检查 1：排除包内文件。
         * 通过 (device, inode) 与包中每个文件条目比较，
         * 如果匹配则说明这是我们自己包里的文件，不应被备份/删除。
         */

        for (j = 0; j < p->num_entries; j++) {
            if ((p->entries[j].device == stat_buf.st_dev) &&
                (p->entries[j].inode == stat_buf.st_ino)) {
                keep = FALSE;
                break;
            }
        }

        /*
         * 检查 2：与新列表中的文件去重。
         * 通过 (device, inode) 比较判断两个路径是否指向同一个文件。
         * 这能正确处理硬链接和通过不同符号链接路径到达的同一文件。
         */

        for (j = 0; keep && (j < n); j++) {

            if ((stat_buf.st_dev == stat_bufs[j].st_dev) &&
                (stat_buf.st_ino == stat_bufs[j].st_ino)) {
                keep = FALSE;
                break;
            }
        }

        /* 如果文件是唯一的且不属于包本身，添加到新列表 */
        if (keep) {
            s = (char **) nvrealloc(s, sizeof(char *) * (n + 1));
            s[n] = nvstrdup(l->filename[i]);
            stat_bufs[n] = stat_buf;
            n++;
        }
    }

    /* 释放临时 stat 数组 */
    if (stat_bufs) nvfree((void *)stat_bufs);

    /* 释放原始文件名列表 */
    for (i = 0; i < l->num; i++) free(l->filename[i]);
    free(l->filename);

    /* 用精简后的新列表替换原列表 */
    l->filename = s;
    l->num = n;

} /* condense_file_list() */



/*
 * get_command_description() - 生成命令的人类可读描述文本。
 *
 * 根据命令类型生成描述字符串，用于 UI 审核显示。
 * 描述文本包含操作类型、涉及的文件路径和权限等详细信息。
 *
 * 注意：FUNCTION_CMD 类型的描述由 add_command() 的调用者提供，
 * 因此此函数不处理该类型。
 *
 * 参数：
 *   c - 命令结构体指针
 *
 * 返回值：
 *   动态分配的描述字符串，调用者负责释放。
 *   对于 FUNCTION_CMD 和未知类型返回 NULL。
 */

static char *get_command_description(const Command *c)
{
    char *ret = NULL;
    char *perms;

    switch (c->cmd) {
    case INSTALL_CMD:
        /* 生成格式如："Install the file 'src' as 'dst' with permissions 'rwxr-xr-x'" */
        perms = mode_to_permission_string(c->mode);
        ret = nvasprintf("Install the file '%s' as '%s' with permissions '%s'",
                         c->path, c->target, perms);
        /* 如果有后处理命令，追加到描述中 */
        if (c->command) {
            char *newret  = nvstrcat(ret, " then execute the command `",
                                     c->command, "`", NULL);
            nvfree(ret);
            ret = newret;
        }
        nvfree(perms);
        break;

    case RUN_CMD:
        ret = nvasprintf("Execute the command `%s`", c->command);
        break;

    case SYMLINK_CMD:
        ret = nvasprintf("Create a symbolic link '%s' to '%s'",
                         c->path, c->target);
        break;

    case BACKUP_CMD:
        ret = nvasprintf("Back up the file '%s'", c->path);
        break;

    case DELETE_CMD:
        ret = nvasprintf("Delete the file '%s'", c->path);
        break;

    case FUNCTION_CMD:
        /* FUNCTION_CMD 的描述由 add_command() 的调用者直接提供 */
        break;

    default:
        /* XXX 不应执行到此处——表示出现了未知的命令类型 */
        break;
    }

    return ret;
}


/*
 * add_command() - 向命令列表追加一条新命令。
 *
 * 动态扩展命令列表数组，解析变参列表中的参数填充新命令的各字段。
 * 不同命令类型的变参格式如下：
 *
 *   INSTALL_CMD  ：(char *path, char *target, char *command, mode_t mode)
 *   SYMLINK_CMD  ：(char *path, char *target)
 *   BACKUP_CMD   ：(char *path)
 *   DELETE_CMD   ：(char *path)
 *   TOUCH_CMD    ：(char *path)
 *   RUN_CMD      ：(char *command)
 *   RUN_CMD_LONG ：(char *command)
 *   FUNCTION_CMD ：(CommandFunction function, char *description)
 *
 * 所有字符串参数都会通过 nvstrdup() 复制一份，
 * 因此调用者可以在调用后释放原始字符串。
 *
 * 参数：
 *   c   - 命令列表
 *   cmd - 命令类型
 *   ... - 根据命令类型不同的变参（见上方说明）
 */

static void add_command(CommandList *c, CommandID cmd, ...)
{
    int n = c->num;   /* 新命令的索引（追加到末尾） */
    char *s;
    va_list ap;

    /* 动态扩展命令数组和描述数组 */
    c->cmds = (Command *) nvrealloc(c->cmds, sizeof(Command) * (n + 1));
    c->descriptions = nvrealloc(c->descriptions, sizeof(char *) * (n + 1));

    /* 将新命令的内存清零，设置命令类型 */
    memset(c->cmds + n, 0, sizeof(Command));
    c->cmds[n].cmd  = cmd;
    c->descriptions[n] = NULL;

    /* 根据命令类型解析变参并填充命令字段 */
    va_start(ap, cmd);

    switch (cmd) {
      case INSTALL_CMD:
      case SYMLINK_CMD:
        /* 读取 path 和 target */
        s = va_arg(ap, char *);
        c->cmds[n].path = nvstrdup(s);
        s = va_arg(ap, char *);
        c->cmds[n].target = nvstrdup(s);

        /* INSTALL_CMD 还需要读取后处理命令和权限 */
        if (cmd == INSTALL_CMD) {
            s = va_arg(ap, char *);
            c->cmds[n].command = nvstrdup(s);  /* 可为 NULL */
            c->cmds[n].mode = va_arg(ap, mode_t);
        }

        break;
      case BACKUP_CMD:
      case DELETE_CMD:
      case TOUCH_CMD:
        /* 只需要 path */
        s = va_arg(ap, char *);
        c->cmds[n].path = nvstrdup(s);
        break;
      case RUN_CMD:
      case RUN_CMD_LONG:
        /* 只需要 command 字符串 */
        s = va_arg(ap, char *);
        c->cmds[n].command = nvstrdup(s);
        break;
      case FUNCTION_CMD:
        /* 读取函数指针和描述文本 */
        c->cmds[n].function = va_arg(ap, CommandFunction);
        s = va_arg(ap, char *);
        c->descriptions[n] = nvstrdup(s);
        break;
      default:
        break;
    }

    va_end(ap);

    /*
     * 如果还没有描述文本（非 FUNCTION_CMD 类型），
     * 自动生成描述文本。
     */
    if (!c->descriptions[n]) {
        c->descriptions[n] = get_command_description(c->cmds + n);
    }

    c->num++;

} /* add_command() */



/*
 * add_file_to_list() - 向文件列表追加一个文件路径。
 *
 * 如果提供了 directory 参数，则将 directory 和 filename 拼接为
 * "directory/filename" 形式的完整路径；否则直接复制 filename。
 *
 * 参数：
 *   directory - 目录前缀，NULL 表示 filename 已是完整路径
 *   filename  - 文件名或完整路径
 *   l         - 要追加到的文件列表
 */

static void add_file_to_list(const char *directory,
                             const char *filename, FileList *l)
{
    int len, n = l->num;

    /* 动态扩展文件名指针数组 */
    l->filename = (char **) nvrealloc(l->filename, sizeof(char *) * (n + 1));

    if (directory) {
        /* 拼接 "directory/filename" */
        len = strlen(filename) + strlen(directory) + 2;  /* +2: '/' 和 '\0' */
        l->filename[n] = (char *) nvalloc(len);
        snprintf(l->filename[n], len, "%s/%s", directory, filename);
    } else {
        /* 直接复制完整路径 */
        l->filename[n] = nvstrdup(filename);
    }
    l->num++;

} /* add_file_to_list() */


/*
 * append_to_rpm_file_list() - 将已安装文件的信息追加到 RPM 文件列表。
 *
 * 如果配置了 RPM 文件列表路径（op->rpm_file_list），
 * 将文件的权限和路径以 RPM spec 文件的 %attr 格式写入。
 * 格式：%attr (权限, root, root) 文件路径
 *
 * 这用于与 RPM 包管理器集成，使 RPM 能追踪 nvidia-installer
 * 安装的文件。
 *
 * 参数：
 *   op - 全局选项结构体（包含 rpm_file_list 路径）
 *   c  - 刚执行完的 INSTALL_CMD 命令（包含文件权限和目标路径）
 */
static void append_to_rpm_file_list(Options *op, Command *c)
{
    FILE *file;

    /* 如果未配置 RPM 文件列表，直接返回 */
    if (!op->rpm_file_list) return;

    /* 以追加模式打开文件，写入 RPM %attr 格式的条目 */
    file = fopen(op->rpm_file_list, "a");
    fprintf(file, "%%attr (%04o, root, root) %s\n", c->mode, c->target);
    fclose(file);
}
