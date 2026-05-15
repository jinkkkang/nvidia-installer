/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003-2010 NVIDIA Corporation
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
 * nvidia-installer.c
 *
 * 【文件说明】nvidia-installer 项目的主入口文件。
 *
 * 本文件包含 nvidia-installer 程序的 main() 函数以及核心启动逻辑。
 * nvidia-installer 是 NVIDIA 官方提供的 Unix/Linux 驱动安装工具，
 * 支持安装、升级和卸载 NVIDIA 加速图形驱动程序套件。
 *
 * 主要功能：
 *   1. main() - 程序入口，按顺序执行以下初始化流程：
 *      load_default_options → pci_device_scan → parse_commandline → log_init →
 *      adjust_cwd → ui_init → begin_initramfs_scan → set_concurrency_level →
 *      check_euid → find_system_utils → find_module_utils → check_selinux →
 *      check_systemd → query_xorg_version → get_default_prefixes_and_paths
 *      然后根据命令行选项执行对应操作（安装/卸载/完整性检查/驱动信息查询）。
 *
 *   2. parse_commandline() - 解析命令行参数，使用 option_table.h 中定义的
 *      __options[] 选项表，将解析结果填充到 Options 结构体中。
 *
 *   3. load_default_options() - 分配并初始化 Options 结构体，设置所有选项的默认值。
 *
 *   4. 辅助函数：print_version()、print_help()、print_options()、
 *      assign_file_type_override()、print_module_type()、
 *      print_recommended_module_type_option() 等。
 *
 * 依赖关系：
 *   - nvidia-installer.h：核心数据结构定义（Options、Package、PackageEntry 等）
 *   - option_table.h：命令行选项表定义（__options[] 数组）
 *   - kernel.h：内核模块构建相关（module_type_info、override_kernel_module_type 等）
 *   - misc.h：系统工具查找、PCI 设备扫描等工具函数
 *   - backup.h：卸载和驱动信息查询功能
 *   - files.h：安装路径和前缀计算
 *   - user-interface.h：用户界面初始化和交互
 *   - sanity.h：已安装驱动的完整性检查
 *   - manifest.h：.manifest 清单文件解析
 *   - initramfs.h：initramfs 扫描和重建
 */


#include <stdio.h>      /* printf、fprintf、stderr 等标准 I/O */
#include <stdlib.h>     /* exit、malloc、free 等标准库函数 */
#include <unistd.h>     /* umask、getuid 等 POSIX 函数 */
#include <limits.h>     /* PATH_MAX 等系统限制常量 */
#include <string.h>     /* strcmp、strdup、strchr、strcasecmp 等字符串操作 */
#include <ctype.h>      /* isalpha、isdigit 等字符分类函数 */
#include <libgen.h>     /* basename：从路径中提取文件名 */

#include <sys/types.h>  /* mode_t、pid_t 等系统类型定义 */
#include <sys/stat.h>   /* umask 所需的权限常量 */

#include <errno.h>      /* errno 错误码 */

#include "nvidia-installer.h"  /* 核心数据结构：Options、Package、PackageEntry 等 */
#include "kernel.h"            /* 内核模块构建：module_type_info、PROPRIETARY、OPEN 等 */
#include "user-interface.h"    /* UI 子系统：ui_init、ui_close、ui_error、ui_warn 等 */
#include "backup.h"            /* 卸载/备份：uninstall_existing_driver、report_driver_information */
#include "files.h"             /* 文件路径：get_default_prefixes_and_paths */
#include "misc.h"              /* 工具函数：find_system_utils、pci_device_scan、check_euid 等 */
#include "sanity.h"            /* 完整性检查：sanity() */
#include "option_table.h"      /* 命令行选项表：__options[] 数组及选项枚举定义 */
#include "msg.h"               /* 消息输出：nv_info_msg 等 */
#include "manifest.h"          /* 清单解析：parse_manifest_file_type */
#include "initramfs.h"         /* initramfs 扫描：begin_initramfs_scan */


/*
 * 静态函数前向声明
 */
static void print_version(void);
static void print_help(const char* name, int is_uninstall, int advanced);
static void print_module_type(char module_type);



/*
 * print_version() - 打印当前 nvidia-installer 的版本信息。
 *
 * 输出内容包括：
 *   - NV_ID_STRING：版本标识字符串（在构建时由宏定义生成，包含版本号）
 *   - 程序用途说明
 *   - 目标操作系统和架构（INSTALLER_OS 和 INSTALLER_ARCH 宏，如 "Linux-x86_64"）
 *
 * 通常在用户传入 --version / -v 选项时调用。
 */

static void print_version(void)
{
    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "%s", NV_ID_STRING);
    nv_info_msg(TAB, "The NVIDIA Software Installer for Unix/Linux.");
    nv_info_msg(NULL, "");
    nv_info_msg(TAB, "This program is used to install, upgrade and uninstall "
                "The NVIDIA Accelerated Graphics Driver Set for %s-%s.",
                INSTALLER_OS, INSTALLER_ARCH);
    nv_info_msg(NULL, "");
}


/*
 * print_help() - 打印帮助/用法信息。
 *
 * 下面包含三个相关函数：
 *   - print_help_helper()：格式化输出单个选项的名称和描述（作为回调函数传递给 nvgetopt_print_help）
 *   - print_options()：根据过滤条件输出选项列表
 *   - print_help()：输出完整的帮助信息（版本 + 用法 + 选项列表）
 */

/*
 * print_help_helper() - 格式化输出单个命令行选项的帮助文本。
 *
 * 作为回调函数传递给 nvgetopt_print_help()，由其在遍历选项表时逐项调用。
 *
 * @param name        选项名称字符串（如 "--kernel-source-path"）
 * @param description 选项描述文本
 */
static void print_help_helper(const char *name, const char *description)
{
    nv_info_msg(TAB, "%s", name);
    nv_info_msg(BIGTAB, "%s", description);
    nv_info_msg(NULL, "");
}

/*
 * print_options() - 按过滤条件输出命令行选项列表。
 *
 * 通过设置 include_mask 位掩码来控制输出哪些选项：
 *   - 当以 nvidia-uninstall 身份运行时，只输出带有 NVGETOPT_OPTION_APPLIES_TO_NVIDIA_UNINSTALL
 *     标志的选项（即适用于卸载的选项）
 *   - 当非高级模式时，只输出带有 NVGETOPT_HELP_ALWAYS 标志的选项（常用选项）
 *   - 高级模式（--advanced-options / -A）则输出所有选项
 *
 * @param is_uninstall 是否以卸载模式运行
 * @param advanced     是否输出高级选项
 */
static void print_options(int is_uninstall, int advanced)
{
    unsigned int include_mask = 0;

    if (is_uninstall) {
        /* 仅输出带有 UNINSTALL 标志的选项 */
        include_mask |= NVGETOPT_OPTION_APPLIES_TO_NVIDIA_UNINSTALL;
    }

    if (!advanced) {
        /* 仅输出带有 ALWAYS 标志的选项（即常用选项） */
        include_mask |= NVGETOPT_HELP_ALWAYS;
    }

    nvgetopt_print_help(__options, include_mask, print_help_helper);
}

/*
 * print_help() - 输出完整的帮助信息。
 *
 * 依次输出版本信息、用法格式和选项列表。
 * 在用户传入 --help / -h 或 --advanced-options / -A 时调用。
 *
 * @param name         程序名称（argv[0]），用于显示用法格式
 * @param is_uninstall 是否以卸载模式运行（影响输出哪些选项）
 * @param advanced     是否包含高级选项
 */
static void print_help(const char* name, int is_uninstall, int advanced)
{
    print_version();

    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "%s [options]", name);
    nv_info_msg(NULL, "");

    print_options(is_uninstall, advanced);
}


/*
 * load_default_options() - 分配 Options 结构体并初始化为默认值。
 *
 * 这是安装器初始化的第一步。分配一个零初始化的 Options 结构体，
 * 然后设置各选项的默认值。未在此函数中显式设置的字段默认为 0/NULL/FALSE
 * （因为 nvalloc 会将内存清零）。
 *
 * 返回值：
 *   - 成功：指向已初始化的 Options 结构体的指针
 *   - 失败（内存不足）：NULL
 *
 * 注意：返回的结构体需要调用 nvfree() 释放。
 */

static Options *load_default_options(void)
{
    Options *op;

    /* 分配并零初始化 Options 结构体 */
    op = (Options *) nvalloc(sizeof(Options));
    if (!op) {
        return NULL;
    }

    /* 设置静态初始化的字符串常量（指向编译时常量，不需要释放） */
    op->proc_mount_point = DEFAULT_PROC_MOUNT_POINT;  /* 默认 /proc */

    /* 获取临时目录路径，按优先级搜索：$TMPDIR → /tmp → . → $HOME */
    op->tmpdir = get_tmpdir(op);

    op->logging = TRUE;             /* 默认启用日志记录 */
    op->nvidia_modprobe = TRUE;     /* 默认安装 nvidia-modprobe（setuid root 工具，用于自动创建设备节点） */
    op->run_nvidia_xconfig = FALSE; /* 默认不自动运行 nvidia-xconfig 配置 X */
    op->selinux_option = SELINUX_DEFAULT; /* SELinux 处理策略：默认自动检测 */

    op->sigwinch_workaround = TRUE;       /* 默认启用 SIGWINCH 信号处理变通方案，防止子进程被终端大小变化信号干扰 */
    op->run_distro_scripts = TRUE;        /* 默认运行发行版特定的安装/卸载钩子脚本（位于 /usr/lib/nvidia/） */
    op->no_kernel_module_source = FALSE;  /* 默认安装内核模块源码（供 DKMS 或手动编译使用） */
    op->dkms = TRUE;                      /* 默认使用 DKMS 管理内核模块（若 DKMS 可用） */
    op->check_for_alternate_installs = TRUE; /* 默认检查是否存在其他安装方式（如发行版包管理器安装的驱动） */
    op->install_uvm = TRUE;              /* 默认安装 nvidia-uvm 模块（CUDA 统一虚拟内存，64 位系统必需） */
    op->install_drm = TRUE;              /* 默认安装 nvidia-drm 模块（DRM/KMS 支持，Wayland 必需） */
    op->install_peermem = TRUE;           /* 默认安装 nvidia-peermem 模块（GPUDirect RDMA 对等内存支持） */
    op->install_compat32_libs = NV_OPTIONAL_BOOL_DEFAULT;    /* 32 位兼容库：默认自动判断 */
    op->install_libglx_indirect = NV_OPTIONAL_BOOL_DEFAULT;  /* 间接 GLX 库：默认自动判断 */
    op->install_libglvnd_libraries = NV_OPTIONAL_BOOL_DEFAULT; /* GLVND 库：默认自动判断（若系统已有则跳过） */
    op->external_platform_json_path = DEFAULT_EGL_EXTERNAL_PLATFORM_JSON_PATH; /* EGL 外部平台 JSON 配置路径 */
    op->skip_depmod = FALSE;              /* 默认在安装/卸载内核模块后运行 depmod 更新依赖 */
    op->use_systemd = NV_OPTIONAL_BOOL_DEFAULT; /* systemd 支持：默认自动检测（若 systemctl 可用则启用） */
    op->rebuild_initramfs = NV_OPTIONAL_BOOL_DEFAULT; /* initramfs 重建：默认自动判断 */
    op->disable_nouveau = TRUE;           /* 默认自动禁用 nouveau 开源驱动（与 NVIDIA 专有驱动冲突） */

    return op;

} /* load_default_options() */


/*
 * assign_file_type_override() - 解析 --override-file-type-destination 选项的参数。
 *
 * 该选项允许用户覆盖特定文件类型的默认安装目标路径。
 * 参数格式为 "<FILE_TYPE>:<destination>"，例如：
 *   --override-file-type-destination "OPENGL_LIB:/opt/nvidia/lib"
 *
 * 处理流程：
 *   1. 在参数字符串中查找第一个 ':' 分隔符
 *   2. 将 ':' 替换为 '\0'，将参数分割为文件类型名称和目标路径两部分
 *   3. 调用 parse_manifest_file_type() 将文件类型名称解析为 PackageEntryFileType 枚举值
 *   4. 将目标路径复制并存储到 Options 结构体的 file_type_destination_overrides 数组中
 *
 * @param op      Options 结构体指针，解析结果将写入其 file_type_destination_overrides 数组
 * @param optarg  命令行参数字符串（格式："<FILE_TYPE>:<destination>"），注意此字符串会被原地修改
 *
 * @return TRUE  解析成功
 * @return FALSE 解析失败（缺少 ':' 分隔符，或文件类型名称无法识别）
 */
static int assign_file_type_override(Options *op, char *optarg)
{
    PackageEntryFileCapabilities dummy;  /* 仅用于满足 parse_manifest_file_type 的参数要求，返回值被忽略 */
    PackageEntryFileType type;
    char *split;

    /*
     * 在参数字符串中查找 ':' 分隔符，将其替换为 '\0' 以分割字符串：
     * 前半部分为文件类型名称，后半部分为目标路径。
     */
    split = strchr(optarg, ':');
    if (split == NULL) {
        return FALSE;
    }
    split[0] = '\0';
    split++;

    /* 将文件类型名称字符串解析为枚举值（如 "OPENGL_LIB" → FILE_TYPE_OPENGL_LIB） */
    type = parse_manifest_file_type(optarg, &dummy);
    if (type == FILE_TYPE_NONE) {
        return FALSE;
    }

    /* 将目标路径复制一份，存入对应文件类型的覆盖数组槽位 */
    op->file_type_destination_overrides[type] = nvstrdup(split);

    return TRUE;
}

/*
 * print_module_type() - 输出内核模块类型的名称字符串。
 *
 * NVIDIA 驱动支持两种内核模块类型：
 *   - PROPRIETARY（'p'）：闭源/专有内核模块
 *   - OPEN（'o'）：开源内核模块（kernel-open，基于 GPL 许可）
 *
 * @param module_type 模块类型字符（PROPRIETARY 或 OPEN，定义在 kernel.h 中）
 */
static void print_module_type(char module_type)
{
    switch (module_type) {
        case PROPRIETARY:
            nv_info_msg(NULL, "proprietary");
            break;
        case OPEN:
            nv_info_msg(NULL, "open");
            break;
    }
}

/*
 * print_recommended_module_type_option() - 输出推荐的内核模块类型并退出。
 *
 * 根据当前系统中检测到的 GPU 硬件，判断应使用开源还是闭源内核模块。
 * 此函数在用户传入 --print-recommended-kernel-module-type 选项时调用。
 *
 * 处理流程：
 *   1. 调用 valid_kernel_module_types() 查询当前系统支持的模块类型
 *   2. 从返回的 module_type_info 结构体中获取推荐的默认类型
 *   3. 调用 print_module_type() 输出推荐类型名称
 *
 * @param op  Options 结构体指针（需要其中的 PCI 设备检测结果）
 *
 * @return TRUE  成功输出推荐类型
 * @return FALSE 无法确定推荐类型（例如未检测到 GPU）
 */
static int print_recommended_module_type_option(Options *op)
{
    int num_types;
    struct module_type_info info;

    /* 查询当前系统支持哪些内核模块类型，以及推荐使用哪种 */
    num_types = valid_kernel_module_types(op, &info, TRUE);

    if (num_types <= 0) {
        ui_error(op, "Unable to determine recommended module type.");
        return FALSE;
    }

    /* 输出推荐的模块类型（info.default_entry 指向推荐的类型索引） */
    print_module_type(info.types[info.default_entry]);
    return TRUE;
}

/*
 * parse_commandline() - 解析命令行参数，将结果填充到 Options 结构体中。
 *
 * 使用 option_table.h 中定义的 __options[] 选项表，通过 nvgetopt() 循环解析
 * 所有命令行参数。每个选项对应一个 case 分支，将解析到的值赋给 Options 结构体
 * 中的相应字段。
 *
 * 设计原则：
 *   - 此函数仅做最小限度的合法性检查（确保命令行语法正确）
 *   - 参数值的实际有效性验证留给后续使用这些数据的函数负责
 *   - 某些互斥选项（如 --kernel-modules-only 和 --no-kernel-modules）在此处处理冲突
 *
 * 特殊行为：
 *   - 如果程序名为 "nvidia-uninstall"，则自动进入卸载模式
 *   - --help、--version 等信息输出选项会在解析完成后执行并退出
 *   - 解析失败时会输出错误信息并调用 exit(1) 退出
 *
 * @param argc  命令行参数个数
 * @param argv  命令行参数字符串数组
 * @param op    已初始化的 Options 结构体指针（由 load_default_options 创建）
 */

static void parse_commandline(int argc, char *argv[], Options *op)
{
    int c;                                              /* 当前解析到的选项字符/枚举值 */
    int print_help_after = FALSE;                       /* 延迟标志：解析完成后是否打印帮助信息 */
    int print_recommended_kernel_module_after = FALSE;  /* 延迟标志：解析完成后是否打印推荐的模块类型 */
    int print_help_args_only_after = FALSE;             /* 延迟标志：解析完成后是否仅打印选项列表（供 makeself.sh 使用） */
    int print_advanced_help = FALSE;                    /* 是否包含高级选项 */
    char *strval = NULL, *program_name = NULL;          /* strval：字符串类型选项的值 */
    int boolval;                                        /* 布尔类型选项的值 */
    int intval = 0;                                     /* 整数类型选项的值 */

    /*
     * 检查程序是否以 "nvidia-uninstall" 名称被调用。
     * nvidia-installer 二进制文件可以通过符号链接以 "nvidia-uninstall" 名称运行，
     * 此时自动进入卸载模式（等效于传入 --uninstall 选项）。
     * 注意：strdup 后再 basename，避免修改 argv[0] 原始字符串。
     */
    program_name = strdup(argv[0]);
    if (strcmp(basename(program_name), "nvidia-uninstall") == 0) {
        op->uninstall = TRUE;
    }
    free(program_name);

    /*
     * 主解析循环：调用 nvgetopt() 逐个解析命令行选项。
     * nvgetopt() 是 NVIDIA 自定义的 getopt 实现（定义在 nvgetopt.h/c 中），
     * 支持长选项、布尔选项、字符串/整数参数等。
     * 返回 -1 表示所有选项已解析完毕。
     */
    while (1) {

        c = nvgetopt(argc, argv, __options, &strval, &boolval, &intval,
                     NULL,  /* doubleval：本程序不使用浮点参数 */
                     NULL); /* disable_val：本程序不使用禁用值 */

        if (c == -1)
            break;

        switch (c) {

        case 'a': /* --accept-license：已废弃，保留仅为向后兼容，忽略 */ break;
        case 'e': op->expert = TRUE; break;        /* --expert：启用专家模式 */
        case 'v': print_version(); exit(0); break;  /* --version：打印版本后退出 */
        case 'd': op->debug = TRUE; break;          /* --debug：启用调试输出 */
        case 'i':
            /* --driver-info：仅显示驱动信息，不需要 UI，设置为 "none" */
            op->driver_info = TRUE;
            op->ui.name = "none";
            break;
        case 'n': op->no_precompiled_interface = TRUE; break; /* --no-precompiled-interface */
        case 'c': op->no_ncurses_color = TRUE; break;         /* --no-ncurses-color */
        case 'h': print_help_after = TRUE; break;              /* --help：延迟到解析完成后打印 */
        case 'A':
            /* --advanced-options：延迟打印，包含高级选项 */
            print_help_after = TRUE;
            print_advanced_help = TRUE;
            break;
        case 'q': op->no_questions = TRUE; break;   /* --no-questions：不提问 */
        case 'b': op->no_backup = TRUE; break;      /* --no-backup：不备份被替换的文件 */
        case 'K':
            /* --kernel-modules-only：仅安装内核模块；与 --no-kernel-modules 互斥 */
            op->kernel_modules_only = TRUE;
            op->no_kernel_modules = FALSE; /* 清除冲突标志 */
            break;
        case 'X': op->run_nvidia_xconfig = TRUE; break; /* --run-nvidia-xconfig */
        case 's':
            /* --silent：静默模式，同时隐含 --no-questions 和 --ui=none */
            op->silent = op->no_questions = TRUE;
            op->ui.name = "none";
            break;
        case 'z': op->no_nouveau_check = TRUE; break;   /* --no-nouveau-check */
        case 'Z': op->disable_nouveau = boolval; break;  /* --disable-nouveau / --no-disable-nouveau */
        case 'k':
            /*
             * --kernel-name：指定目标内核版本名（如 "5.15.0-generic"），
             * 用于为非当前运行的内核构建模块。
             * 隐含 --no-precompiled-interface（因为预编译接口是针对运行内核的）。
             */
            op->kernel_name = strval;
            op->no_precompiled_interface = TRUE;
            break;
        case 'j':
            /*
             * --concurrency-level：设置并行编译级别（对应 make -j 参数）。
             * 若用户指定的值小于 1，发出警告并重置为 0（后续会自动检测 CPU 数量）。
             */
            if (intval < 1) {
                ui_error(op, "Invalid concurrency level %d: nvidia-installer "
                             "will attempt to autodetect the number of CPUs.",
                             intval);
                intval = 0;
            }
            op->concurrency_level = intval;
            break;
            
        /*
         * ===== 安装路径相关选项 =====
         * 以下选项允许用户自定义各类文件的安装路径前缀和子目录。
         * 大多数用户不需要修改这些路径，仅在特殊环境下使用。
         */

        case XFREE86_PREFIX_OPTION:     /* --xfree86-prefix：已废弃的同义词 */
        case X_PREFIX_OPTION:           /* --x-prefix：X.Org 安装前缀 */
            op->x_prefix = strval; break;
        case X_LIBRARY_PATH_OPTION:     /* --x-library-path：X.Org 库安装路径 */
            op->x_library_path = strval; break;
        case X_MODULE_PATH_OPTION:      /* --x-module-path：X.Org 模块安装路径 */
            op->x_module_path = strval; break;
        case X_SYSCONFIG_PATH_OPTION:   /* --x-sysconfig-path：X.Org 系统配置路径 */
            op->x_sysconfig_path = strval; break;
        case OPENGL_PREFIX_OPTION:      /* --opengl-prefix：OpenGL 库安装前缀 */
            op->opengl_prefix = strval; break;
        case OPENGL_LIBDIR_OPTION:      /* --opengl-libdir：OpenGL 库相对子目录 */
            op->opengl_libdir = strval; break;
        case WINE_PREFIX_OPTION:        /* --wine-prefix：Wine 兼容库安装前缀 */
            op->wine_prefix = strval; break;
        case WINE_LIBDIR_OPTION:        /* --wine-libdir：Wine 兼容库相对子目录 */
            op->wine_libdir = strval; break;

#if defined(NV_X86_64)
        /* 以下 32 位兼容库选项仅在 x86_64 架构上可用 */
        case COMPAT32_CHROOT_OPTION:    /* --compat32-chroot：32 位兼容库的 chroot 前缀 */
            op->compat32_chroot = strval; break;
        case COMPAT32_PREFIX_OPTION:    /* --compat32-prefix：32 位兼容库安装前缀 */
            op->compat32_prefix = strval; break;
        case COMPAT32_LIBDIR_OPTION:    /* --compat32-libdir：32 位兼容库相对子目录 */
            op->compat32_libdir = strval; break;
        case INSTALL_COMPAT32_LIBS_OPTION:
            /* --install-compat32-libs / --no-install-compat32-libs：显式控制是否安装 32 位兼容库 */
            op->install_compat32_libs = boolval ? NV_OPTIONAL_BOOL_TRUE :
                                                  NV_OPTIONAL_BOOL_FALSE;
            break;
#endif

        case DOCUMENTATION_PREFIX_OPTION:       /* --documentation-prefix：文档安装前缀 */
            op->documentation_prefix = strval; break;
        case APPLICATION_PROFILE_PATH_OPTION:   /* --application-profile-path：应用性能配置文件路径 */
            op->application_profile_path = strval; break;
        case INSTALLER_PREFIX_OPTION:           /* --installer-prefix：安装器自身的安装前缀（已废弃，建议用 --utility-prefix） */
            op->installer_prefix = strval; break;
        case UTILITY_PREFIX_OPTION:             /* --utility-prefix：工具程序安装前缀 */
            op->utility_prefix = strval; break;
        case UTILITY_LIBDIR_OPTION:             /* --utility-libdir：工具程序库相对子目录 */
            op->utility_libdir = strval; break;
        case XDG_DATA_DIR_OPTION:               /* --xdg-data-dir：XDG 数据文件目录（.desktop、图标等） */
            op->xdg_data_dir = strval; break;

        /*
         * ===== 内核相关路径选项 =====
         */

        case KERNEL_SOURCE_PATH_OPTION:     /* --kernel-source-path：内核源码路径 */
            op->kernel_source_path = strval; break;
        case KERNEL_OUTPUT_PATH_OPTION:     /* --kernel-output-path：内核构建输出路径（KBUILD_OUTPUT） */
            op->kernel_output_path = strval; break;
        case KERNEL_INCLUDE_PATH_OPTION:    /* --kernel-include-path：内核头文件路径（已废弃，建议用 --kernel-source-path） */
            op->kernel_include_path = strval; break;
        case KERNEL_INSTALL_PATH_OPTION:    /* --kernel-install-path：内核模块安装目标路径 */
            op->kernel_module_installation_path = strval; break;

        /*
         * ===== 操作模式选项 =====
         */

        case UNINSTALL_OPTION:              /* --uninstall：执行卸载 */
            op->uninstall = TRUE; break;
        case SKIP_MODULE_UNLOAD_OPTION:     /* --skip-module-unload：卸载时不卸载内核模块 */
            op->skip_module_unload = TRUE; break;
        case SKIP_MODULE_LOAD_OPTION:       /* --skip-module-load：安装后不加载内核模块 */
            op->skip_module_load = TRUE; break;

        /*
         * ===== 杂项选项 =====
         */

        case PROC_MOUNT_POINT_OPTION:       /* --proc-mount-point：/proc 挂载点（默认 /proc） */
            op->proc_mount_point = strval; break;
        case USER_INTERFACE_OPTION:         /* --ui：指定用户界面类型（"ncurses" 或 "none"） */
            op->ui.name = strval; break;
        case LOG_FILE_NAME_OPTION:          /* --log-file-name：安装日志文件路径 */
            op->log_file_name = strval; break;
        case HELP_ARGS_ONLY_OPTION:
            /*
             * --help-args-only：仅输出选项列表（不含版本信息和用法格式），
             * 供 makeself.sh 自解压包生成脚本调用。
             */
            print_help_args_only_after = TRUE;
            break;
        case TMPDIR_OPTION:                 /* --tmpdir：指定临时目录 */
            op->tmpdir = strval; break;
        case NO_NVIDIA_MODPROBE_OPTION:     /* --no-nvidia-modprobe：不安装 nvidia-modprobe 工具 */
            op->nvidia_modprobe = FALSE; break;

        /*
         * ===== 已废弃选项 =====
         * 以下选项不再使用，但为保持向后兼容仍被接受（并输出警告）。
         */

        case FORCE_TLS_OPTION:
            /* --force-tls：TLS 库强制选项，已废弃 */
            ui_warn(op, "The '--force-tls' option is deprecated:  "
                        "nvidia-installer will ignore this option.");
            break;
        case FORCE_TLS_COMPAT32_OPTION:
            /* --force-tls-compat32：32 位 TLS 库强制选项，已废弃 */
            ui_warn(op, "The '--force-tls-compat32' option is deprecated:  "
                        "nvidia-installer will ignore this option.");
            break;

        case SANITY_OPTION:                 /* --sanity：执行现有安装的完整性检查 */
            op->sanity = TRUE;
            break;
        case ADD_THIS_KERNEL_OPTION:        /* --add-this-kernel：为当前内核添加预编译接口 */
            op->add_this_kernel = TRUE;
            break;
        case ADVANCED_OPTIONS_ARGS_ONLY_OPTION:
            /* --advanced-options-args-only：仅输出高级选项列表（供脚本调用） */
            print_help_args_only_after = TRUE;
            print_advanced_help = TRUE;
            break;
        case RPM_FILE_LIST_OPTION:          /* --rpm-file-list：RPM 文件列表路径（用于冲突检测） */
            op->rpm_file_list = strval;
            break;
        case NO_RUNLEVEL_CHECK_OPTION:
            /* --no-runlevel-check：运行级别检查选项，已废弃 */
            ui_warn(op, "The '--no-runlevel-check' option is deprecated:  "
                        "nvidia-installer will ignore this option.");
            break;
        case 'N':
            /* --no-network：网络检查选项，已废弃 */
            ui_warn(op, "The '--no-network' option is deprecated:  "
                        "nvidia-installer will ignore this option.");
            break;
        case PRECOMPILED_KERNEL_INTERFACES_PATH_OPTION:
            /* --precompiled-kernel-interfaces-path：预编译内核接口的额外搜索路径 */
            op->precompiled_kernel_interfaces_path = strval;
            break;
        case NO_ABI_NOTE_OPTION:            /* --no-abi-note：移除 OpenGL 库中的 ABI 注释标记 */
            op->no_abi_note = TRUE;
            break;
        case NO_RPMS_OPTION:                /* --no-rpms：不检查冲突 RPM 包 */
            op->no_rpms = TRUE;
            break;
        case 'r':                           /* --no-recursion：不递归搜索冲突文件 */
            op->no_recursion = TRUE;
            break;

        case FORCE_SELINUX_OPTION:
            /*
             * --force-selinux：强制指定 SELinux 处理策略。
             * 参数值：
             *   "yes"     - 强制设置 SELinux 安全上下文
             *   "no"      - 强制不设置 SELinux 安全上下文
             *   "default" - 自动检测（安装器默认行为）
             *
             * 注意：此处的 strcasecmp 检查 "default" 时使用了不带 == 0 的条件，
             * 这意味着当参数既不是 "yes" 也不是 "no" 也不是 "default" 时才报错。
             * 这是正确的逻辑（对 "default" 不做任何操作，因为 SELINUX_DEFAULT 已是默认值）。
             */
            if (strcasecmp(strval, "yes") == 0)
                op->selinux_option = SELINUX_FORCE_YES;
            else if (strcasecmp(strval, "no") == 0)
                op->selinux_option = SELINUX_FORCE_NO;
            else if (strcasecmp(strval, "default")) {
                ui_error(op, "Invalid parameter for '--force-selinux'");
                goto fail;
            }
            break;
        case SELINUX_CHCON_TYPE_OPTION:     /* --selinux-chcon-type：指定 chcon 使用的安全上下文类型 */
            op->selinux_chcon_type = strval; break;
        case NO_SIGWINCH_WORKAROUND_OPTION: /* --no-sigwinch-workaround：禁用 SIGWINCH 处理变通方案 */
            op->sigwinch_workaround = FALSE;
            break;
        case NO_KERNEL_MODULES_OPTION:
            /*
             * --no-kernel-modules：不安装内核模块。
             * 与 --kernel-modules-only 互斥，同时也隐含 --no-kernel-module-source。
             */
            op->no_kernel_modules = TRUE;
            op->kernel_modules_only = FALSE; /* 清除冲突标志 */
            op->no_kernel_module_source = TRUE;
            break;
        case NO_X_CHECK_OPTION:             /* --no-x-check：不检测 X Server 是否运行 */
            op->no_x_check = TRUE;
            break;
        case NO_CC_VERSION_CHECK_OPTION:
            /* --no-cc-version-check：已废弃，静默忽略 */
            break;
        case NO_DISTRO_SCRIPTS_OPTION:      /* --no-distro-scripts：不运行发行版钩子脚本 */
            op->run_distro_scripts = FALSE;
            break;
        case NO_OPENGL_FILES_OPTION:        /* --no-opengl-files：不安装 OpenGL 相关文件 */
            op->no_opengl_files = TRUE;
            break;
        case NO_WINE_FILES_OPTION:          /* --no-wine-files：不安装 Wine 兼容文件 */
            op->no_wine_files = TRUE;
            break;

        /*
         * ===== 内核模块源码安装选项 =====
         */

        case KERNEL_MODULE_SOURCE_PREFIX_OPTION:    /* --kernel-module-source-prefix：源码安装前缀（默认 /usr/src） */
            op->kernel_module_src_prefix = strval;
            break;
        case KERNEL_MODULE_SOURCE_DIR_OPTION:       /* --kernel-module-source-dir：源码目录名（默认 "nvidia-VERSION"） */
            op->kernel_module_src_dir = strval;
            break;
        case NO_KERNEL_MODULE_SOURCE_OPTION:        /* --no-kernel-module-source：不安装内核模块源码 */
            op->no_kernel_module_source = TRUE;
            break;
        case DKMS_OPTION:                           /* --dkms / --no-dkms：是否使用 DKMS 管理内核模块 */
            op->dkms = boolval;
            break;

        /*
         * ===== 内核模块签名选项（Secure Boot 支持） =====
         * 启用 Secure Boot 的系统要求内核模块必须经过签名才能加载。
         * 以下选项允许用户指定签名所需的密钥和工具。
         */

        case MODULE_SIGNING_SECRET_KEY_OPTION:      /* --module-signing-secret-key：签名私钥路径 */
            op->module_signing_secret_key = strval;
            break;
        case MODULE_SIGNING_PUBLIC_KEY_OPTION:      /* --module-signing-public-key：签名公钥/证书路径 */
            op->module_signing_public_key = strval;
            break;
        case MODULE_SIGNING_SCRIPT_OPTION:          /* --module-signing-script：自定义签名脚本路径 */
            op->module_signing_script = strval;
            break;
        case MODULE_SIGNING_KEY_PATH_OPTION:        /* --module-signing-key-path：签名密钥存储/生成路径 */
            op->module_signing_key_path = strval;
            break;
        case MODULE_SIGNING_HASH_OPTION:            /* --module-signing-hash：签名哈希算法（如 sha256） */
            op->module_signing_hash = strval;
            break;
        case MODULE_SIGNING_X509_HASH_OPTION:       /* --module-signing-x509-hash：X.509 证书哈希算法 */
            op->module_signing_x509_hash = strval;
            break;

        case INSTALL_VDPAU_WRAPPER_OPTION:
            /*
             * --install-vdpau-wrapper：安装 VDPAU 包装器库。
             * 当前驱动包不再包含预编译的 libvdpau，因此若用户显式请求安装（boolval=TRUE），
             * 则报错退出；若用户指定 --no-install-vdpau-wrapper 则静默接受。
             */
            if (boolval) {
                ui_error(op, "This driver package does not contain a "
                             "pre-compiled copy of libvdpau.  Please see the "
                             "README for instructions on how to install "
                             "libvdpau.");
                goto fail;
            }
            break;

        /*
         * ===== 可选内核模块控制选项 =====
         */

        case NO_UVM_OPTION:             /* --no-unified-memory：不安装 nvidia-uvm 模块（禁用 CUDA） */
            op->install_uvm = FALSE;
        break;
        case NO_DRM_OPTION:             /* --no-drm：不安装 nvidia-drm 模块（禁用 DRM/KMS） */
            op->install_drm = FALSE;
        break;
        case NO_PEERMEM_OPTION:         /* --no-peermem：不安装 nvidia-peermem 模块（禁用 GPUDirect RDMA） */
            op->install_peermem = FALSE;
        break;
        case NO_CHECK_FOR_ALTERNATE_INSTALLS_OPTION:
            /* --no-check-for-alternate-installs：跳过检查其他安装方式 */
            op->check_for_alternate_installs = FALSE;
        break;

        /*
         * ===== OpenGL/GLVND 相关选项 =====
         */

        case FORCE_LIBGLX_INDIRECT:     /* --force-libglx-indirect：强制安装间接 GLX 符号链接 */
            op->install_libglx_indirect = NV_OPTIONAL_BOOL_TRUE;
            break;
        case NO_LIBGLX_INDIRECT:        /* --no-libglx-indirect：不安装间接 GLX 符号链接 */
            op->install_libglx_indirect = NV_OPTIONAL_BOOL_FALSE;
            break;
        case INSTALL_LIBGLVND_OPTION:
            /* --install-libglvnd / --no-install-libglvnd：显式控制是否安装 GLVND 库 */
            op->install_libglvnd_libraries = boolval ? NV_OPTIONAL_BOOL_TRUE :
                                                       NV_OPTIONAL_BOOL_FALSE;
            break;
        case GLVND_GLX_CLIENT_OPTION:
            /*
             * --glvnd-glx-client：已废弃。
             * --no-glvnd-glx-client 不再支持（GLVND GLX 现在是必需的），若用户尝试禁用则报错。
             */
            if (!boolval) {
                ui_error(op, "The --no-glvnd-glx-client option is no longer supported");
                goto fail;
            }

            ui_warn(op, "The '--glvnd-glx-client' option is deprecated:  "
                        "nvidia-installer will ignore this option.");
            break;
        case GLVND_EGL_CONFIG_FILE_PATH_OPTION:
            /* --glvnd-egl-config-path：GLVND EGL 厂商 JSON 配置文件路径 */
            op->libglvnd_json_path = strval;
            break;
        case GLVND_EGL_CLIENT_OPTION:
            /*
             * --glvnd-egl-client：已废弃。
             * 与上面 GLVND_GLX_CLIENT_OPTION 类似的处理逻辑。
             */
            if (!boolval) {
                ui_error(op, "The --no-glvnd-egl-client option is no longer supported");
                goto fail;
            }

            ui_warn(op, "The '--glvnd-egl-client' option is deprecated:  "
                        "nvidia-installer will ignore this option.");
            break;
        case EGL_EXTERNAL_PLATFORM_CONFIG_FILE_PATH_OPTION:
            /* --egl-external-platform-config-path：EGL 外部平台 JSON 配置路径 */
            op->external_platform_json_path = strval;
            break;

        case OVERRIDE_FILE_TYPE_DESTINATION_OPTION:
            /* --override-file-type-destination：覆盖指定文件类型的安装路径 */
            if (!assign_file_type_override(op, strval)) {
                goto fail;
            }
            break;
        case SKIP_DEPMOD_OPTION:        /* --skip-depmod：安装后不运行 depmod */
            op->skip_depmod = TRUE;
            break;

        /*
         * ===== systemd 相关选项 =====
         */

        case SYSTEMD_OPTION:            /* --systemd / --no-systemd：是否安装 systemd 服务单元 */
            op->use_systemd = boolval ? NV_OPTIONAL_BOOL_TRUE :
                                        NV_OPTIONAL_BOOL_FALSE;
            break;
        case SYSTEMD_UNIT_PREFIX_OPTION:     /* --systemd-unit-prefix：systemd 单元文件安装路径 */
            op->systemd_unit_prefix = strval;
            break;
        case SYSTEMD_SLEEP_PREFIX_OPTION:    /* --systemd-sleep-prefix：systemd 睡眠脚本安装路径 */
            op->systemd_sleep_prefix = strval;
            break;
        case SYSTEMD_SYSCONF_PREFIX_OPTION:  /* --systemd-sysconf-prefix：systemd 系统配置路径 */
            op->systemd_sysconf_prefix = strval;
            break;

        /*
         * ===== 内核模块类型选项 =====
         */

        case 'm':
            /*
             * --kernel-module-build-directory（-m）：直接指定内核模块构建目录。
             * 已废弃，建议使用 --kernel-module-type（-M）代替。
             * 调用 override_kernel_module_build_directory() 处理，失败则退出。
             */
            if (!override_kernel_module_build_directory(op, strval)) {
                goto fail;
            }
            break;
        case 'M':
            /*
             * --kernel-module-type（-M）：指定内核模块类型。
             * 有效值："open"（开源模块）或 "proprietary"（闭源模块）。
             * 调用 override_kernel_module_type() 处理，失败则退出。
             */
            if (!override_kernel_module_type(op, strval)) {
                goto fail;
            }
            break;
        case PRINT_RECOMMENDED_MODULE_TYPE_OPTION:
            /* --print-recommended-kernel-module-type：延迟到解析完成后输出推荐的模块类型 */
            print_recommended_kernel_module_after = TRUE;
            break;

        case ALLOW_INSTALLATION_WITH_RUNNING_DRIVER_OPTION:
            /* --allow-installation-with-running-driver：允许在 NVIDIA 驱动运行时安装 */
            op->allow_installation_with_running_driver = boolval;
            break;
        case REBUILD_INITRAMFS_OPTION:
            /* --rebuild-initramfs / --no-rebuild-initramfs：显式控制是否重建 initramfs */
            op->rebuild_initramfs = boolval ? NV_OPTIONAL_BOOL_TRUE :
                                              NV_OPTIONAL_BOOL_FALSE;
            break;
        case GBM_BACKEND_DIR_OPTION:        /* --gbm-backend-dir：GBM 后端库安装路径 */
            op->gbm_backend_dir = strval;
            break;
        case GBM_BACKEND_DIR32_OPTION:      /* --gbm-backend-dir32：32 位 GBM 后端库安装路径 */
            op->compat32_gbm_backend_dir = strval;
            break;

        default:
            /* 未识别的选项，跳转到错误处理 */
            goto fail;
        }

    }

    /*
     * ===== 延迟执行的信息输出操作 =====
     * 某些选项（如 --help、--print-recommended-kernel-module-type）需要在
     * 所有命令行参数解析完成后才执行，以确保其他选项（如 --uninstall）
     * 已被正确设置，从而影响输出内容。
     */

    if (print_help_after) {
        /* 输出帮助信息后退出 */
        print_help(argv[0], op->uninstall, print_advanced_help);
        exit(0);
    }

    if (print_help_args_only_after) {
        /*
         * 输出选项列表供 makeself.sh 使用。
         * 不按当前终端宽度格式化输出，而是硬编码为 65 列宽度，
         * 因为该输出会被嵌入到自解压包脚本的帮助文本中。
         */
        reset_current_terminal_width(65);

        print_options(op->uninstall, print_advanced_help);
        exit(0);
    }

    if (print_recommended_kernel_module_after) {
        /* 输出推荐的内核模块类型后退出 */
        if (!print_recommended_module_type_option(op)) {
            goto fail;
        }
        exit(0);
    }

    /*
     * 如果未显式指定 --installer-prefix，则将其默认设置为 --utility-prefix 的值。
     * 这样做的目的是：installer_prefix 优先级更高（若设置了就用它），
     * 但若未设置，则回退到 utility_prefix（两者通常是相同的）。
     */

    if (!op->installer_prefix) {
        op->installer_prefix = op->utility_prefix;
    }

    /*
     * 设置默认的日志文件路径。
     * 延迟到命令行解析完成后才设置，因为需要先确定是安装模式还是卸载模式，
     * 两者使用不同的日志文件名：
     *   - 安装模式：/var/log/nvidia-installer.log
     *   - 卸载模式：/var/log/nvidia-uninstall.log
     */

    if (!op->log_file_name) {
        op->log_file_name = op->uninstall ? DEFAULT_UNINSTALL_LOG_FILE_NAME :
                                            DEFAULT_LOG_FILE_NAME;
    }

    return;

 fail:
    /*
     * 命令行解析失败的统一错误处理。
     * 输出错误提示，释放 Options 结构体内存，以退出码 1 终止程序。
     */
    ui_error(op, "Invalid commandline, please run `%s --help` "
                 "for usage information.", argv[0]);
    nvfree((void*)op);
    exit(1);
} /* parse_commandline() */



/*
 * main() - 程序主入口。
 *
 * 整体执行流程分为三个阶段：
 *
 * 【第一阶段：初始化】
 *   1. umask(022)        - 设置文件创建掩码，确保新创建的文件权限正确
 *   2. load_default_options() - 分配并初始化 Options 结构体（默认值）
 *   3. pci_device_scan()      - 扫描系统 PCI 设备，检测 NVIDIA GPU 硬件
 *   4. parse_commandline()    - 解析命令行参数，覆盖默认选项
 *   5. log_init()             - 初始化日志系统，打开日志文件
 *   6. adjust_cwd()           - 将工作目录切换到程序二进制文件所在目录
 *   7. ui_init()              - 初始化用户界面（ncurses 或 printf/scanf）
 *   8. begin_initramfs_scan() - 启动 initramfs 内容扫描（异步）
 *   9. set_concurrency_level()- 确定并行编译级别（基于 CPU 数量）
 *
 * 【第二阶段：环境检测】
 *   10. check_euid()           - 检查是否以 root 身份运行
 *   11. find_system_utils()    - 搜索必需的系统工具（ldconfig、grep、dmesg、tail）
 *   12. find_module_utils()    - 搜索内核模块管理工具（modprobe、rmmod、lsmod、depmod）
 *   13. check_selinux()        - 检测 SELinux 状态
 *   14. check_systemd()        - 检测 systemd 是否可用
 *   15. query_xorg_version()   - 查询 X.Org Server 版本
 *   16. get_default_prefixes_and_paths() - 根据系统配置确定默认安装路径
 *
 * 【第三阶段：执行操作】
 *   根据命令行选项执行对应操作（互斥，按优先级排列）：
 *   - --driver-info：显示已安装驱动的版本信息
 *   - --sanity：执行现有安装的完整性检查
 *   - --uninstall：卸载已安装的驱动程序
 *   - --add-this-kernel：为当前内核添加预编译接口
 *   - 默认：从当前目录执行安装流程
 *
 * 返回值：
 *   0 - 操作成功
 *   1 - 操作失败（或初始化阶段出错）
 */

int main(int argc, char *argv[])
{
    Options *op;
    int ret = FALSE;  /* 操作结果：TRUE=成功，FALSE=失败 */

    /*
     * 设置文件创建掩码为 022，确保安装器创建的文件权限为：
     *   - 普通文件：644（rw-r--r--）
     *   - 目录：755（rwxr-xr-x）
     * 这防止了因用户环境中不安全的 umask 设置而导致安装的文件权限过于宽松。
     */
    umask(022);

    /* 第一步：加载默认配置 */

    op = load_default_options();
    if (!op) {
        fprintf(stderr, "\nOut of memory error.\n\n");
        return 1;
    }

    /*
     * 第二步：扫描 PCI 总线上的 NVIDIA 设备。
     * 需要在解析命令行之前执行，因为某些选项的默认行为
     * 取决于检测到的 GPU 类型（如开源 vs 闭源模块推荐）。
     */
    pci_device_scan(op);

    /* 第三步：解析命令行选项，将用户指定的值覆盖到 Options 结构体中 */

    parse_commandline(argc, argv, op);

    /*
     * 第四步：初始化日志系统。
     * 打开日志文件（由 op->log_file_name 指定），
     * 记录程序启动信息和完整的命令行参数。
     */

    log_init(op, argc, argv);

    /*
     * 第五步：将工作目录切换到包含程序二进制文件（及 .manifest 文件）的目录。
     * 安装操作需要从该目录读取驱动包文件。
     */

    if (!adjust_cwd(op, argv[0])) return 1;

    /*
     * 第六步：初始化用户界面。
     * 根据 op->ui.name 加载对应的 UI 库（ncurses 或 none/stream）。
     * 失败时无法继续安装（因为无法与用户交互）。
     */

    if (!ui_init(op)) return 1;

    /*
     * 第七步：启动 initramfs 扫描。
     * 异步扫描当前 initramfs 的内容，用于后续判断是否需要重建。
     * 扫描失败不是致命错误，仅记录日志。
     */
    if (!begin_initramfs_scan(op)) {
        ui_log(op, "Failed to initiate initramfs scan");
    }

    /*
     * 第八步：确定并行编译级别。
     * 尽早确定，以便后续内核模块编译等操作可以利用多核并行。
     * 默认自动检测 CPU 数量，上限为 32（可通过 -j 选项覆盖）。
     */

    set_concurrency_level(op);

    /*
     * 第九步：检查是否以 root（EUID=0）身份运行。
     * 安装/卸载/完整性检查都需要 root 权限。
     * 但 --add-this-kernel 操作可以不需要 root（取决于具体实现）。
     */

    if (!op->add_this_kernel && !check_euid(op)) {
        goto done;
    }

    /*
     * 第十步：搜索安装过程中需要的系统工具。
     *
     * find_system_utils()：搜索必需工具（ldconfig、grep、dmesg、tail）和
     *   可选工具（objcopy、chcon、pkg-config、openssl、dkms、systemctl 等）。
     *
     * find_module_utils()：搜索内核模块管理工具（modprobe、rmmod、lsmod、depmod）。
     *
     * 注意（XXX）：实际上并不是所有操作都需要全部工具，理想情况下应该
     * 只搜索当前操作所需的工具，但目前实现为统一搜索。
     */

    if (!find_system_utils(op)) goto done;
    if (!find_module_utils(op)) goto done;

    /* 检测 SELinux 状态，若启用则确定安全上下文设置策略 */
    if (!check_selinux(op)) goto done;

    /* 检测 systemd 是否可用，决定是否安装 systemd 服务单元 */
    if (!check_systemd(op)) goto done;

    /*
     * 查询 X.Org Server 版本。
     * 根据版本号确定安装路径（模块化 Xorg 7.x+ vs 旧版 XFree86 风格），
     * 以及是否支持 OutputClass 配置等特性。
     */

    query_xorg_version(op);

    /*
     * 根据系统配置和已解析的选项，计算所有文件类型的默认安装前缀和路径。
     * 这是安装路径确定的最终步骤，之后所有路径都已确定。
     */

    get_default_prefixes_and_paths(op);

    /*
     * ===== 根据操作模式执行对应操作 =====
     * 以下操作互斥，按优先级执行第一个匹配的操作。
     */

    /* 操作 1：显示已安装驱动的信息（--driver-info / -i） */

    if (op->driver_info) {
        ret = report_driver_information(op);
    }

    /* 操作 2：执行现有安装的完整性检查（--sanity） */

    else if (op->sanity) {
        ret = sanity(op);
    }

    /* 操作 3：卸载已安装的驱动（--uninstall 或程序名为 nvidia-uninstall） */

    else if (op->uninstall) {
        ret = uninstall_existing_driver(op, TRUE /* interactive：允许用户交互确认 */,
                                        op->skip_depmod);
    }

    /* 操作 4：为当前运行的内核添加预编译接口（--add-this-kernel） */

    else if (op->add_this_kernel) {
        ret = add_this_kernel(op);
    }

    /* 操作 5（默认）：从当前工作目录执行完整安装流程 */

    else {
        ret = install_from_cwd(op);
    }

    /*
     * 操作完成后，如果成功，检查是否需要建议用户重启系统。
     * 例如：安装了新的内核模块但无法卸载旧模块时，需要重启才能生效。
     */
    if (ret) {
        suggest_reboot(op);
    }

 done:

    /* 关闭用户界面（释放 ncurses 资源等） */
    ui_close(op);

    /* 释放 Options 结构体内存 */
    nvfree((void*)op);

    /*
     * 返回退出码：
     *   0 = 操作成功（ret 为 TRUE）
     *   1 = 操作失败（ret 为 FALSE）
     */
    return (ret ? 0 : 1);

} /* main() */
