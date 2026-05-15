/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003-2009 NVIDIA Corporation
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
 * nvidia-installer.h
 *
 * 【文件说明】nvidia-installer 项目的核心头文件。
 * 定义了安装器所需的全部核心数据结构，包括：
 *   - 系统工具枚举（SystemUtils / SystemOptionalUtils / ModuleUtils / DevelopUtils）
 *   - 安装包文件类型枚举（PackageEntryFileType，60+ 种文件类型）
 *   - 命令行选项结构体（Options，100+ 字段）
 *   - 包条目结构体（PackageEntry）及包结构体（Package）
 *   - 内核模块信息结构体（KernelModuleInfo）
 *   - 各种默认安装路径宏定义
 * 几乎所有 .c 文件都会包含此头文件。
 */

#ifndef __NVIDIA_INSTALLER_H__
#define __NVIDIA_INSTALLER_H__

#include <sys/types.h>   /* mode_t, ino_t, dev_t 等 POSIX 类型 */
#include <inttypes.h>    /* uint8_t, uint16_t, uint32_t 等定宽整数类型 */

#include "common-utils.h" /* 通用工具函数和宏，如 NV_ATTRIBUTE_PRINTF、nvfree 等 */

/*
 * 系统必需工具枚举。
 * 列出安装过程中必须存在的系统工具。
 * 注意：此枚举必须与 misc.c:find_system_utils() 中的 needed_utils 字符串数组保持同步。
 * 安装器启动时会在 PATH 中搜索这些工具，若缺失会报错。
 */

typedef enum {
    MIN_SYSTEM_UTILS = 0,           /* 系统工具枚举起始值 */
    LDCONFIG = MIN_SYSTEM_UTILS,    /* ldconfig：用于更新共享库缓存 */
    GREP,                           /* grep：用于文本搜索（如检查内核配置） */
    DMESG,                          /* dmesg：用于读取内核日志（insmod 测试时使用） */
    TAIL,                           /* tail：用于截取日志末尾内容 */
    MAX_SYSTEM_UTILS                /* 系统必需工具枚举上界（同时作为可选工具枚举起始值） */
} SystemUtils;

/*
 * 系统可选工具枚举。
 * 列出安装过程中可选的系统工具，缺失不会导致安装失败，但某些功能会被禁用。
 * 枚举值从 MAX_SYSTEM_UTILS 开始，确保所有工具类型共享统一的索引空间。
 */
typedef enum {
    MIN_SYSTEM_OPTIONAL_UTILS = MAX_SYSTEM_UTILS,
    OBJCOPY = MIN_SYSTEM_OPTIONAL_UTILS,  /* objcopy：用于处理目标文件（如去除/添加 ELF section） */
    CHCON,                                /* chcon：SELinux 上下文修改工具 */
    SELINUX_ENABLED,                      /* selinuxenabled：检测 SELinux 是否启用 */
    GETENFORCE,                           /* getenforce：获取 SELinux 当前执行模式 */
    EXECSTACK,                            /* execstack：设置/查询可执行栈标记 */
    PKG_CONFIG,                           /* pkg-config：查询已安装库的编译/链接参数 */
    XSERVER,                              /* X：X Server 可执行文件，用于检测 Xorg 版本 */
    OPENSSL,                              /* openssl：用于内核模块签名（Secure Boot） */
    DKMS,                                 /* dkms：动态内核模块支持工具 */
    SYSTEMCTL,                            /* systemctl：systemd 服务管理工具 */
    TAR,                                  /* tar：归档工具，用于打包预编译接口 */
    MAX_SYSTEM_OPTIONAL_UTILS             /* 系统可选工具枚举上界 */
} SystemOptionalUtils;

/*
 * 内核模块管理工具枚举。
 * 列出加载/卸载/查询内核模块所需的工具。
 * 注意：此枚举必须与 misc.c:find_module_utils() 中的 needed_utils 字符串数组保持同步。
 */

typedef enum {
    MIN_MODULE_UTILS = MAX_SYSTEM_OPTIONAL_UTILS,
    MODPROBE = MIN_MODULE_UTILS,    /* modprobe：加载内核模块（自动处理依赖） */
    RMMOD,                          /* rmmod：卸载已加载的内核模块 */
    LSMOD,                          /* lsmod：列出当前已加载的内核模块 */
    DEPMOD,                         /* depmod：生成模块依赖关系文件 */
    MAX_MODULE_UTILS                /* 模块工具枚举上界 */
} ModuleUtils;

/*
 * 开发工具枚举。
 * 列出编译内核模块所需的开发工具。
 * 注意：此枚举必须与 misc.c:check_development_tools() 中的 develop_utils 字符串数组保持同步。
 */

typedef enum {
    MIN_DEVELOP_UTILS = MAX_MODULE_UTILS,
    CC = MIN_DEVELOP_UTILS,         /* cc：C 编译器，用于编译内核模块 */
    MAKE,                           /* make：构建工具，用于执行 Makefile */
    LD,                             /* ld：链接器，用于链接内核模块目标文件 */
    /* TR 和 SED 不被安装器直接使用，但 conftest.sh（内核配置检测脚本）
     * 依赖它们，所以仍需检查其存在性 */
    TR,                             /* tr：字符转换工具 */
    SED,                            /* sed：流编辑器 */
    MAX_DEVELOP_UTILS               /* 开发工具枚举上界 */
} DevelopUtils;

/* 所有工具类型的总上界，用于定义 Options::utils[] 数组大小 */
#define MAX_UTILS MAX_DEVELOP_UTILS

/*
 * 安装包文件类型枚举。
 * 定义了 .manifest 文件中所有可能的文件类型。
 * 注意：此枚举必须与 manifest.c:packageEntryFileTypeTable[] 保持同步。
 * 每种类型都有对应的能力标志（PackageEntryFileCapabilities），
 * 决定该类型文件是否可安装、是否为符号链接、是否为共享库等。
 */
typedef enum {
    FILE_TYPE_NONE,                        /* 无类型/未指定 */
    FILE_TYPE_KERNEL_MODULE_SRC,           /* 内核模块源代码 */
    FILE_TYPE_OPENGL_LIB,                 /* OpenGL 共享库 */
    FILE_TYPE_WINE_LIB,                   /* Wine 兼容层库文件 */
    FILE_TYPE_DOCUMENTATION,              /* 文档文件 */
    FILE_TYPE_OPENGL_SYMLINK,             /* OpenGL 库的符号链接 */
    FILE_TYPE_KERNEL_MODULE,              /* 编译后的内核模块（.ko 文件） */
    FILE_TYPE_INSTALLER_BINARY,           /* 安装器自身的二进制文件 */
    FILE_TYPE_UTILITY_BINARY,             /* 工具程序（如 nvidia-smi） */
    FILE_TYPE_TLS_LIB,                    /* 线程本地存储（TLS）库 */
    FILE_TYPE_UTILITY_LIB,               /* 工具程序依赖的库文件 */
    FILE_TYPE_DOT_DESKTOP,               /* .desktop 桌面入口文件 */
    FILE_TYPE_ICON,                       /* 图标文件 */
    FILE_TYPE_UTILITY_LIB_SYMLINK,       /* 工具库的符号链接 */
    FILE_TYPE_XMODULE_SHARED_LIB,        /* X.Org 模块共享库 */
    FILE_TYPE_MANPAGE,                    /* man 手册页 */
    FILE_TYPE_EXPLICIT_PATH,             /* 使用显式路径安装的文件 */
    FILE_TYPE_CUDA_LIB,                  /* CUDA 库文件 */
    FILE_TYPE_OPENCL_LIB,               /* OpenCL 库文件 */
    FILE_TYPE_OPENCL_WRAPPER_LIB,        /* OpenCL 包装器库 */
    FILE_TYPE_CUDA_SYMLINK,              /* CUDA 库的符号链接 */
    FILE_TYPE_OPENCL_LIB_SYMLINK,        /* OpenCL 库的符号链接 */
    FILE_TYPE_OPENCL_WRAPPER_SYMLINK,    /* OpenCL 包装器库的符号链接 */
    FILE_TYPE_VDPAU_LIB,                /* VDPAU（视频解码加速）库 */
    FILE_TYPE_VDPAU_SYMLINK,            /* VDPAU 库的符号链接 */
    FILE_TYPE_UTILITY_BIN_SYMLINK,       /* 工具程序的符号链接 */
    FILE_TYPE_CUDA_ICD,                  /* CUDA ICD（可安装客户端驱动）配置文件 */
    FILE_TYPE_NVCUVID_LIB,              /* NVCUVID（NVIDIA CUDA 视频解码器）库 */
    FILE_TYPE_NVCUVID_LIB_SYMLINK,      /* NVCUVID 库的符号链接 */
    FILE_TYPE_GLX_MODULE_SHARED_LIB,    /* GLX 模块共享库（libglx.so） */
    FILE_TYPE_GLX_MODULE_SYMLINK,        /* GLX 模块的符号链接 */
    FILE_TYPE_ENCODEAPI_LIB,            /* 视频编码 API（NVENC）库 */
    FILE_TYPE_ENCODEAPI_LIB_SYMLINK,    /* 视频编码 API 库的符号链接 */
    FILE_TYPE_VGX_LIB,                  /* VGX（虚拟 GPU）库 */
    FILE_TYPE_VGX_LIB_SYMLINK,          /* VGX 库的符号链接 */
    FILE_TYPE_GRID_LIB,                 /* GRID（企业虚拟化）库 */
    FILE_TYPE_GRID_LIB_SYMLINK,         /* GRID 库的符号链接 */
    FILE_TYPE_APPLICATION_PROFILE,       /* 应用程序性能配置文件 */
    FILE_TYPE_NVIDIA_MODPROBE,           /* nvidia-modprobe 工具（自动加载设备节点） */
    FILE_TYPE_NVIDIA_MODPROBE_MANPAGE,   /* nvidia-modprobe 的 man 手册 */
    FILE_TYPE_MODULE_SIGNING_KEY,        /* 内核模块签名密钥文件 */
    FILE_TYPE_XORG_OUTPUTCLASS_CONFIG,   /* Xorg OutputClass 配置文件 */
    FILE_TYPE_DKMS_CONF,                /* DKMS 配置文件（dkms.conf） */
    FILE_TYPE_GLVND_LIB,                /* GLVND（GL Vendor-Neutral Dispatch）库 */
    FILE_TYPE_GLVND_SYMLINK,            /* GLVND 库的符号链接 */
    FILE_TYPE_VULKAN_ICD_JSON,           /* Vulkan ICD（可安装客户端驱动）JSON 配置 */
    FILE_TYPE_GLX_CLIENT_LIB,           /* GLX 客户端库 */
    FILE_TYPE_GLX_CLIENT_SYMLINK,        /* GLX 客户端库的符号链接 */
    FILE_TYPE_GLVND_EGL_ICD_JSON,       /* GLVND EGL ICD JSON 配置 */
    FILE_TYPE_EGL_CLIENT_LIB,           /* EGL 客户端库 */
    FILE_TYPE_EGL_CLIENT_SYMLINK,        /* EGL 客户端库的符号链接 */
    FILE_TYPE_EGL_EXTERNAL_PLATFORM_JSON,/* EGL 外部平台 JSON 配置 */
    FILE_TYPE_FLEXERA_LIB,              /* Flexera 许可证管理库 */
    FILE_TYPE_FLEXERA_LIB_SYMLINK,      /* Flexera 库的符号链接 */
    FILE_TYPE_INTERNAL_UTILITY_BINARY,   /* 内部工具程序（不对外暴露） */
    FILE_TYPE_INTERNAL_UTILITY_LIB,     /* 内部工具库 */
    FILE_TYPE_INTERNAL_UTILITY_DATA,    /* 内部工具数据文件 */
    FILE_TYPE_FIRMWARE,                  /* GPU 固件文件 */
    FILE_TYPE_SYSTEMD_UNIT,             /* systemd 服务单元文件 */
    FILE_TYPE_SYSTEMD_UNIT_SYMLINK,     /* systemd 服务单元的符号链接 */
    FILE_TYPE_SYSTEMD_SLEEP_SCRIPT,     /* systemd 睡眠/唤醒钩子脚本 */
    FILE_TYPE_GBM_BACKEND_LIB,          /* GBM（Generic Buffer Management）后端库 */
    FILE_TYPE_GBM_BACKEND_LIB_SYMLINK,  /* GBM 后端库的符号链接 */
    FILE_TYPE_OPENGL_DATA,              /* OpenGL 数据文件 */
    FILE_TYPE_VULKANSC_ICD_JSON,        /* Vulkan SC（Safety Critical）ICD JSON 配置 */
    FILE_TYPE_SANDBOXUTILS_FILELIST_JSON,/* 沙箱工具文件列表 JSON */
    FILE_TYPE_MAX                        /* 文件类型枚举上界，用于数组大小 */
} PackageEntryFileType;


/* 简化的定宽整数类型别名 */
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;

/* 不确定进度指示器的内部数据结构（前向声明，定义在 ui-status-indeterminate.c 中） */
typedef struct __indeterminate_data IndeterminateData;

/*
 * Options 结构体：安装器的核心配置结构。
 * 由 parse_commandline() 分配和初始化，几乎所有安装器函数都通过指针接收此结构。
 * 包含命令行选项、安装路径、系统工具路径、UI 状态等所有运行时配置。
 */


typedef struct __options {

    /* ===== 布尔型命令行选项 ===== */

    int expert;              /* --expert：专家模式，显示更多详细信息 */
    int uninstall;           /* --uninstall：执行卸载操作 */
    int skip_module_unload;  /* --skip-module-unload：跳过卸载旧内核模块步骤 */
    int driver_info;         /* --driver-info：仅显示已安装驱动信息 */
    int debug;               /* --debug：启用调试输出 */
    int logging;             /* 是否启用日志记录（默认启用） */
    int no_precompiled_interface; /* --no-precompiled-interface：禁止使用预编译的内核接口 */
    int no_ncurses_color;    /* --no-ncurses-color：禁用 ncurses UI 的颜色 */
    int nvidia_modprobe;     /* --nvidia-modprobe：安装 nvidia-modprobe 工具 */
    int no_questions;        /* --no-questions：安装过程中不提问（使用默认值） */
    int silent;              /* --silent：静默模式，不输出任何信息 */
    int sanity;              /* --sanity：执行现有安装的完整性检查 */
    int add_this_kernel;     /* --add-this-kernel：为当前运行的内核添加预编译接口 */
    int no_backup;           /* --no-backup：安装时不备份被替换的文件 */
    int kernel_modules_only; /* --kernel-modules-only：仅安装内核模块 */
    int no_kernel_modules;   /* --no-kernel-modules：跳过内核模块安装 */
    int no_abi_note;         /* --no-abi-note：不检查 ABI 注释 */
    int no_rpms;             /* --no-rpms：不检查 RPM 包冲突 */
    int no_recursion;        /* --no-recursion：不递归搜索冲突文件 */
    int run_nvidia_xconfig;  /* --run-nvidia-xconfig：安装后运行 nvidia-xconfig 配置 X */
    int selinux_option;      /* --selinux：SELinux 设置选项（SELINUX_DEFAULT/FORCE_YES/FORCE_NO） */
    int selinux_enabled;     /* 运行时检测到 SELinux 是否启用 */
    int sigwinch_workaround; /* --sigwinch-workaround：启用 SIGWINCH 信号处理的变通方案 */
    int no_x_check;          /* --no-x-check：跳过 X Server 运行检测 */
    int no_nvidia_xconfig_question; /* 不询问是否运行 nvidia-xconfig */
    int run_distro_scripts;  /* --run-distro-scripts：运行发行版特定的安装脚本 */
    int no_nouveau_check;    /* --no-nouveau-check：跳过 nouveau 驱动冲突检测 */
    int disable_nouveau;     /* --disable-nouveau：自动禁用 nouveau 驱动 */
    int no_opengl_files;     /* --no-opengl-files：不安装 OpenGL 文件 */
    int no_wine_files;       /* --no-wine-files：不安装 Wine 兼容文件 */
    int no_kernel_module_source; /* --no-kernel-module-source：不安装内核模块源码 */
    int dkms;                /* --dkms：使用 DKMS 管理内核模块 */
    int check_for_alternate_installs; /* --check-for-alternate-installs：检查其他安装方式（如包管理器） */
    int install_uvm;         /* 是否安装 nvidia-uvm（Unified Virtual Memory）模块 */
    int install_drm;         /* 是否安装 nvidia-drm（DRM/KMS 支持）模块 */
    int install_peermem;     /* 是否安装 nvidia-peermem（GPU 对等内存）模块 */
    int compat32_files_packaged; /* 包中是否包含 32 位兼容库 */
    int x_files_packaged;    /* 包中是否包含 X.Org 相关文件 */
    int vulkan_icd_json_packaged;   /* 包中是否包含 Vulkan ICD JSON 配置 */
    int vulkansc_icd_json_packaged; /* 包中是否包含 Vulkan SC ICD JSON 配置 */
    int concurrency_level;   /* --concurrency-level：内核模块并行编译级别（make -j） */
    int skip_module_load;    /* --skip-module-load：安装后不加载内核模块 */
    int skip_depmod;         /* --skip-depmod：安装后不运行 depmod */
    int allow_installation_with_running_driver; /* --allow-installation-with-running-driver：允许在驱动运行时安装 */
    int loaded_kernel_module_detected;  /* 运行时检测到已加载的 NVIDIA 内核模块 */
    int running_x_server_detected;      /* 运行时检测到正在运行的 X Server */

    /* ===== 三态可选布尔值（NV_OPTIONAL_BOOL_TRUE/FALSE/DEFAULT） ===== */

    NVOptionalBool install_libglx_indirect;   /* 是否安装间接 GLX 渲染库 */
    NVOptionalBool install_libglvnd_libraries;/* 是否安装 GLVND 库 */
    NVOptionalBool install_compat32_libs;     /* 是否安装 32 位兼容库 */
    NVOptionalBool rebuild_initramfs;         /* 安装后是否重建 initramfs */

    /* 每种文件类型的安装目标路径覆盖，由命令行参数设置 */
    char *file_type_destination_overrides[FILE_TYPE_MAX];

    /* ===== 安装路径配置 ===== */

    /* OpenGL 库安装路径 */
    char *opengl_prefix;     /* OpenGL 安装前缀（默认 /usr） */
    char *opengl_libdir;     /* OpenGL 库目录（如 lib 或 lib64） */

    /* Wine 兼容库安装路径 */
    char *wine_prefix;       /* Wine 库安装前缀 */
    char *wine_libdir;       /* Wine 库目录 */

    /* X.Org 相关安装路径 */
    char *x_prefix;          /* X.Org 安装前缀（默认 /usr/X11R6 或 /usr） */
    char *x_libdir;          /* X.Org 库目录 */
    char *x_moddir;          /* X.Org 模块目录 */
    char *x_module_path;     /* X.Org 模块搜索路径（完整路径） */
    char *x_library_path;    /* X.Org 库搜索路径 */
    char *x_sysconfig_path;  /* X.Org 系统配置路径（如 /etc/X11/xorg.conf.d） */

    /* 32 位兼容库安装路径（仅 x86_64 系统） */
    char *compat32_chroot;   /* 32 位兼容 chroot 前缀（Debian 旧方式） */
    char *compat32_prefix;   /* 32 位兼容库安装前缀 */
    char *compat32_libdir;   /* 32 位兼容库目录 */
    char *compat32_gbm_backend_dir; /* 32 位 GBM 后端目录 */

    /* 工具程序安装路径 */
    char *utility_prefix;    /* 工具程序安装前缀（默认 /usr） */
    char *utility_libdir;    /* 工具程序库目录 */
    char *utility_bindir;    /* 工具程序可执行文件目录（默认 bin） */
    char *installer_prefix;  /* 安装器自身的安装前缀 */

    char *gbm_backend_dir;   /* GBM 后端库安装目录 */

    /* 桌面集成相关路径 */
    char *xdg_data_dir;      /* XDG 数据目录（.desktop 文件） */
    char *icon_dir;          /* 图标文件目录 */

    /* 文档安装路径 */
    char *documentation_prefix;  /* 文档安装前缀 */
    char *documentation_docdir;  /* 文档目录（如 share/doc） */
    char *documentation_mandir;  /* man 手册目录（如 share/man） */

    char *application_profile_path; /* 应用程序性能配置文件路径 */

    /* X.Org 版本特性检测结果 */
    int modular_xorg;              /* 是否为模块化 Xorg（7.x+）而非 monolithic XFree86 */
    int xorg_supports_output_class;/* Xorg 是否支持 OutputClass 配置（xorg.conf.d） */

    /* ===== 内核相关路径 ===== */

    char *kernel_source_path;    /* 内核源码路径（如 /lib/modules/$(uname -r)/build） */
    char *kernel_output_path;    /* 内核构建输出路径（通常与源码路径相同） */
    char *kernel_include_path;   /* 内核头文件路径 */
    char *kernel_module_installation_path; /* 内核模块安装路径（如 /lib/modules/$(uname -r)/kernel/drivers/video） */
    char *kernel_module_src_prefix;  /* 内核模块源码安装前缀（如 /usr/src） */
    char *kernel_module_src_dir;     /* 内核模块源码安装目录名 */
    char *utils[MAX_UTILS];      /* 所有外部工具的完整路径数组，下标为各工具枚举值 */

    /* ===== 系统路径 ===== */

    char *proc_mount_point;      /* /proc 挂载点（默认 /proc） */
    char *log_file_name;         /* 安装日志文件路径（默认 /var/log/nvidia-installer.log） */

    char *tmpdir;                /* 临时目录路径 */
    char *kernel_name;           /* 目标内核版本名（如 5.15.0-generic），可通过 --kernel-name 指定 */
    char *rpm_file_list;         /* RPM 文件列表路径（用于冲突检测） */
    char *precompiled_kernel_interfaces_path; /* 预编译内核接口搜索路径 */
    const char *selinux_chcon_type; /* SELinux 安全上下文类型（如 textrel_shlib_t） */

    /* ===== 内核模块签名相关（Secure Boot 支持） ===== */

    char *module_signing_secret_key;   /* 模块签名私钥文件路径 */
    char *module_signing_public_key;   /* 模块签名公钥/证书文件路径 */
    char *module_signing_script;       /* 自定义模块签名脚本路径 */
    char *module_signing_key_path;     /* 签名密钥搜索/生成路径 */
    char *module_signing_hash;         /* 签名使用的哈希算法（如 sha256） */
    char *module_signing_x509_hash;    /* X.509 证书使用的哈希算法 */

    char *libglvnd_json_path;          /* GLVND EGL ICD JSON 配置路径 */

    char *external_platform_json_path; /* EGL 外部平台 JSON 配置路径 */

    int kernel_module_signed;    /* 标记：内核模块是否已成功签名 */
    int dkms_registered;         /* 标记：模块是否已注册到 DKMS */

    /* ===== systemd 相关路径 ===== */

    NVOptionalBool use_systemd;        /* 是否使用 systemd 管理服务 */
    char *systemd_unit_prefix;         /* systemd 单元文件安装路径（默认 /usr/lib/systemd/system） */
    char *systemd_sleep_prefix;        /* systemd 睡眠脚本安装路径 */
    char *systemd_sysconf_prefix;      /* systemd 系统配置路径（默认 /etc/systemd/system） */

    char *kernel_module_build_directory_override; /* 内核模块构建目录覆盖路径 */

    /* ===== UI 子系统状态 ===== */
    struct {
        char *name;              /* 当前 UI 库名称（如 "ncurses6"、"stream"） */
        void *priv;              /* UI 库的私有数据指针（由具体 UI 实现分配） */
        int status_active;       /* 是否有活跃的进度条/状态显示 */
        IndeterminateData *indeterminate_data; /* 不确定进度指示器数据 */
    } ui;

    /* ===== PCI 设备检测结果 ===== */
    struct {
        unsigned int found_supported   :1; /* 是否检测到当前驱动支持的 GPU */
        unsigned int found_vga         :1; /* 是否检测到 NVIDIA VGA 设备 */

        int num_legacy;         /* 检测到的旧版（legacy）GPU 数量 */
        int legacy[16];         /* 旧版 GPU 所需的驱动分支号数组 */
    } pci_devices;

    /* ===== 开源内核模块选项 ===== */
    struct {
        unsigned int required                :1; /* 是否要求使用开源模块 */
        unsigned int supported_gpu_present   :1; /* 是否存在支持开源模块的 GPU */
        unsigned int unsupported_gpu_present :1; /* 是否存在不支持开源模块的 GPU */
    } open_modules;

    /* ===== UI 延迟消息队列 ===== */
    /* 在 UI 初始化完成之前产生的消息会被缓存在此，待 UI 就绪后统一显示 */
    int num_ui_deferred_messages;  /* 延迟消息数量 */
    struct {
        int level;        /* 消息级别（error/warn/message 等） */
        char *message;    /* 消息文本 */
    } *ui_deferred_messages;      /* 延迟消息数组（动态分配） */
} Options;

/*
 * 文件兼容架构枚举。
 * 用于区分文件是原生架构还是 32 位兼容架构（在 64 位系统上安装 32 位库时使用）。
 */
typedef enum {
    FILE_COMPAT_ARCH_NONE,      /* 无架构信息（如文档、配置文件） */
    FILE_COMPAT_ARCH_NATIVE,    /* 原生架构（当前系统架构） */
    FILE_COMPAT_ARCH_COMPAT32,  /* 32 位兼容架构 */
} PackageEntryFileCompatArch;

/*
 * 文件类型能力标志位域。
 * 描述每种 PackageEntryFileType 的属性和行为特征。
 * 在 manifest.c:packageEntryFileTypeTable[] 中为每种文件类型定义这些标志。
 */
typedef struct {
    unsigned int has_arch       : 1; /* 文件是否区分架构（有 native/compat32 之分） */
    unsigned int installable    : 1; /* 文件是否需要安装到目标系统 */
    unsigned int has_path       : 1; /* .manifest 中是否指定了安装子路径 */
    unsigned int is_symlink     : 1; /* 该条目是否为符号链接 */
    unsigned int is_shared_lib  : 1; /* 是否为共享库（需要处理 soname 等） */
    unsigned int is_opengl      : 1; /* 是否为 OpenGL 相关文件（受 --no-opengl-files 影响） */
    unsigned int is_temporary   : 1; /* 是否为临时文件（安装后不保留） */
    unsigned int is_conflicting : 1; /* 是否需要检查文件冲突 */
    unsigned int inherit_path   : 1; /* 是否从父条目继承安装路径 */
} PackageEntryFileCapabilities;

/*
 * 文件类型布尔数组。
 * types[] 以 PackageEntryFileType 枚举值为下标，值为布尔值。
 * 用于高效判断一组文件类型是否被选中（如：哪些类型需要冲突检测）。
 */
typedef struct {
    uint8_t types[FILE_TYPE_MAX];
} PackageEntryFileTypeList;


/*
 * PackageEntry：安装包中单个文件条目的描述。
 * 每个条目对应 .manifest 文件中的一行，描述了一个待安装的文件（或符号链接）。
 */
typedef struct __package_entry {

    char *file;     /* 包内文件名，相对于 .manifest 文件所在目录的路径 */

    char *path;     /* 安装子路径（在目标前缀下的相对路径） */


    char *name;     /* 不含目录部分的纯文件名；直接指向 file 字段中最后一个 '/' 之后的位置 */

    char *target;   /* 对于符号链接条目，指定链接目标；非符号链接条目为 NULL */

    char *dst;      /*
                     * 安装目标的完整绝对路径。
                     * 由 set_destinations() 函数计算并赋值。
                     * 对于不需要安装到文件系统的条目为 NULL。
                     */

    PackageEntryFileCapabilities caps;  /* 文件类型能力标志（是否可安装、是否为链接等） */
    PackageEntryFileType type;          /* 文件类型枚举值 */
    PackageEntryFileCompatArch compat_arch; /* 文件兼容架构（native/compat32） */
    int inherit_path_depth;             /* 路径继承深度（inherit_path 时使用） */

    mode_t mode;    /* 文件权限模式（如 0755） */

    ino_t inode;    /* 文件从包中提取后的 inode 号 */
    dev_t device;   /*
                     * 文件从包中提取后的设备号。
                     * 需要与用户系统上的文件进行比较，
                     * 以防止符号链接循环导致误删包中的文件。
                     */
} PackageEntry;

/*
 * ConflictingFileInfo：冲突文件信息描述。
 * 安装器会搜索系统中与待安装文件冲突的现有文件（如旧版本的同名库），
 * 此结构定义了冲突文件的匹配规则。
 */

typedef struct {
    const char *name;  /* 冲突文件名模式（用于匹配） */
    int len;           /* 文件名长度 */

    /*
     * requiredString：额外的匹配条件。
     * 若非 NULL，则文件内容中必须包含此字符串才被视为冲突文件。
     * 例如：对于 "libglx.*" 文件，只有包含 "glxModuleData" 字符串的才是
     * NVIDIA 版本的 GLX 模块，需要被识别为冲突文件。
     */

    const char *requiredString;
} ConflictingFileInfo;


/*
 * KernelModuleInfo：内核模块信息结构。
 * 存储构建和识别各个内核模块所需的信息。
 * NVIDIA 驱动包含多个内核模块（nvidia、nvidia-modeset、nvidia-uvm、nvidia-drm、nvidia-peermem），
 * 每个模块的构建方式和可选性不同。
 */
typedef struct {
    char *module_name;               /* 模块名称，如 "nvidia"、"nvidia-uvm" */
    char *module_filename;           /* 模块文件名，如 "nvidia.ko" */
    int has_separate_interface_file; /* 是否有独立的内核接口文件（如 nvidia 有 nv-linux.o，但 nvidia-uvm 没有） */
    char *interface_filename;        /* 内核接口目标文件名，如 "nv-linux.o"（编译时生成） */
    char *core_object_name;          /* 核心目标文件名，如 "nv-kernel.o"（预编译的闭源部分） */
    int is_optional;                 /* 是否为可选模块（如 nvidia-uvm 可通过 --no-unified-memory 跳过） */
    char *optional_module_dependee;  /* 可选模块的功能依赖说明，如 "CUDA"（用于提示用户） */
    char *disable_option;            /* 禁用此可选模块的命令行选项，如 "--no-unified-memory" */
    int option_offset;               /* 该选项在 Options 结构体中的字段偏移量（用于运行时访问） */
} KernelModuleInfo;


/*
 * Package：整个安装包的描述结构。
 * 由 parse_manifest() 解析 .manifest 文件后创建，
 * 包含包的元信息和所有文件条目列表。
 */
typedef struct __package {

    char *description;          /* 包描述字符串（如 "NVIDIA Accelerated Graphics Driver for Linux-x86_64"） */
    char *version;              /* 驱动版本号（如 "535.129.03"） */
    char *kernel_module_build_directory; /* 内核模块构建目录名 */
    char *kernel_make_logs;     /* 内核模块编译日志内容（编译失败时用于诊断） */

    PackageEntry *entries;      /* 包文件条目数组（所有待安装的文件） */
    int num_entries;            /* 包文件条目总数 */

    KernelModuleInfo *kernel_modules;  /* 内核模块信息数组 */
    int num_kernel_modules;            /* 内核模块数量 */
    char *excluded_kernel_modules;     /* 被排除的内核模块名称（用逗号分隔） */

} Package;


/* install_from_cwd() 的行为标志 */
#define ADJUST_CWD  0x01  /* 调整当前工作目录到 .manifest 所在目录 */


/* 字符串缓冲区默认长度 */
#define NV_LINE_LEN 1024       /* 默认行缓冲区长度 */
#define NV_MIN_LINE_LEN 256    /* 最小行缓冲区长度 */

/* SELinux 选项常量 */
#define SELINUX_DEFAULT             0x0000  /* 使用默认 SELinux 处理策略 */
#define SELINUX_FORCE_YES           0x0001  /* 强制启用 SELinux 上下文设置 */
#define SELINUX_FORCE_NO            0x0002  /* 强制禁用 SELinux 上下文设置 */

/* 文件权限掩码：提取 rwx 权限位 */
#define PERM_MASK (S_IRWXU|S_IRWXG|S_IRWXO)

/* 预编译内核接口包的文件名 */
#define PRECOMPILED_PACKAGE_FILENAME "nvidia-precompiled"

/*
 * 默认安装路径前缀和相对路径。
 * 部分默认值会根据发行版或检测到的 Xorg 版本（模块化 vs 整体式）进行覆盖。
 * 所有前缀和路径都可以通过命令行参数覆盖。
 */
#define DEFAULT_OPENGL_PREFIX            "/usr"                     /* OpenGL 库安装前缀 */
#define DEFAULT_X_PREFIX                 "/usr/X11R6"               /* X.Org 安装前缀（XFree86 风格） */
#define DEFAULT_UTILITY_PREFIX           "/usr"                     /* 工具程序安装前缀 */
#define DEFAULT_DOCUMENTATION_PREFIX     "/usr"                     /* 文档安装前缀 */
#define DEFAULT_GBM_PREFIX               "/usr"                     /* GBM 库安装前缀 */
#define DEFAULT_APPLICATION_PROFILE_PATH "/usr/share/nvidia"        /* 应用性能配置文件路径 */
#define DEFAULT_WINE_PREFIX              "/usr"                     /* Wine 库安装前缀 */

/* 各架构的库目录名。安装器会根据系统架构和发行版选择合适的目录 */
#define DEFAULT_LIBDIR                  "lib"                       /* 通用库目录 */
#define DEFAULT_32BIT_LIBDIR            "lib32"                     /* 32 位库目录 */
#define DEFAULT_64BIT_LIBDIR            "lib64"                     /* 64 位库目录 */
#define DEFAULT_IA32_TRIPLET_LIBDIR     "lib/i386-linux-gnu"        /* Debian i386 三元组库目录 */
#define DEFAULT_AMD64_TRIPLET_LIBDIR    "lib/x86_64-linux-gnu"      /* Debian amd64 三元组库目录 */
#define DEFAULT_ARMV7_TRIPLET_LIBDIR    "lib/arm-linux-gnueabi"     /* ARM softfloat 三元组库目录 */
#define DEFAULT_ARMV7HF_TRIPLET_LIBDIR  "lib/arm-linux-gnueabihf"   /* ARM hardfloat 三元组库目录 */
#define DEFAULT_AARCH64_TRIPLET_LIBDIR  "lib/aarch64-linux-gnu"     /* AArch64 三元组库目录 */
#define DEFAULT_PPC64LE_TRIPLET_LIBDIR  "lib/powerpc64le-linux-gnu"  /* POWER9 LE 三元组库目录 */
#define DEFAULT_WINE_LIBDIR_SUFFIX      "nvidia/wine"               /* Wine 库子目录后缀 */
#define DEFAULT_BINDIR                  "bin"                        /* 可执行文件目录 */
#define DEFAULT_X_MODULEDIR             "modules"                    /* X.Org 模块目录 */
#define DEFAULT_XDG_DATA_DIR            "share"                      /* XDG 数据目录 */
#define DEFAULT_DOCDIR                  "share/doc"                  /* 文档目录 */
#define DEFAULT_MANDIR                  "share/man"                  /* man 手册目录 */
#define DEFAULT_CONFDIR                 "X11/xorg.conf.d"            /* Xorg 配置目录 */

#define DEFAULT_MODULE_SIGNING_KEY_PATH "/usr/share/nvidia"          /* 模块签名密钥存储路径 */
#define DEFAULT_KERNEL_MODULE_SRC_PREFIX "/usr/src"                  /* 内核模块源码安装前缀 */
#define DEFAULT_X_DATAROOT_PATH         "/usr/share"                 /* X 数据根目录 */
#define DEFAULT_SYSTEMD_UNIT_PREFIX     "/usr/lib/systemd/system"    /* systemd 单元文件目录 */
#define DEFAULT_SYSTEMD_SLEEP_PREFIX    "/usr/lib/systemd/system-sleep" /* systemd 睡眠钩子目录 */
#define DEFAULT_SYSTEMD_SYSCONF_PREFIX  "/etc/systemd/system"        /* systemd 系统配置目录 */

/*
 * Xorg 7.x 模块化安装路径。
 * 从 Xorg 7.x 起，X 组件不再需要安装到专门的顶层目录，
 * 可以与系统其他部分更紧密地集成。安装器会查询系统获取实际路径，
 * 若查询失败则使用以下回退值。
 */
#define XORG7_DEFAULT_X_PREFIX          "/usr"               /* Xorg 7.x 默认安装前缀 */
#define XORG7_DEFAULT_X_MODULEDIR       "xorg/modules"       /* Xorg 7.x 默认模块目录 */

#define DEFAULT_GLVND_EGL_JSON_PATH     "/usr/share/glvnd/egl_vendor.d"  /* GLVND EGL 厂商 JSON 路径 */

#define DEFAULT_EGL_EXTERNAL_PLATFORM_JSON_PATH "/usr/share/egl/egl_external_platform.d" /* EGL 外部平台 JSON 路径 */

/*
 * 旧版 Debian x86-64 系统的 32 位兼容库 chroot 前缀。
 * 旧版 Debian 将 32 位兼容库安装在一个类 chroot 的顶层目录下，
 * 下方前缀会被添加到完整路径之前。
 */
#define DEBIAN_DEFAULT_COMPAT32_CHROOT  "/emul/ia32-linux"

#define DEFAULT_PROC_MOUNT_POINT "/proc"                     /* 默认 /proc 挂载点 */

#define DEFAULT_LOG_FILE_NAME "/var/log/nvidia-installer.log"          /* 安装日志文件 */
#define DEFAULT_UNINSTALL_LOG_FILE_NAME "/var/log/nvidia-uninstall.log" /* 卸载日志文件 */

#define NUM_TIMES_QUESTIONS_ASKED 3    /* 连续询问同一问题的最大次数 */

#define LD_OPTIONS "-d -r"             /* 链接器选项：-d 强制解析符号，-r 生成可重定位目标文件 */
#define NVIDIA_VERSION_PROC_FILE "/proc/driver/nvidia/version" /* NVIDIA 驱动版本 proc 文件 */

#define NV_BULLET_STR "-> "            /* 列表项前缀符号 */
#define NV_CMD_OUT_PREFIX "   "        /* 命令输出缩进前缀 */

/*
 * OpenCL ICD（可安装客户端驱动）加载器会在以下目录中查找 NVIDIA ICD 配置文件。
 */
#define DEFAULT_CUDA_ICD_PREFIX          "/etc"              /* OpenCL ICD 前缀 */
#define DEFAULT_CUDA_ICD_DIR             "OpenCL/vendors"    /* OpenCL ICD 子目录 */

/* 通用工具宏 */

#define NV_MIN(x,y) ((x) < (y) ? (x) : (y))  /* 取两值中的较小值 */
#define NV_MAX(x,y) ((x) > (y) ? (x) : (y))  /* 取两值中的较大值 */

#define TAB "  "         /* 缩进用制表符（2 空格） */
#define BIGTAB "      "  /* 大缩进（6 空格） */

/* ===== 全局函数原型声明 ===== */

/* 日志系统初始化（打开日志文件，记录命令行参数），定义在 log.c */
void log_init(Options *op, int argc, char * const argv[]);
/* 写入日志文件，定义在 log.c */
void log_printf(Options *op, const char *prefix, const char *fmt, ...) NV_ATTRIBUTE_PRINTF(3, 4);

/* 从当前目录执行安装流程（核心安装入口），定义在 install-from-cwd.c */
int  install_from_cwd(Options *op);
/* 为当前运行的内核添加预编译接口，定义在 install-from-cwd.c */
int  add_this_kernel(Options *op);

/* 向 Package 结构体添加一个文件条目，定义在 install-from-cwd.c */
void add_package_entry(Package *p,
                       char *file,
                       char *path,
                       char *name,
                       char *target,
                       char *dst,
                       PackageEntryFileType type,
                       PackageEntryFileCompatArch compat_arch,
                       mode_t mode);

/* 文本格式化函数类型定义：将文本格式化为指定宽度的行（用于 UI 显示） */
typedef TextRows *(*FormatTextRows)(const char*, const char*, int, int);


#endif /* __NVIDIA_INSTALLER_H__ */
