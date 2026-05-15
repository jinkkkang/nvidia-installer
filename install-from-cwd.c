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
 * install_from_cwd.c -
 *
 * 【文件说明】nvidia-installer 项目的核心安装逻辑实现文件。
 *
 * 本文件实现了 NVIDIA 驱动程序从当前工作目录（即解压后的 .run 包目录）执行
 * 完整安装流程的所有核心功能。安装器的 .run 自解压包在解压后，会将所有文件
 * 放置在当前目录中，并在此目录下执行安装。
 *
 * 主要功能函数：
 *   - install_from_cwd()：主安装入口函数，编排完整的安装流程，包括：
 *     1. 解析 .manifest 文件，构建 Package 结构
 *     2. 检查 GPU 兼容性和 X Server 运行状态
 *     3. 运行发行版钩子脚本（pre-unload / pre-install）
 *     4. 卸载旧驱动并检查 Nouveau 驱动冲突
 *     5. 编译或链接内核模块
 *     6. 设置安装目标路径，构建命令列表
 *     7. 执行安装命令，加载内核模块
 *     8. 运行 post-install 钩子和 nvidia-xconfig
 *
 *   - parse_manifest()：解析 .manifest 文件，读取包描述、版本号、内核模块列表、
 *     以及所有待安装文件条目，构建完整的 Package 数据结构。
 *
 *   - install_kernel_modules()：内核模块安装流程，优先查找预编译接口（避免
 *     用户系统上需要完整的内核头文件和编译工具链），若无预编译接口则从源码编译。
 *
 *   - assisted_module_signing()：在启用 UEFI Secure Boot 或内核强制模块签名
 *     的系统上，引导用户完成内核模块的数字签名流程。
 *
 *   - add_package_entry()：向 Package 结构中添加单个文件条目。
 *   - free_package()：释放 Package 结构及其所有动态分配的内存。
 *   - add_this_kernel()：为当前运行的内核构建预编译接口，并重新打包 .run 文件。
 *
 * 辅助函数：
 *   - has_separate_interface_file()：判断指定模块是否有独立的内核接口文件。
 *   - populate_optional_module_info()：填充可选内核模块的元信息。
 *   - nvidia_to_nv()：将 "nvidia" 前缀替换为 "nv" 并拼接后缀。
 *   - parse_kernel_modules_list()：解析 .manifest 中的内核模块列表。
 */


/* ===== 标准 C 库头文件 ===== */
#include <stdio.h>      /* 标准输入输出（printf, fprintf 等） */
#include <stdlib.h>      /* 标准库（malloc, free, atoi, exit 等） */

#include <limits.h>      /* 整数类型极值（INT_MAX, PATH_MAX 等） */
#include <stdlib.h>      /* 注意：重复包含，不影响编译（头文件保护宏） */
#include <string.h>      /* 字符串操作（strcmp, strchr, memset, strncmp 等） */
#include <unistd.h>      /* POSIX 接口（close, stat 等） */
#include <ctype.h>       /* 字符分类（isspace, isdigit 等） */
#include <errno.h>       /* 错误码（errno, strerror） */
#include <fcntl.h>       /* 文件控制（open, O_RDONLY 等） */
#include <unistd.h>      /* 注意：重复包含 */
#include <stddef.h>      /* offsetof 宏，用于获取结构体成员偏移量 */

/* ===== 系统调用相关头文件 ===== */
#include <sys/mman.h>    /* 内存映射（mmap, munmap），用于高效读取 .manifest 文件 */
#include <sys/types.h>   /* 系统数据类型（mode_t, ino_t, dev_t 等） */
#include <sys/stat.h>    /* 文件状态（stat, fstat 结构体和宏） */

/* ===== 项目内部头文件 ===== */
#include "nvidia-installer.h"  /* 核心数据结构定义（Options, Package, PackageEntry 等） */
#include "user-interface.h"    /* 用户界面接口（ui_message, ui_error, ui_warn 等） */
#include "kernel.h"            /* 内核模块管理（编译、签名、加载、预编译接口等） */
#include "command-list.h"      /* 命令列表构建和执行（build_command_list, do_install 等） */
#include "backup.h"            /* 备份与卸载（init_backup, run_existing_uninstaller 等） */
#include "files.h"             /* 文件操作工具（set_destinations, process_dot_desktop_files 等） */
#include "misc.h"              /* 杂项工具（check_for_running_x, run_distro_hook 等） */
#include "sanity.h"            /* 安装完整性检查（check_installed_files_from_package 等） */
#include "manifest.h"          /* .manifest 文件类型解析（parse_manifest_file_type 等） */

/* ===== 本文件内部静态函数的前向声明 ===== */

/* 解析当前目录下的 .manifest 文件，构建并返回 Package 结构体；失败返回 NULL */
static Package *parse_manifest(Options *op);
/* 编译/链接内核模块并添加到安装包中；成功返回 TRUE，失败返回 FALSE */
static int install_kernel_modules(Options *op,  Package *p);
/* 释放 Package 结构体及其所有动态分配的内存（包括所有文件条目） */
static void free_package(Package *p);
/* 引导用户完成内核模块签名流程（Secure Boot 支持）；成功返回 TRUE，失败返回 FALSE */
static int assisted_module_signing(Options *op, Package *p);

/*
 * 可选内核模块信息表。
 * 定义了安装器支持的可选内核模块及其元信息。这些模块不是驱动正常运行的
 * 必需模块，用户可以通过命令行选项禁用它们的安装。
 *
 * 各字段含义：
 *   module_name              - 模块名称（对应 .ko 文件名，不含扩展名）
 *   optional_module_dependee - 依赖此模块的功能名称（用于提示用户）
 *   disable_option           - 禁用此模块安装的命令行选项名称
 *   option_offset            - Options 结构体中对应布尔开关字段的偏移量
 */
static const KernelModuleInfo optional_modules[] = {
    {
         .module_name = "nvidia-uvm",
         .optional_module_dependee = "CUDA",
         .disable_option = "no-unified-memory",
         .option_offset = offsetof(Options, install_uvm),
    },                           /* nvidia-uvm：统一虚拟内存模块，CUDA 应用所需 */
    {
         .module_name = "nvidia-drm",
         .optional_module_dependee = "DRM-KMS",
         .disable_option = "no-drm",
         .option_offset = offsetof(Options, install_drm),
    },                           /* nvidia-drm：DRM/KMS 模块，用于内核模式设置（Wayland 等需要） */
    {
        .module_name = "nvidia-peermem",
        .optional_module_dependee = "GPUDirect RDMA p2p memory sharing",
        .disable_option = "no-peermem",
        .option_offset = offsetof(Options, install_peermem),
    },                           /* nvidia-peermem：GPUDirect RDMA 点对点内存共享模块 */
};

/*
 * install_from_cwd() - 从当前工作目录执行 NVIDIA 驱动安装。
 *
 * 这是整个安装器的主入口函数，编排完整的安装流程：
 *
 * 【前置检查阶段】
 *   1. 确认当前目录存在 .manifest 文件，并验证其中列出的所有文件
 *      的校验和（用于检测包损坏，而非安全用途）
 *   2. 确认用户接受许可协议
 *   3. 可选：覆盖 OpenGL 和 XFree86 的安装前缀路径
 *   4. 检测当前已安装的 NVIDIA 驱动版本（如有）
 *
 * 【安装流程】
 *   5. 检查 GPU 兼容性，检查 X Server 运行状态
 *   6. 运行发行版钩子脚本（pre-unload / pre-install）
 *   7. 卸载旧内核模块，检查 Nouveau 驱动冲突
 *   8. 编译/链接内核模块（优先使用预编译接口）
 *   9. 确定安装路径，构建文件安装命令列表
 *   10. 执行安装，加载 nvidia-drm 等内核模块
 *   11. 运行 post-install 钩子，可选运行 nvidia-xconfig
 *
 * 参数：
 *   op - 全局选项结构体指针，包含所有命令行参数和运行时配置
 *
 * 返回值：
 *   TRUE  - 安装成功
 *   FALSE - 安装失败或用户主动取消
 */

int install_from_cwd(Options *op)
{
    Package *p;           /* 安装包结构体，由 parse_manifest() 构建 */
    CommandList *c;       /* 安装命令列表，由 build_command_list() 构建 */
    int ran_pre_install_hook = FALSE;  /* 标记是否已运行 pre-install 钩子（用于失败时回调） */
    HookScriptStatus res; /* 钩子脚本执行结果 */

    /*
     * 以下三个字符串用于拼接安装完成后的提示消息。
     * 根据安装模式（仅内核模块 / 完整安装）和 xconfig 执行结果，
     * 部分字符串可能被置为空字符串以调整最终消息内容。
     */
    static const char* module_only_text =
        "kernel module for the ";              /* 仅安装内核模块时显示的前缀文本 */
    static const char* xconfig_success_text =
        "Your X configuration file has been successfully updated.  ";  /* xconfig 成功时的提示 */
    static const char* edit_your_xorgconf_text =
        "  Please update your xorg.conf file as "
        "appropriate; see the file /usr/share/doc/"
        "NVIDIA_GLX-1.0/README.txt for details.";  /* xconfig 未运行时提示用户手动编辑 */

    /*
     * 【第 1 步】验证并解析当前目录下的 .manifest 文件，构建 Package 结构。
     * 解析失败（文件不存在、格式错误、校验和不匹配等）则跳转到 failed 标签。
     */

    if ((p = parse_manifest(op)) == NULL) goto failed;

    /* 如果包中不包含 X 相关文件，则不提示用户编辑 xorg.conf */
    if (!op->x_files_packaged) {
        edit_your_xorgconf_text = "";
    }

    /* 设置用户界面标题栏，显示包描述和版本号 */
    ui_set_title(op, "%s (%s)", p->description, p->version);

    /*
     * 【第 2 步】检查 GPU 兼容性。
     * 如果系统中安装了仅被旧版驱动支持的"legacy" GPU，向用户发出警告。
     * 如果完全没有找到受支持的 NVIDIA GPU，也会发出警告。
     */

    check_for_nvidia_graphics_devices(op, p);

    /* 【第 3 步】检查是否有 X Server 正在运行（安装过程中不能有 X 运行） */

    if (!check_for_running_x(op)) goto failed;

    /*
     * 运行发行版提供的 pre-unload 钩子脚本。
     * 这个钩子在卸载旧内核模块之前执行，允许发行版执行特定的清理操作。
     */
    res = run_distro_hook(op, "pre-unload");
    if (res == HOOK_SCRIPT_FAIL) {
        ui_error(op, "Pre-unload hook script failed");
        goto failed;
    }

    /* 确保旧的 NVIDIA 内核模块已被卸载（rmmod） */

    if (!check_for_unloaded_kernel_module(op)) goto failed;

    /* 记录安装日志：开始安装指定版本的驱动 */
    ui_log(op, "Installing NVIDIA driver version %s.", p->version);

    /*
     * 【第 4 步】检测当前已安装的 NVIDIA 驱动版本（如有），
     * 询问用户是否确认覆盖已有的安装。
     * 用户拒绝则跳转到 exit_install（正常退出，非错误）。
     */

    if (!check_for_existing_driver(op, p)) goto exit_install;

    /*
     * 检查是否存在其他安装方式（如通过包管理器安装的驱动）已安装或可用。
     * 如果检测到替代安装方式，询问用户是否仍要继续安装。
     */

    if (!check_for_alternate_install(op)) goto exit_install;

    /*
     * 运行发行版提供的 pre-install 钩子脚本。
     * 典型用途：禁用 Nouveau 驱动、加入黑名单等。
     *
     * 钩子执行结果的三种处理：
     *   HOOK_SCRIPT_FAIL    - 脚本失败，询问用户是否继续
     *   HOOK_SCRIPT_SUCCESS - 脚本成功（可能已禁用 Nouveau），提示可能需要重启
     *   其他                - 脚本不存在或无需处理，直接继续
     */

    res = run_distro_hook(op, "pre-install");
    if (res == HOOK_SCRIPT_FAIL) {
        /* pre-install 钩子失败，询问用户是否仍要继续安装 */
        if (ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                               NUM_CONTINUE_ABORT_CHOICES,
                               CONTINUE_CHOICE, /* 默认选择：继续 */
                               "The distribution-provided pre-install "
                               "script failed!  Are you sure you want "
                               "to continue?") == ABORT_CHOICE) {
            goto failed;
        }
    } else if (res == HOOK_SCRIPT_SUCCESS) {
        /*
         * pre-install 钩子成功执行。如果是首次安装，该脚本可能已帮助
         * 禁用了 Nouveau 驱动，但可能需要重启才能生效。
         * 询问用户是继续安装还是先重启系统。
         */
        if (ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                               NUM_CONTINUE_ABORT_CHOICES,
                               CONTINUE_CHOICE, /* 默认选择：继续 */
                               "The distribution-provided pre-install script "
                               "completed successfully. If this is the first "
                               "time you have run the installer, this script "
                               "may have helped disable Nouveau, but a reboot "
                               "may be required first.  "
                               "Would you like to continue, or would you "
                               "prefer to abort installation to reboot the "
                               "system?") == ABORT_CHOICE) {
            goto exit_install;
        }
        ran_pre_install_hook = TRUE;  /* 标记已运行 pre-install 钩子，失败时需运行 failed-install 钩子 */
    }

    /*
     * 【第 5 步】检查开发工具是否可用。
     * 如果不跳过内核模块安装，则需要检查编译/链接工具：
     *   - 有预编译接口：只需链接工具（check_precompiled_kernel_interface_tools）
     *   - 无预编译接口：需要完整的开发工具链（check_development_tools），
     *     包括 cc、make、ld 等
     */
    if (!op->no_kernel_modules) {
        PrecompiledInfo *info = find_precompiled_kernel_interface(op, p);

        if (info) {
            free_precompiled(info);

            /*
             * 找到了预编译的内核接口，只需确保链接工具可用
             * （不需要完整的编译工具链）
             */
            if (!check_precompiled_kernel_interface_tools(op)) return FALSE;
        } else {
            /*
             * 没有找到预编译的内核接口，需要从源码编译，
             * 确保完整的开发工具链可用（编译器、make 等）
             */
            if (!check_development_tools(op, p)) return FALSE;
        }
    }

    /* 检查 Nouveau 开源驱动是否正在使用中（必须先卸载 Nouveau 才能安装 NVIDIA 驱动） */

    if (!check_for_nouveau(op)) goto failed;

    /*
     * 询问用户是否安装可选的内核模块（nvidia-uvm、nvidia-drm、nvidia-peermem）。
     * 根据用户选择和命令行选项更新 Options 结构体中的对应标志位。
     */

    should_install_optional_modules(op, p, optional_modules,
                                    ARRAY_LEN(optional_modules));

    /*
     * 【第 6 步】为目标内核编译/链接内核模块。
     * install_kernel_modules() 会尝试查找预编译接口，找不到则从源码编译。
     */

    if (!op->no_kernel_modules) {
        if (!install_kernel_modules(op, p)) {
            goto failed;
        }
    } else {
        /* 用户通过 --no-kernel-modules 选项显式跳过了内核模块安装 */
        ui_warn(op, "You specified the '--no-kernel-modules' command line "
                "option, nvidia-installer will not install any kernel "
                "modules as part of this driver installation, and it will "
                "not remove existing NVIDIA kernel modules not part of "
                "an earlier NVIDIA driver installation.  Please ensure "
                "that NVIDIA kernel modules matching this driver version "
                "are installed separately.");
    }
    
    /*
     * 【第 7 步】根据安装模式裁剪安装包内容。
     * 如果仅安装内核模块（--kernel-modules-only），则从包中移除所有非内核模块文件。
     * 否则，处理 OpenGL / XFree86 相关的安装配置。
     */

    if (op->kernel_modules_only) {
        /* 仅安装内核模块模式：移除包中所有非内核模块文件 */
        remove_non_kernel_module_files_from_package(p);
    } else {

        /* 询问用户 XFree86 和 OpenGL 库的安装前缀路径（默认 /usr） */

        if (!get_prefixes(op)) goto failed;

        /*
         * 处理 .desktop 文件：对包中的 .desktop 文件进行路径替换，
         * 使其反映实际的安装路径，然后将处理后的文件添加到安装列表。
         */

        process_dot_desktop_files(op, p);

#if defined(NV_X86_64)
        /*
         * 在 x86_64 系统上，询问用户是否安装 32 位兼容库文件。
         * 这些库文件用于在 64 位系统上运行 32 位的 OpenGL 应用程序。
         */

        should_install_compat32_files(op, p);
#endif /* NV_X86_64 */
    }

    /* 如果用户指定了 --no-opengl-files，则从包中移除所有 OpenGL 相关文件 */
    if (op->no_opengl_files) {
        remove_opengl_files_from_package(p);
    } else {
        /* 检查系统中是否存在 Vulkan 加载器（libvulkan.so） */
        check_for_vulkan_loader(op);
    }

    /* 如果用户指定了 --no-wine-files，则从包中移除所有 Wine 兼容层文件 */
    if (op->no_wine_files) {
        remove_wine_files_from_package(p);
    }

    /*
     * 根据系统是否使用 systemd 决定是否安装 systemd 相关文件
     * （服务单元文件、睡眠/唤醒钩子脚本等）
     */
    if (op->use_systemd != NV_OPTIONAL_BOOL_TRUE) {
        remove_systemd_files_from_package(p);
    }

    /*
     * 移除不会被安装的内核模块源文件。
     * 被用户禁用或不适用于当前配置的内核模块，其源文件无需安装。
     */
    remove_non_installed_kernel_module_source_files_from_package(p);

    /*
     * 【第 8 步】为包中的每个文件设置安装目标路径。
     * 根据安装前缀和文件类型，计算每个文件的最终安装位置
     * （如 /usr/lib/x86_64-linux-gnu/libcuda.so.1）。
     */

    if (!set_destinations(op, p)) goto failed;

    /*
     * 如果安装 OpenGL 库，确保在 /usr/lib/libGL.so.1 创建符号链接
     * （OpenGL ABI 兼容性要求）。
     * 注意：add_libgl_abi_symlink() 会自行设置目标路径，
     * 因此必须在 set_destinations() 之后调用。
     */
    if (!op->kernel_modules_only && !op->no_opengl_files) {
        add_libgl_abi_symlink(op, p);
    }

    if (!op->kernel_modules_only) {
        /*
         * 【第 9 步】卸载已存在的 NVIDIA 驱动。
         * 必须在构建命令列表之前完成，否则会有文件冲突。
         *
         * XXX 已知问题：如果卸载完成后用户拒绝执行安装命令列表，
         * 系统将处于没有任何 NVIDIA 驱动的状态。
         */
        if (!run_existing_uninstaller(op)) goto failed;

        /* 初始化备份日志，记录将被覆盖的文件以便后续卸载时恢复 */
        if (!init_backup(op, p)) goto failed;
    }

    if (!op->no_kernel_modules) {
        /* 如果用户启用了 DKMS，将内核模块注册到 DKMS 中管理 */
        dkms_register_module(op, p, get_kernel_name(op));
    }

    /* 检查 libglvnd 文件的兼容性和依赖关系 */
    if (!check_libglvnd_files(op, p)) {
        goto failed;
    }

    /*
     * 【第 10 步】构建安装命令列表。
     * 根据 Package 中的文件条目，生成一系列文件复制、符号链接创建、
     * 权限设置等操作的命令列表。
     */

    if ((c = build_command_list(op, p)) == NULL) goto failed;

    /*
     * 向用户展示即将执行的命令列表，请求确认。
     * 用户拒绝则跳转到 exit_install（正常退出）。
     */

    if (!ui_approve_command_list(op, c, "%s", p->description)) {
        goto exit_install;
    }

    /* 【第 11 步】执行安装命令列表（实际的文件复制和配置操作） */

    if (!do_install(op, p, c)) goto failed;

    /*
     * 【第 12 步】加载必要的内核模块。
     * 保持 nvidia-drm 模块处于加载状态，以支持使用 OutputClass 配置
     * 进行驱动匹配的 X Server（如 Xorg 1.16+）。
     */

    if (!op->no_kernel_modules) {
        /* 如果包中包含 nvidia-drm 模块，立即加载它 */
        if (package_includes_kernel_module(p, "nvidia-drm")) {
            if (!load_kernel_module(op, "nvidia-drm")) {
                goto failed;
            }
        }

        /* 如果包中包含 nvidia-vgpu-vfio 模块（vGPU 虚拟化支持），也立即加载 */
        if (package_includes_kernel_module(p, "nvidia-vgpu-vfio")) {
            if (!load_kernel_module(op, "nvidia-vgpu-vfio")) {
                goto failed;
            }
        }
    }

    /* 运行发行版提供的 post-install 钩子脚本 */

    run_distro_hook(op, "post-install");

    /*
     * 【第 13 步】安装后完整性检查。
     * 验证所有文件是否已正确安装到目标位置。
     */

    check_installed_files_from_package(op, p);

    /* ===== 安装完成，构造并显示完成消息 ===== */

    /* 如果不是仅安装内核模块，则不需要 "kernel module for the" 前缀 */
    if (!op->kernel_modules_only) {
        module_only_text = "";
    }

    if (op->kernel_modules_only || op->no_nvidia_xconfig_question) {
        /* 仅安装内核模块或不询问 xconfig，则不显示 xconfig 相关提示 */
        xconfig_success_text = "";
        edit_your_xorgconf_text = "";
    } else {
        int ret;

        /*
         * 询问用户是否运行 nvidia-xconfig 工具来自动更新 X 配置文件，
         * 使 NVIDIA X 驱动在下次启动 X 时被使用。
         * 已有的 X 配置文件会被备份。
         */

        const char *msg = "Would you like to run the nvidia-xconfig utility "
                          "to automatically update your X configuration file "
                          "so that the NVIDIA X driver will be used when you "
                          "restart X?  Any pre-existing X configuration "
                          "file will be backed up.";

        ret = run_nvidia_xconfig(op, FALSE, msg, op->run_nvidia_xconfig);

        if (ret) {
            /* nvidia-xconfig 成功运行，不需要提示用户手动编辑 */
            edit_your_xorgconf_text = "";
        } else {
            /* nvidia-xconfig 未运行或失败，不显示成功提示，但提示手动编辑 */
            xconfig_success_text = "";
        }
    }

    /* 显示安装完成消息 */
    ui_message(op,
               "%sInstallation of the %s%s (version: %s) is now complete.%s",
               xconfig_success_text, module_only_text,
               p->description, p->version,
               edit_your_xorgconf_text);

    /* 释放包结构体内存 */
    free_package(p);

    return TRUE;  /* 安装成功 */
    
 failed:

    /*
     * 安装失败标签：安装过程中发生错误时跳转到此处。
     * 打印错误信息并引导用户查看日志文件或在线文档。
     */

    if (op->logging) {
        /* 如果启用了日志记录，引导用户查看日志文件 */
        ui_error(op, "Installation has failed.  Please see the file '%s' "
                 "for details.  You may find suggestions on fixing "
                 "installation problems in the README available on the "
                 "Linux driver download page at www.nvidia.com.",
                 op->log_file_name);
    } else {
        ui_error(op, "Installation has failed.  You may find suggestions "
                 "on fixing installation problems in the README available "
                 "on the Linux driver download page at www.nvidia.com.");
    }

    /* 如果之前已运行 pre-install 钩子，则运行 failed-install 钩子进行清理 */
    if (ran_pre_install_hook)
        run_distro_hook(op, "failed-install");

    /* 直接落入 exit_install 标签... */

 exit_install:

    /*
     * 正常退出标签：当安装因非错误原因退出时到达此处。
     * 例如用户拒绝了许可协议、拒绝覆盖已有驱动、
     * 或在确认安装命令列表时选择了取消。
     */

    free_package(p);

    return FALSE;  /* 安装未完成 */

} /* install_from_cwd() */



/*
 * install_kernel_modules() - 编译/链接并安装内核模块。
 *
 * 此函数实现内核模块的完整构建和安装流程：
 *
 * 1. 处理 DKMS 配置文件
 * 2. 确定内核模块的安装路径（如 /lib/modules/<版本>/kernel/drivers/video）
 * 3. 尝试查找预编译的内核接口：
 *    - 如果找到：解压预编译的内核接口文件，与二进制核心模块链接
 *    - 如果未找到：确定内核源码路径，从源码编译内核模块
 * 4. 可选：对内核模块进行数字签名（Secure Boot 支持）
 * 5. 测试编译好的内核模块（尝试 insmod 加载）
 * 6. 将编译好的内核模块添加到安装文件列表中
 *
 * 参数：
 *   op - 全局选项结构体指针
 *   p  - 安装包结构体指针，包含内核模块源文件信息
 *
 * 返回值：
 *   TRUE  - 内核模块构建和测试成功
 *   FALSE - 构建或测试失败
 */

static int install_kernel_modules(Options *op,  Package *p)
{
    PrecompiledInfo *precompiled_info;  /* 预编译内核接口信息 */

    /* 处理包中的 dkms.conf 配置文件（如果存在） */
    process_dkms_conf(op,p);

    /* 确定内核模块的安装目标路径 */

    if (!determine_kernel_module_installation_path(op)) return FALSE;

    /* 检查 /proc/sys/kernel/modprobe 路径是否有效 */

    if (!check_proc_modprobe_path(op)) return FALSE;

    /*
     * 使用类似 nvchooser 的逻辑判断是否有适用于当前内核的预编译接口。
     *
     * XXX 设计讨论：有人可能会认为不应该在此时执行编译/链接，
     * 而应该将其添加到命令列表中，在执行阶段再处理。但我认为
     * 尽早确认内核模块可用更好 —— 用户常见的问题是：既没有
     * 预编译的内核接口，又没有安装内核头文件，所以越早发现越好。
     */

    if ((precompiled_info = find_precompiled_kernel_interface(op, p))) {

        int i, precompiled_success = TRUE;

        /*
         * 找到了预编译的内核接口包，现在将接口文件与二进制核心模块
         * 链接，生成最终的内核模块（.ko 文件）。
         *
         * XXX 如果链接失败，是否应该回退到从源码编译？
         * 不，如果链接都失败了，说明有严重问题，应该直接报错退出。
         */

        /* 遍历预编译包中的所有文件，逐个解压和链接 */
        for (i = 0; i < precompiled_info->num_files; i++) {
            if (!unpack_kernel_modules(op, p, p->kernel_module_build_directory,
                                       &(precompiled_info->files[i]))) {
                precompiled_success = FALSE;
                break;
            }
        }

        /* 释放预编译信息结构体 */
        free_precompiled(precompiled_info);
        if (!precompiled_success) {
            return FALSE;
        }
    } else {
        /*
         * 没有找到预编译的内核接口，需要从源码编译。
         * 首先确定内核源码/头文件的路径。
         */

        if (!determine_kernel_source_path(op, p)) return FALSE;

        /* 从源码编译内核模块 */

        if (!build_kernel_modules(op, p)) return FALSE;
    }

    /* 可选：对内核模块进行数字签名（Secure Boot / CONFIG_MODULE_SIG_FORCE 支持） */
    if (!assisted_module_signing(op, p)) return FALSE;

    /*
     * 到此为止已有完整的内核模块，进行加载测试确保其正常工作。
     * test_kernel_modules() 会尝试 insmod 加载模块并检查 dmesg 输出。
     */

    if (!test_kernel_modules(op, p)) return FALSE;

    /* 将编译好的内核模块文件添加到安装包的文件列表中 */

    add_kernel_modules_to_package(op, p);

    return TRUE;
}


/*
 * add_this_kernel() - 为当前运行的内核构建预编译接口，并重新打包 .run 文件。
 *
 * 此函数用于 --add-this-kernel 命令行选项。它不执行安装，而是：
 * 1. 解析 .manifest 文件获取包信息
 * 2. 确认开发工具可用（编译器、make 等）
 * 3. 确定内核源码/头文件路径
 * 4. 为当前内核编译内核接口文件（.o 文件，不是完整的 .ko 模块）
 * 5. 将编译好的接口文件打包，添加到 .run 安装包中
 *
 * 这样做的好处是：其他使用相同内核版本的系统在安装时可以直接使用
 * 预编译接口进行链接，无需再安装内核头文件和编译工具链。
 *
 * 参数：
 *   op - 全局选项结构体指针
 *
 * 返回值：
 *   TRUE  - 预编译接口构建和打包成功
 *   FALSE - 过程中发生错误
 */

int add_this_kernel(Options *op)
{
    Package *p;
    PrecompiledFileInfo *fileInfos;   /* 编译好的内核接口文件信息数组 */

    /* 解析 .manifest 文件，构建 Package 结构 */

    if ((p = parse_manifest(op)) == NULL) goto failed;

    /* 确保系统上有完整的开发工具链（cc、make、ld 等） */

    if (!check_development_tools(op, p)) goto failed;

    /* 确定内核头文件/源码的路径 */

    if (!determine_kernel_source_path(op, p)) goto failed;

    /*
     * 编译所有内核模块的接口文件。
     * build_kernel_interfaces() 返回成功编译的接口数量，
     * 必须等于包中的内核模块数量才算成功。
     */

    if (p->num_kernel_modules != build_kernel_interfaces(op, p, &fileInfos))
        goto failed;

    /* 将编译好的预编译接口文件打包（添加到包的预编译目录中） */

    if (!pack_precompiled_files(op, p, p->num_kernel_modules, fileInfos))
        goto failed;

    free_package(p);

    return TRUE;

 failed:

    ui_error(op, "Unable to add a precompiled kernel interface for the "
             "running kernel.");

    free_package(p);

    return FALSE;

} /* add_this_kernel() */



/*
 * has_separate_interface_file() - 判断指定的内核模块是否有独立的接口文件。
 *
 * NVIDIA 驱动的主模块（nvidia.ko）由两部分组成：
 *   - 内核接口层（nv-linux.o）：与特定内核版本相关的部分
 *   - 二进制核心（nv-kernel.o_binary）：与内核版本无关的预编译二进制
 * 这种分离设计使得预编译接口成为可能。
 *
 * 但并非所有模块都有这种分离结构，以下模块是单体式的，
 * 它们的所有代码都在一个源文件中编译：
 *   - nvidia-vgpu-vfio、nvidia-uvm、nvidia-drm、nvidia-peermem
 *
 * 参数：
 *   name - 内核模块名称（如 "nvidia"、"nvidia-uvm"）
 *
 * 返回值：
 *   TRUE  - 该模块有独立的接口文件（如主模块 "nvidia"）
 *   FALSE - 该模块没有独立的接口文件（单体式模块）
 */
static int has_separate_interface_file(char *name) {
    int i;

    /* 不具有独立接口文件的模块列表 */
    static const char* no_interface_modules[] = {
        "nvidia-vgpu-vfio",
        "nvidia-uvm",
        "nvidia-drm",
        "nvidia-peermem",
    };

    /* 遍历列表，如果模块名匹配则返回 FALSE */
    for (i = 0; i < ARRAY_LEN(no_interface_modules); i++) {
        if (strcmp(no_interface_modules[i],name) == 0) {
            return FALSE;
        }
    }

    /* 不在排除列表中的模块都有独立的接口文件 */
    return TRUE;
};

/*
 * populate_optional_module_info() - 填充可选模块的元信息。
 *
 * 在 optional_modules 全局表中查找与给定模块名匹配的条目，
 * 如果找到，则将该模块标记为可选模块，并复制相关的元信息
 * （依赖者名称、禁用选项名称、选项偏移量）到模块信息记录中。
 * 这些元信息后续用于生成用户提示和错误消息。
 *
 * 如果模块不在 optional_modules 表中，则不做任何操作
 * （该模块被视为必需模块）。
 *
 * 参数：
 *   module - 指向待填充的 KernelModuleInfo 结构体的指针
 */
static void populate_optional_module_info(KernelModuleInfo *module)
{
    int i;

    /* 遍历可选模块表，查找匹配的模块名 */
    for (i = 0; i < ARRAY_LEN(optional_modules); i++) {
        if (strcmp(optional_modules[i].module_name, module->module_name) == 0) {
            module->is_optional = TRUE;
            module->optional_module_dependee =
                optional_modules[i].optional_module_dependee;
            module->disable_option = optional_modules[i].disable_option;
            module->option_offset = optional_modules[i].option_offset;
            return;  /* 找到匹配项后立即返回 */
        }
    }
    /* 未找到匹配项，模块保持默认的非可选状态 */
}


/*
 * nvidia_to_nv() - 将 "nvidia" 前缀替换为 "nv"，并拼接后缀。
 *
 * 用于生成内核模块的接口文件名和核心二进制文件名。
 * 例如：
 *   nvidia_to_nv("nvidia", "-linux.o")       => "nv-linux.o"
 *   nvidia_to_nv("nvidia", "-kernel.o_binary") => "nv-kernel.o_binary"
 *   nvidia_to_nv("nvidia-modeset", "-linux.o") => "nv-modeset-linux.o"
 *
 * 参数：
 *   name   - 原始模块名称（必须以 "nvidia" 开头）
 *   suffix - 要拼接的后缀字符串
 *
 * 返回值：
 *   成功 - 新分配的字符串（调用者负责释放）
 *   失败 - NULL（name 不以 "nvidia" 开头时）
 */
static char *nvidia_to_nv(const char *name, const char *suffix) {
    /* 确认 name 以 "nvidia" 开头，否则无法进行替换 */
    if (strncmp("nvidia", name, strlen("nvidia")) != 0) {
        return NULL;
    }

    /* 拼接 "nv" + name 中 "nvidia" 之后的部分 + suffix */
    return nvstrcat("nv", name + strlen("nvidia"), suffix, NULL);
}


/*
 * parse_kernel_modules_list() - 解析 .manifest 文件中的内核模块列表。
 *
 * .manifest 文件的第四行是以空格分隔的内核模块名列表
 * （如 "nvidia nvidia-modeset nvidia-uvm nvidia-drm nvidia-peermem"）。
 * 此函数遍历列表中的每个模块名，为其生成 KernelModuleInfo 记录，
 * 并存储到 Package 结构体中。
 *
 * 对于每个模块，生成的信息包括：
 *   - module_name：模块名称（如 "nvidia"）
 *   - module_filename：模块文件名（如 "nvidia.ko"）
 *   - has_separate_interface_file：是否有独立的内核接口文件
 *   - interface_filename：接口文件名（如 "nv-linux.o"），仅限有独立接口的模块
 *   - core_object_name：核心二进制路径（如 "nvidia/nv-kernel.o_binary"），仅限有独立接口的模块
 *   - 可选模块的元信息（is_optional、disable_option 等）
 *
 * 参数：
 *   p    - 安装包结构体指针
 *   list - 以空格分隔的内核模块名字符串（注意：strtok 会修改此字符串）
 *
 * 返回值：
 *   解析到的内核模块数量
 */
static int parse_kernel_modules_list(Package *p, char *list) {
    char *name;

    p->num_kernel_modules = 0; /* 重置计数（以防此函数被多次调用） */

    /* 使用 strtok 按空格分割模块名列表，逐个处理 */
    for (name = strtok(list, " "); name; name = strtok(NULL, " ")) {
        KernelModuleInfo *module;

        /* 扩展 kernel_modules 数组以容纳新条目 */
        p->kernel_modules = nvrealloc(p->kernel_modules,
                                      (p->num_kernel_modules + 1) *
                                      sizeof(p->kernel_modules[0]));
        module = p->kernel_modules + p->num_kernel_modules;
        memset(module, 0, sizeof(*module));  /* 清零新条目 */

        /* 设置模块基本信息 */
        module->module_name = nvstrdup(name);                  /* 复制模块名 */
        module->module_filename = nvstrcat(name, ".ko", NULL); /* 模块文件名 = 模块名 + ".ko" */
        module->has_separate_interface_file = has_separate_interface_file(name);

        /* 如果模块有独立的接口文件，生成接口文件名和核心二进制路径 */
        if (module->has_separate_interface_file) {
            /* 例如 "nvidia" => 核心二进制 "nv-kernel.o_binary" */
            char *core_binary = nvidia_to_nv(name, "-kernel.o_binary");
            /* 例如 "nvidia" => 接口文件名 "nv-linux.o" */
            module->interface_filename = nvidia_to_nv(name, "-linux.o");
            /* 核心二进制的完整路径，如 "nvidia/nv-kernel.o_binary" */
            module->core_object_name = nvstrcat(name, "/", core_binary, NULL);
            nvfree(core_binary);
        }

        /* 填充可选模块的元信息（如果是可选模块的话） */
        populate_optional_module_info(module);

        p->num_kernel_modules++;
    }

    return p->num_kernel_modules;
}


/*
 * parse_manifest() - 打开并解析当前目录下的 .manifest 文件。
 *
 * .manifest 文件是 NVIDIA 安装包的核心描述文件，采用纯文本格式。
 * 该文件使用 mmap 映射到内存中进行解析，以提高大文件的读取效率。
 *
 * 文件格式说明：
 *
 * 【头部信息（前 9 行，部分行已被忽略/合并）】
 *   第 1 行 - 包描述字符串（如 "NVIDIA Accelerated Graphics Driver for Linux-x86_64"）
 *   第 2 行 - 版本号字符串（如 "535.129.03"）
 *   第 3 行 - [已忽略] 原为内核模块文件名
 *   第 4 行 - 以空格分隔的内核模块名列表（如 "nvidia nvidia-modeset nvidia-uvm nvidia-drm"）
 *   第 5~8 行 - [已忽略] 原为模块卸载列表、构建目录、预编译目录等
 *
 * 【文件条目（第 9 行之后的每一行）】
 *   每行是一个以空格分隔的文件条目，包含以下字段：
 *   - filename：文件名（相对于当前目录的路径）
 *   - permissions：八进制权限值（如 "0755"）
 *   - type_flag：文件类型标志（如 "OPENGL_LIB"、"KERNEL_MODULE_SRC" 等）
 *   - [可选] architecture：架构标志（"NATIVE" 或 "COMPAT32"），仅部分类型有
 *   - [可选] path：安装子路径，仅部分类型有
 *   - [可选] INHERIT_PATH_DEPTH:N：路径继承深度，用于从文件名推导安装路径
 *   - [可选] target：符号链接目标，仅符号链接类型有
 *
 * 参数：
 *   op - 全局选项结构体指针
 *
 * 返回值：
 *   成功 - 新分配并填充的 Package 结构体指针
 *   失败 - NULL（.manifest 不存在、格式无效等）
 */

static Package *parse_manifest (Options *op)
{
    char *buf, *c, *tmpstr;
    int line;                       /* 当前解析的行号（用于错误报告） */
    int fd, ret, len = 0;          /* 文件描述符、函数返回值、文件长度 */
    struct stat stat_buf;           /* 文件状态信息 */
    Package *p;                     /* 构建中的安装包结构体 */
    char *manifest = MAP_FAILED, *ptr;  /* mmap 映射地址和当前读取位置 */
    int opengl_files_packaged = FALSE;  /* 标记包中是否包含 OpenGL 相关文件 */

    /* 分配并清零 Package 结构体 */
    p = (Package *) nvalloc(sizeof (Package));

    /* 打开当前目录下的 .manifest 文件 */

    if ((fd = open(".manifest", O_RDONLY)) == -1) {
        ui_error(op, "No package found for installation.  Please run "
                 "this utility with the '--help' option for usage "
                 "information.");
        goto fail;
    }

    /* 获取文件大小 */
    if (fstat(fd, &stat_buf) == -1) goto cannot_open;

    len = stat_buf.st_size;

    /* 将 .manifest 文件映射到内存，以只读共享方式访问 */
    manifest = mmap(0, len, PROT_READ, MAP_FILE|MAP_SHARED, fd, 0);
    if (manifest == MAP_FAILED) goto cannot_open;

    /* ===== 解析头部信息 ===== */

    /* 第 1 行：包描述字符串 */

    line = 1;
    p->description = get_next_line(manifest, &ptr, manifest, len);
    if (!p->description) goto invalid_manifest_file;

    /* 第 2 行：版本号字符串 */

    line++;
    p->version = get_next_line(ptr, &ptr, manifest, len);
    if (!p->version) goto invalid_manifest_file;

    /* 第 3 行：忽略（历史遗留字段，原为单个内核模块文件名） */

    line++;
    nvfree(get_next_line(ptr, &ptr, manifest, len));

    /* 第 4 行：以空格分隔的内核模块名列表，解析为 KernelModuleInfo 数组 */

    line++;
    tmpstr = get_next_line(ptr, &ptr, manifest, len);
    if (parse_kernel_modules_list(p, tmpstr) == 0) {
        goto invalid_manifest_file;
    }
    nvfree(tmpstr);

    /*
     * 将 excluded_kernel_modules 初始化为堆分配的空字符串。
     * 这样做的原因：
     * 1. 可以安全地对其调用 nvfree()
     * 2. 在 nvstrcat() 拼接时不会因为 NULL 而提前终止字符串
     */

    p->excluded_kernel_modules = nvstrdup("");

    /*
     * 第 5~8 行：忽略（历史遗留字段，这些信息已通过其他方式获取）。
     * 注意原注释 "eigth" 应为 "eighth"，但保留原文不修改。
     */

    line++;
    nvfree(get_next_line(ptr, &ptr, manifest, len));
    line++;
    nvfree(get_next_line(ptr, &ptr, manifest, len));
    line++;
    nvfree(get_next_line(ptr, &ptr, manifest, len));
    line++;
    nvfree(get_next_line(ptr, &ptr, manifest, len));

    /*
     * 确定内核模块的构建目录。
     * 构建目录决定了使用哪套内核模块源码（闭源 "kernel" 或开源 "kernel-open"）。
     *
     * 优先级：
     * 1. 命令行指定的 --kernel-module-build-directory 覆盖值
     * 2. 自动检测可用的模块类型，如果有多种则让用户选择
     * 3. 默认使用 "kernel"（闭源模块）
     */

    if (op->kernel_module_build_directory_override) {
        /* 使用命令行指定的构建目录（转移所有权，置空源指针） */
        p->kernel_module_build_directory =
            op->kernel_module_build_directory_override;
        op->kernel_module_build_directory_override = NULL;
    } else {
        struct module_type_info types;
        int num_types;

        /* 检测当前系统支持的内核模块类型（闭源/开源） */
        num_types = valid_kernel_module_types(op, &types, FALSE);

        if (num_types > 0) {
            int selection;

            if (num_types == 1) {
                /* 只有一种可用类型，自动选择 */
                selection = 0;
            } else {
                /* 有多种类型可用，让用户选择（闭源 vs 开源） */
                selection = ui_multiple_choice(op, types.licenses, num_types,
                    types.default_entry,
                    "Multiple kernel module types are available for this "
                    "system. Which would you like to use?"
                );
            }

            p->kernel_module_build_directory = nvstrdup(types.dirs[selection]);
        } else {
            /* 未检测到有效类型，使用默认值 "kernel"（闭源模块目录） */
            p->kernel_module_build_directory = nvstrdup("kernel");
        }
    }

    /* 移除构建目录路径末尾可能存在的斜杠 */
    remove_trailing_slashes(p->kernel_module_build_directory);

    /* ===== 解析文件条目部分（第 9 行之后） ===== */

    line++;

    /* 逐行读取并解析文件条目，直到遇到空行或文件结束 */
    for (; (buf = get_next_line(ptr, &ptr, manifest, len)); line++) {
        char *flag = NULL;           /* 当前读取的类型标志字符串 */
        PackageEntry entry;          /* 当前文件条目（临时变量） */
        int entry_success = FALSE;   /* 当前条目是否解析成功 */

        /* 遇到空行则结束文件条目解析 */
        if (buf[0] == '\0') {
            free(buf);
            break;
        }

        /* 清零条目结构体 */

        memset(&entry, 0, sizeof(PackageEntry));

        /* 读取第一个字段：文件名（相对路径） */

        c = buf;

        entry.file = read_next_word(buf, &c);

        if (!entry.file) goto entry_done;

        /* 读取第二个字段：权限字符串（如 "0755"） */

        tmpstr = read_next_word(c, &c);

        if (!tmpstr) goto entry_done;

        /* 将权限字符串转换为 mode_t 类型的八进制权限值 */

        ret = mode_string_to_mode(op, tmpstr, &entry.mode);

        free(tmpstr);

        if (!ret) goto entry_done;

        /* 读取第三个字段：文件类型标志（每个文件都必须有） */

        entry.type = FILE_TYPE_NONE;

        flag = read_next_word(c, &c);
        if (!flag) goto entry_done;

        /* 将类型标志字符串解析为枚举值，同时获取该类型的能力标志 */
        entry.type = parse_manifest_file_type(flag, &entry.caps);

        if (entry.type == FILE_TYPE_NONE) {
            goto entry_done;  /* 无法识别的文件类型 */
        }

        /*
         * 跟踪特定文件类型是否被包含在安装包中。
         * 这些标志后续用于决定是否执行某些安装步骤。
         */

        switch (entry.type) {
            case FILE_TYPE_XMODULE_SHARED_LIB:
                op->x_files_packaged = TRUE;       /* 包含 X.Org 模块文件 */
                break;
            case FILE_TYPE_VULKAN_ICD_JSON:
                op->vulkan_icd_json_packaged = TRUE;  /* 包含 Vulkan ICD 配置 */
                break;
            case FILE_TYPE_VULKANSC_ICD_JSON:
                op->vulkansc_icd_json_packaged = TRUE; /* 包含 Vulkan SC ICD 配置 */
                break;
            default: break;
        }

        /* 如果此文件属于 OpenGL 类型，标记包中包含 OpenGL 文件 */

        if (entry.caps.is_opengl) {
            opengl_files_packaged = TRUE;
        }

        /*
         * 某些库文件和符号链接有架构字段，
         * 区分 NATIVE（原生 64 位）和 COMPAT32（32 位兼容）
         */

        entry.compat_arch = FILE_COMPAT_ARCH_NONE;

        if (entry.caps.has_arch) {
            /* 该类型需要架构字段，读取下一个字段 */
            nvfree(flag);
            flag = read_next_word(c, &c);
            if (!flag) goto entry_done;

            if (strcmp(flag, "COMPAT32") == 0)
                entry.compat_arch = FILE_COMPAT_ARCH_COMPAT32;  /* 32 位兼容库 */
            else if (strcmp(flag, "NATIVE") == 0)
                entry.compat_arch = FILE_COMPAT_ARCH_NATIVE;    /* 原生架构库 */
            else {
                goto entry_done;  /* 无法识别的架构标志 */
            }
        }

        /* 如果包中含有 32 位兼容文件，设置标志位 */

        if (entry.compat_arch == FILE_COMPAT_ARCH_COMPAT32) {
            op->compat32_files_packaged = TRUE;
        }

        /*
         * 处理安装路径。某些文件类型有显式路径字段（has_path），
         * 某些类型从文件名中继承路径（inherit_path）。
         */

        if (entry.caps.has_path) {
            /* 直接读取显式指定的安装路径 */
            entry.path = read_next_word(c, &c);
            if (!entry.path) goto invalid_manifest_file;
        } else if (entry.caps.inherit_path) {
            /*
             * 从文件名中推导安装路径。
             * 格式为 "INHERIT_PATH_DEPTH:N"，其中 N 表示要去掉的前导目录层数。
             * 例如文件 "a/b/c/file.so"，深度为 2 时，路径为 "c/"。
             */
            int i;
            char *path, *depth, *slash;
            const char * const depth_marker = "INHERIT_PATH_DEPTH:";

            /* 读取深度标记字段 */
            depth = read_next_word(c, &c);
            if (!depth ||
                strncmp(depth, depth_marker, strlen(depth_marker)) != 0) {
                goto invalid_manifest_file;
            }
            entry.inherit_path_depth = atoi(depth + strlen(depth_marker));
            nvfree(depth);

            /* 从文件名中去掉最后的文件名部分，保留目录路径 */
            path = entry.path = nvstrdup(entry.file);
            slash = strrchr(path, '/');
            if (slash == NULL) {
                goto invalid_manifest_file;
            }
            slash[1] = '\0';  /* 在最后一个 '/' 之后截断 */

            /* 去掉指定层数的前导目录 */
            for (i = 0; i < entry.inherit_path_depth; i++) {
                slash = strchr(entry.path, '/');

                if (slash == NULL) {
                    goto invalid_manifest_file;
                }

                entry.path = slash + 1;  /* 跳过当前目录层 */
            }

            /* 复制最终路径并释放原始字符串 */
            entry.path = nvstrdup(entry.path);
            nvfree(path);
        } else {
            entry.path = NULL;  /* 该类型不需要安装路径 */
        }

        /* 符号链接类型需要读取链接目标 */

        if (entry.caps.is_symlink) {
            entry.target = read_next_word(c, &c);
            if (!entry.target) goto invalid_manifest_file;
        } else {
            entry.target = NULL;
        }

        /*
         * 为后续使用方便，将 name 指针设置为 file 中的基本文件名
         * （即去掉所有前导目录后的部分）。
         * 注意：name 只是指向 file 字符串内部的指针，不单独分配内存。
         */

        entry.name = strrchr(entry.file, '/');
        if (entry.name) entry.name++;  /* 跳过 '/' 字符 */

        if (!entry.name) entry.name = entry.file;  /* 没有目录分隔符，整个就是文件名 */

        /* 将解析好的条目添加到 Package 的文件列表中 */
        add_package_entry(p,
                          entry.file,
                          entry.path,
                          entry.name,
                          entry.target,
                          entry.dst,
                          entry.type,
                          entry.compat_arch,
                          entry.mode);

        entry_success = TRUE;

 entry_done:
        /* 清理当前行的临时分配 */

        nvfree(buf);
        nvfree(flag);
        if (!entry_success) {
            goto invalid_manifest_file;  /* 条目解析失败，报告错误 */
        }
    }

    /*
     * 如果包中没有 OpenGL 文件，则自动设置 no_opengl_files 标志。
     * 这确保了当 OpenGL 文件未被打包时，所有与 OpenGL 相关的
     * 安装步骤都会被跳过（与用户通过 --no-opengl-files 显式禁用的效果相同）。
     */

    if (!opengl_files_packaged) {
        op->no_opengl_files = TRUE;
    }

    /* 解除 .manifest 文件的内存映射并关闭文件描述符 */
    munmap(manifest, len);
    if (fd != -1) close(fd);

    return p;  /* 返回构建完成的 Package 结构体 */

 cannot_open:
    /* 无法打开或映射 .manifest 文件 */
    ui_error(op, "Failure opening package's .manifest file (%s).",
             strerror(errno));
    goto fail;

 invalid_manifest_file:
    /* .manifest 文件格式无效，报告出错的行号 */
    ui_error(op, "Invalid .manifest file; error on line %d.", line);
    goto fail;

 fail:
    /* 统一的错误清理路径：释放所有已分配的资源 */
    free_package(p);
    if (manifest != MAP_FAILED) munmap(manifest, len);
    if (fd != -1) close(fd);
    return NULL;

} /* parse_manifest() */



/*
 * add_package_entry() - 向安装包的文件条目数组中添加一个新条目。
 *
 * 此函数在 parse_manifest() 中被调用以添加 .manifest 中列出的文件，
 * 也在 assisted_module_signing() 等其他地方被调用以动态添加文件
 * （如生成的签名密钥文件）。
 *
 * 注意：传入的字符串指针（file、path、name、target、dst）的所有权
 * 被转移给 Package 结构体，调用者不应在调用后释放这些指针。
 * 其中 name 通常指向 file 字符串内部，不需要单独释放。
 *
 * 参数：
 *   p           - 安装包结构体指针
 *   file        - 源文件路径（相对于安装包目录）
 *   path        - 安装子路径（部分文件类型使用，如 "tls/"）
 *   name        - 基本文件名（通常是 file 的 basename 部分的指针）
 *   target      - 符号链接目标（仅符号链接类型使用）
 *   dst         - 安装目标完整路径（由 set_destinations() 后续设置，此处通常为 NULL）
 *   type        - 文件类型枚举值
 *   compat_arch - 架构兼容性（NATIVE / COMPAT32 / NONE）
 *   mode        - 文件权限（八进制，如 0755）
 */

void add_package_entry(Package *p,
                       char *file,
                       char *path,
                       char *name,
                       char *target,
                       char *dst,
                       PackageEntryFileType type,
                       PackageEntryFileCompatArch compat_arch,
                       mode_t mode)
{
    int n;
    struct stat stat_buf;

    n = p->num_entries;  /* 当前条目数量即为新条目的索引 */

    /* 扩展条目数组以容纳新条目（每次增加一个元素） */
    p->entries =
        (PackageEntry *) nvrealloc(p->entries, (n + 1) * sizeof(PackageEntry));

    /* 清零新条目 */
    memset(&p->entries[n], 0, sizeof(PackageEntry));

    /* 设置条目的各个字段 */
    p->entries[n].file        = file;
    p->entries[n].path        = path;
    p->entries[n].name        = name;
    p->entries[n].target      = target;
    p->entries[n].dst         = dst;
    p->entries[n].type        = type;
    p->entries[n].mode        = mode;
    p->entries[n].caps        = get_file_type_capabilities(type);  /* 根据类型获取能力标志 */
    p->entries[n].compat_arch = compat_arch;

    /*
     * 获取源文件的 inode 和 device 信息。
     * 这些信息用于后续检测硬链接（同一 inode + device 的文件是同一个文件）。
     * 如果 stat 失败（文件暂不存在，如尚未编译的内核模块），则设为 0。
     */
    if (stat(p->entries[n].file, &stat_buf) != -1) {
        p->entries[n].inode = stat_buf.st_ino;
        p->entries[n].device = stat_buf.st_dev;
    } else {
        p->entries[n].inode = 0;
        p->entries[n].device = 0;
    }

    p->num_entries++;  /* 更新条目计数 */

} /* add_package_entry() */



/*
 * free_package() - 释放 Package 数据结构及其所有动态分配的内存。
 *
 * 此函数在安装完成（成功或失败）时被调用，负责清理 parse_manifest()
 * 及后续操作过程中分配的所有内存。
 *
 * 释放顺序：
 * 1. 包描述和版本字符串
 * 2. 内核模块构建目录路径
 * 3. 所有内核模块信息记录（KernelModuleInfo）
 * 4. 排除的内核模块列表字符串
 * 5. 内核 make 编译日志
 * 6. 所有文件条目（PackageEntry）及其内部字符串
 * 7. Package 结构体自身
 *
 * 参数：
 *   p - 待释放的 Package 结构体指针（允许为 NULL，此时直接返回）
 */

static void free_package(Package *p)
{
    int i;

    if (!p) return;  /* NULL 安全：避免对空指针操作 */

    /* 释放包的基本信息字符串 */
    nvfree(p->description);
    nvfree(p->version);

    /* 释放内核模块构建目录路径 */
    nvfree(p->kernel_module_build_directory);

    /* 释放每个内核模块信息记录（包括其内部的字符串成员） */
    for (i = 0; i < p->num_kernel_modules; i++) {
        free_kernel_module_info(p->kernel_modules[i]);
    }
    nvfree(p->kernel_modules);  /* 释放模块信息数组本身 */

    nvfree(p->excluded_kernel_modules);  /* 释放排除的模块列表 */

    nvfree(p->kernel_make_logs);  /* 释放内核编译日志 */

    /* 释放所有文件条目 */
    for (i = 0; i < p->num_entries; i++) {
        nvfree(p->entries[i].file);
        nvfree(p->entries[i].path);
        nvfree(p->entries[i].target);
        nvfree(p->entries[i].dst);

        /*
         * 注意：p->entries[i].name 只是指向 p->entries[i].file 内部的指针，
         * 不需要（也不能）单独释放。释放 file 时 name 所指的内存已被释放。
         */
    }

    nvfree((char *) p->entries);  /* 释放文件条目数组本身 */

    nvfree((char *) p);  /* 释放 Package 结构体本身 */

} /* free_package() */



/*
 * assisted_module_signing() - 引导用户完成内核模块签名流程。
 *
 * 在启用 UEFI Secure Boot 或内核配置了 CONFIG_MODULE_SIG / CONFIG_MODULE_SIG_FORCE
 * 的系统上，内核模块可能需要数字签名才能被加载。此函数实现了以下流程：
 *
 * 1. 检测是否需要签名（检查 Secure Boot 状态和内核配置）
 * 2. 如果需要签名，确定签名密钥来源：
 *    a. 用户通过命令行参数提供了现有密钥对
 *    b. 用户选择使用已有的密钥对（交互式输入路径）
 *    c. 用户选择生成新的密钥对（使用 openssl）
 * 3. 使用密钥对所有内核模块进行签名
 * 4. 如果生成了新密钥，处理密钥的安装/删除：
 *    - 公钥证书（.der）总是被添加到安装包中
 *    - 私钥可选择删除（安全擦除）或保留安装
 *
 * 参数：
 *   op - 全局选项结构体指针
 *   p  - 安装包结构体指针（需要签名的内核模块信息在其中）
 *
 * 返回值：
 *   TRUE  - 签名成功，或不需要签名
 *   FALSE - 签名失败（缺少工具、生成密钥失败等）
 */

static int assisted_module_signing(Options *op, Package *p)
{
    int generate_keys = FALSE;   /* 是否需要生成新密钥对 */
    int do_sign = FALSE;         /* 是否需要执行签名 */
    int secureboot;              /* Secure Boot 状态：1=启用，0=未启用，<0=无法确定 */
    int i;

    /* 检测 UEFI Secure Boot 是否启用 */
    secureboot = secure_boot_enabled();

    if (secureboot < 0) {
        /* 无法确定 Secure Boot 状态（可能不是 UEFI 系统） */
        ui_log(op, "Unable to determine if Secure Boot is enabled: %s",
               strerror(-secureboot));
    }

    if (op->kernel_module_signed) {
        /*
         * 内核模块已经被签名了（例如使用预编译接口时，
         * 签名可能已作为分离签名附加到模块中）。
         */
        return TRUE;
    }

    if (test_kernel_config_option(op, p, "CONFIG_DUMMY_OPTION") ==
        KERNEL_CONFIG_OPTION_UNKNOWN) {
        /*
         * 无法检查内核配置选项（可能缺少内核头文件）。
         * 由于可能是在没有头文件的系统上安装，直接跳过签名。
         */
        return TRUE;
    }

    /*
     * 根据以下优先级判断是否需要签名：
     * 1. 用户显式提供了签名密钥 => 强制签名
     * 2. 内核设置了 CONFIG_MODULE_SIG_FORCE => 必须签名
     * 3. 非 Secure Boot 系统且非专家模式 => 跳过签名
     * 4. 内核设置了 CONFIG_MODULE_SIG => 询问用户
     */

    if (op->module_signing_secret_key && op->module_signing_public_key) {
        /* 用户通过命令行提供了签名密钥，无论是否需要都执行签名 */
        do_sign = TRUE;
    } else if (test_kernel_config_option(op, p, "CONFIG_MODULE_SIG_FORCE") ==
               KERNEL_CONFIG_OPTION_DEFINED) {
        /* CONFIG_MODULE_SIG_FORCE 已设置，内核强制要求模块签名 */
        ui_message(op, "The target kernel has CONFIG_MODULE_SIG_FORCE set, "
                   "which means that it requires that kernel modules be "
                   "cryptographically signed by a trusted key.");
        do_sign = TRUE;
    } else if (secureboot != 1 && !op->expert) {
        /*
         * 非 UEFI 系统，或 Secure Boot 未启用/无法确定，
         * 且不在专家模式下 => 跳过签名。
         */
        return TRUE;
    } else if (test_kernel_config_option(op, p, "CONFIG_MODULE_SIG") ==
               KERNEL_CONFIG_OPTION_DEFINED){
        /*
         * 内核支持模块签名（CONFIG_MODULE_SIG），但不强制要求。
         * 询问用户是否要签名模块。
         */

        const char *choices[2] = {
            "Sign the kernel module",
            "Install without signing"
        };

        /* 如果 Secure Boot 启用，附加额外的警告信息 */
        const char* sb_message = (secureboot == 1) ?
                                     "This system also has UEFI Secure Boot "
                                     "enabled; many distributions enforce "
                                     "module signature verification on UEFI "
                                     "systems when Secure Boot is enabled. " :
                                     "";

        do_sign = (ui_multiple_choice(op, choices, 2, 1, "The target kernel "
                                      "has CONFIG_MODULE_SIG set, which means "
                                      "that it supports cryptographic "
                                      "signatures on kernel modules. On some "
                                      "systems, the kernel may refuse to load "
                                      "modules without a valid signature from "
                                      "a trusted key. %sWould you like to sign "
                                      "the NVIDIA kernel module?",
                                      sb_message) == 0);
    }

    if (!do_sign) {
        /*
         * 不需要签名的情况：
         * - 用户明确选择不签名
         * - 内核不支持模块签名
         * - 没有提供签名密钥且不满足签名条件
         */
        return TRUE;
    }

    /*
     * 如果缺少任一签名密钥（私钥或公钥），需要从用户获取。
     * 提供两种方式：使用已有密钥对，或生成新的密钥对。
     */
    if (!op->module_signing_secret_key || !op->module_signing_public_key) {

        const char *choices[2] = {
            "Use an existing key pair",
            "Generate a new key pair"
        };

        /* 询问用户是使用已有密钥还是生成新密钥 */
        generate_keys = (ui_multiple_choice(op, choices, 2, 1, "Would you like "
                                            "to sign the NVIDIA kernel module "
                                            "with an existing key pair, or "
                                            "would you like to generate a new "
                                            "one?") == 1);

        if (generate_keys) {
            /* ===== 生成新密钥对的流程 ===== */

            char *x509_hash, *private_key_path, *public_key_path;
            int ret, generate_failed = FALSE;
            /* 限制 openssl 命令输出为 8 行（避免过长的输出干扰界面） */
            const RunCommandOutputMatch output_match[] = {
                { .lines = 8, .initial_match = NULL },
                { 0 }
            };

            /* 检查 openssl 工具是否可用 */
            if (!op->utils[OPENSSL]) {
                ui_error(op, "Unable to generate key pair: openssl not "
                         "found!");
                return FALSE;
            }

            /*
             * 确定 X.509 证书使用的哈希算法。
             *
             * XXX 默认使用与内核模块签名相同的哈希算法。
             * 虽然证书的哈希和模块签名的哈希实际上是相互独立的，
             * 但选择模块签名使用的哈希算法可以确保该算法已内置到内核中。
             */
            if (op->module_signing_x509_hash) {
                /* 用户通过 --module-signing-x509-hash 指定了哈希算法 */
                x509_hash = nvstrdup(op->module_signing_x509_hash);
            } else {
                /* 尝试从内核配置中猜测模块签名使用的哈希算法 */
                char *guess, *guess_trimmed, *warn = NULL;

                char *no_guess = "Unable to guess the module signing hash.";
                char *common_warn = "The module signing certificate generated "
                                    "by nvidia-installer will be signed with "
                                    "sha256 as a fallback. If the resulting "
                                    "certificate fails to import into your "
                                    "kernel's trusted keyring, please run the "
                                    "installer again, and either use a pre-"
                                    "generated key pair, or set the "
                                    "--module-signing-x509-hash option if you "
                                    "plan to generate a new key pair with "
                                    "nvidia-installer.";

                guess = guess_module_signing_hash(op,
                                                  p->kernel_module_build_directory);

                if (guess == NULL) {
                    warn = no_guess;
                    goto guess_fail;
                }

                /* 去除猜测结果中的空白和引号 */
                guess_trimmed = nv_trim_space(guess);
                guess_trimmed = nv_trim_char_strict(guess_trimmed, '"');

                if (guess_trimmed) {
                    if (strlen(guess_trimmed) == 0) {
                        warn = no_guess;
                        goto guess_fail;
                    }

                    x509_hash = nvstrdup(guess_trimmed);
                } else {
                    warn = "Error while parsing the detected module signing "
                           "hash.";
                    goto guess_fail;
                }

guess_fail:
                nvfree(guess);

                if (warn) {
                    /* 猜测失败，回退到 sha256 */
                    ui_warn(op, "%s %s", warn, common_warn);
                    x509_hash = nvstrdup("sha256");
                }
            }

            log_printf(op, NULL, "Generating key pair for module signing...");

            /* 创建临时文件用于存放生成的私钥和公钥证书 */

            private_key_path = write_temp_file(op, 0, NULL, 0600);  /* 私钥权限 0600 */
            public_key_path = write_temp_file(op, 0, NULL, 0644);   /* 公钥权限 0644 */

            if (!private_key_path || !public_key_path) {
                ui_error(op, "Failed to create one or more temporary files for "
                         "the module signing keys.");
                generate_failed = TRUE;
                goto generate_done;
            }

            /*
             * 使用 openssl 生成 RSA 2048 位密钥对和自签名 X.509 证书。
             * 生成参数：
             *   - RSA 2048 位密钥
             *   - 有效期 7300 天（约 20 年）
             *   - 不使用密码保护（-nodes）
             *   - 证书主题为 "nvidia-installer generated signing key"
             *   - 输出 DER 格式的证书
             *
             * XXX 假设内核的 sign-file 工具要求 DER 格式的 X.509 证书。
             * 如果未来格式要求发生变化，此处需要相应调整。
             */

            ret = run_command(op, NULL, TRUE, output_match, TRUE,
                              "cd ", p->kernel_module_build_directory, "; ",
                              op->utils[OPENSSL], " req -new -x509 -newkey "
                              "rsa:2048 -days 7300 -nodes -subj "
                              "\"/CN=nvidia-installer generated signing key/\""
                              " -keyout ", private_key_path,
                              " -outform DER -out ", public_key_path,
                              " -", x509_hash, NULL);
            nvfree(x509_hash);

            if (ret != 0) {
                ui_error(op, "Failed to generate key pair!");
                generate_failed = TRUE;
                goto generate_done;
            }

            log_printf(op, NULL, "Signing keys generated successfully.");

            /* 将生成的密钥路径保存到全局选项中 */

            op->module_signing_secret_key = nvstrdup(private_key_path);
            op->module_signing_public_key = nvstrdup(public_key_path);

generate_done:
            nvfree(private_key_path);
            nvfree(public_key_path);

            if (generate_failed) {
                return FALSE;
            }
        } else {
            /* ===== 使用已有密钥对的流程 ===== */
            /* 提示用户输入私钥和公钥的文件路径 */
            op->module_signing_secret_key =
                get_filename(op, op->module_signing_secret_key,
                             "Please provide the path to the private key");
            op->module_signing_public_key =
                get_filename(op, op->module_signing_public_key,
                             "Please provide the path to the public key");
        }
    }

    /*
     * 现在已有签名密钥（用户提供的或安装器生成的），
     * 对之前编译好的所有内核模块逐个签名。
     */

    for (i = 0; i < p->num_kernel_modules; i++) {
        if (!sign_kernel_module(op, p->kernel_module_build_directory,
                                p->kernel_modules[i].module_filename, TRUE)) {
            return FALSE;
        }
    }

    if (generate_keys) {

        /*
         * 如果密钥是新生成的，需要处理两件事：
         * 1. 将公钥证书添加到安装包中，用户后续需要将其导入内核信任密钥库
         * 2. 根据用户选择，删除或保留私钥
         */
        char *name, *result = NULL, *fingerprint;
        char short_fingerprint[9];   /* 证书指纹的前 8 个字符（用于文件名） */
        int ret, delete_secret_key;

        /* 询问用户是否删除私钥（出于安全考虑） */
        delete_secret_key = ui_yes_no(op, TRUE, "The NVIDIA kernel module was "
                                      "successfully signed with a newly "
                                      "generated key pair. Would you like to "
                                      "delete the private signing key?");

        /*
         * 获取 X.509 证书的 SHA1 指纹，用于生成唯一的文件名。
         * 此时 openssl 工具一定存在（因为刚用它生成了密钥对）。
         */
        ret = run_command(op, &result, FALSE, NULL, FALSE,
                          op->utils[OPENSSL], " x509 -noout -fingerprint ",
                          "-inform DER -in ", op->module_signing_public_key,
                          NULL);

        /* openssl 输出格式: "SHA1 Fingerprint=00:00:00:00:..." */
        fingerprint = strchr(result, '=') + 1;

        if (ret != 0 || !fingerprint || strlen(fingerprint) < 40) {
            /* openssl 命令失败或输出解析错误，尝试使用 sha1sum 作为备选 */
            char *sha1sum = find_system_util("sha1sum");

            if (sha1sum) {
                /* 用 sha1sum 直接计算 DER 证书文件的哈希 */
                ret = run_command(op, &result, FALSE, NULL, FALSE,
                                  sha1sum, " ", op->module_signing_public_key,
                                  NULL);
                nvfree(sha1sum);

                fingerprint = result;
            }

            if (!sha1sum || ret != 0 || !fingerprint ||
                strlen(fingerprint) < 40) {
                /* 两种方法都失败了，使用 "UNKNOWN" 作为指纹 */
                fingerprint = "UNKNOWN";
            } else {
                /* sha1sum 输出格式: "<hash>  <filename>"，去掉文件名部分 */
                char *end = strchr(fingerprint, ' ');
                *end = '\0';
            }
        } else {
            /* 成功获取指纹，去除冒号分隔符并截取前 8 个字符作为简短指纹 */
            char *tmp = nv_strreplace(fingerprint, ":", "");
            strncpy(short_fingerprint, tmp, sizeof(short_fingerprint) - 1);
            nvfree(tmp);
        }
        short_fingerprint[sizeof(short_fingerprint) - 1] = '\0';  /* 确保 null 终止 */

        /* 将公钥证书添加到安装包的文件列表中 */

        /* XXX name 在释放 package 时会发生内存泄漏（已知问题，未修复） */
        name = nvstrcat("nvidia-modsign-crt-", short_fingerprint, ".der", NULL);

        add_package_entry(p,
                          nvstrdup(op->module_signing_public_key),
                          NULL, /* path：由 set_destinations() 设置 */
                          name,
                          NULL, /* target：非符号链接 */
                          NULL, /* dst：由 set_destinations() 设置 */
                          FILE_TYPE_MODULE_SIGNING_KEY,
                          FILE_COMPAT_ARCH_NONE,
                          0444);  /* 公钥证书权限：所有人可读 */

        /* 告知用户证书的安装位置和指纹，以及后续操作指引 */
        ui_message(op, "An X.509 certificate containing the public signing "
                    "key will be installed to %s/%s. The SHA1 fingerprint of "
                    "this certificate is: %s.\n\nThis certificate must be "
                    "added to a key database which is trusted by your kernel "
                    "in order for the kernel to be able to verify the module "
                    "signature.", op->module_signing_key_path, name,
                    fingerprint);

        nvfree(result);

        /* 根据用户选择，删除或安装私钥 */

        if (delete_secret_key) {
            /* 安全删除私钥（覆写后删除，防止被恢复） */
            secure_delete(op, op->module_signing_secret_key);
        } else {

            /* 将私钥也添加到安装包中，以便后续复用 */

            name = nvstrcat("nvidia-modsign-key-", short_fingerprint, ".key",
                            NULL);

            add_package_entry(p,
                              nvstrdup(op->module_signing_secret_key),
                              NULL, /* path */
                              name,
                              NULL, /* target */
                              NULL, /* dst */
                              FILE_TYPE_MODULE_SIGNING_KEY,
                              FILE_COMPAT_ARCH_NONE,
                              0400);  /* 私钥权限：仅所有者可读 */

            /* 告知用户私钥的安装位置和安全注意事项 */
            ui_message(op, "The private signing key will be installed to %s/%s. "
                       "After the public key is added to a key database which "
                       "is trusted by your kernel, you may reuse the saved "
                       "public/private key pair to sign additional kernel "
                       "modules, without needing to re-enroll the public key. "
                       "Please take some reasonable precautions to secure the "
                       "private key: see the README for suggestions.",
                       op->module_signing_key_path, name);
        }
    } /* if (generate_keys) - 生成密钥的后处理结束 */

    return TRUE;
} /* assisted_module_signing() */
