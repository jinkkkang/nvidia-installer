/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2013 NVIDIA Corporation
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
 * 【文件说明】manifest.c -- 清单文件（.manifest）的文件类型管理实现
 *
 * 本文件是 nvidia-installer 项目的核心组成部分，负责管理安装包中所有文件的
 * 类型定义及其属性。NVIDIA 驱动安装包通过 .manifest 清单文件描述包内每个文件
 * 的类型、安装路径、架构等信息，本文件提供了类型定义表和相关查询/过滤函数。
 *
 * 主要数据结构：
 *   - packageEntryFileTypeTable[]：静态文件类型定义表，包含 60+ 种文件类型，
 *     每种类型定义了名称字符串、枚举值和能力标志（PackageEntryFileCapabilities）。
 *
 * 主要功能函数：
 *   - get_file_type_capabilities()：根据文件类型枚举值查询其能力标志
 *   - parse_manifest_file_type()：将 .manifest 中的类型字符串解析为枚举值
 *   - get_installable_file_type_list()：根据用户选项生成可安装文件类型列表
 *   - add_symlinks_to_file_type_list()：向文件类型列表中添加所有符号链接类型
 *   - remove_file_type_from_file_type_list()：从文件类型列表中移除指定类型
 *
 * 能力标志（PackageEntryFileCapabilities）各字段含义：
 *   - has_arch：该文件是否区分 CPU 架构（如 x86 vs x86_64）
 *   - installable：该文件类型是否可直接安装（符号链接类型通常不可直接安装）
 *   - has_path：该文件在 .manifest 中是否携带自定义安装路径
 *   - is_symlink：该文件类型是否为符号链接（而非实际文件）
 *   - is_shared_lib：该文件是否为共享库（.so 文件）
 *   - is_opengl：该文件是否属于 OpenGL/EGL/GLX 相关组件
 *   - is_temporary：该文件是否为临时文件（安装后不保留或仅在构建阶段使用）
 *   - is_conflicting：该文件是否可能与其他软件包冲突（需要特殊处理）
 *   - inherit_path：该文件类型是否从其关联的主文件继承安装路径
 */

/* 标准库：提供 strcmp() 和 memset() 等字符串/内存操作函数 */
#include <string.h>

/* 项目头文件：包含 PackageEntryFileType 枚举、PackageEntryFileCapabilities
 * 结构体、PackageEntryFileTypeList 结构体以及函数声明 */
#include "manifest.h"

/*
 * 为了让文件类型定义表更加简洁易读，将 TRUE/FALSE 缩写为 T/F。
 * 这样每行定义表条目可以对齐显示各个标志位的值。
 */
#define T TRUE
#define F FALSE

/*
 * ENTRY 宏：用于简化 packageEntryFileTypeTable[] 中每个条目的定义。
 *
 * 该宏接收文件类型名称和 9 个能力标志参数，展开后生成三个字段：
 *   1. 字符串形式的类型名称（通过 # 字符串化操作符），如 "KERNEL_MODULE_SRC"
 *   2. 枚举值（通过 ## 拼接操作符），如 FILE_TYPE_KERNEL_MODULE_SRC
 *   3. PackageEntryFileCapabilities 结构体（使用 C99 指定初始化器）
 *
 * 参数说明：
 *   _name          - 文件类型名称（不带 FILE_TYPE_ 前缀），同时用于生成字符串和枚举值
 *   _has_arch      - 是否区分 CPU 架构（T/F）
 *   _installable   - 是否可直接安装（T/F），符号链接类型通常为 F
 *   _has_path      - .manifest 中是否携带自定义安装路径（T/F）
 *   _is_symlink    - 是否为符号链接文件（T/F）
 *   _is_shared_lib - 是否为共享库文件（T/F）
 *   _is_opengl     - 是否为 OpenGL/EGL/GLX 相关文件（T/F）
 *   _is_temporary  - 是否为临时文件（T/F），如签名密钥、DKMS 配置等
 *   _is_conflicting- 是否可能与系统中已有文件冲突（T/F）
 *   _inherit_path  - 是否从关联的主文件继承安装路径（T/F）
 */
#define ENTRY(_name,                                                    \
              _has_arch,                                                \
              _installable,                                             \
              _has_path,                                                \
              _is_symlink,                                              \
              _is_shared_lib,                                           \
              _is_opengl,                                               \
              _is_temporary,                                            \
              _is_conflicting,                                          \
              _inherit_path                                             \
             )                                                          \
    #_name , FILE_TYPE_ ## _name ,                                      \
        {                                                               \
            .has_arch       = _has_arch,                                \
            .installable    = _installable,                             \
            .has_path       = _has_path,                                \
            .is_symlink     = _is_symlink,                              \
            .is_shared_lib  = _is_shared_lib,                           \
            .is_opengl      = _is_opengl,                               \
            .is_temporary   = _is_temporary,                            \
            .is_conflicting = _is_conflicting,                          \
            .inherit_path   = _inherit_path,                            \
        }

/*
 * packageEntryFileTypeTable[] -- 文件类型定义表
 *
 * 定义了所有 FILE_TYPE_* 枚举值对应的属性。此表必须与 nvidia-installer.h 中的
 * PackageEntryFileType 枚举定义保持同步。每个条目包含：
 *   - name：类型名称字符串，用于在 .manifest 文件中匹配
 *   - type：对应的 PackageEntryFileType 枚举值
 *   - caps：该类型的能力标志（PackageEntryFileCapabilities）
 *
 * 注意：该表中条目的顺序不要求与枚举定义顺序一致，因为查找时使用线性扫描。
 * 但增删类型时需同步修改 nvidia-installer.h 中的枚举定义。
 */
static const struct {
    const char *name;                /* 文件类型名称字符串，如 "KERNEL_MODULE_SRC" */
    PackageEntryFileType type;       /* 文件类型枚举值，如 FILE_TYPE_KERNEL_MODULE_SRC */
    PackageEntryFileCapabilities caps; /* 该类型的能力标志集合 */
} packageEntryFileTypeTable[] = {

    /*
     * 下面的 ASCII 图示标注了每列标志位的含义，方便阅读各条目的 T/F 值：
     *
     * inherit_path   ------------------------------------------+  继承安装路径
     * is_conflicting ---------------------------------------+  |  可能冲突
     * is_temporary   ------------------------------------+  |  |  临时文件
     * is_opengl      ---------------------------------+  |  |  |  OpenGL 相关
     * is_shared_lib  ------------------------------+  |  |  |  |  共享库
     * is_symlink     ---------------------------+  |  |  |  |  |  符号链接
     * has_path       ------------------------+  |  |  |  |  |  |  有自定义路径
     * installable    ---------------------+  |  |  |  |  |  |  |  可安装
     * has_arch       ------------------+  |  |  |  |  |  |  |  |  区分架构
     *                                  |  |  |  |  |  |  |  |  |
     */

    /* === 内核相关文件 === */
    /* 内核模块源代码：不区分架构，可安装，冲突标记，路径从关联文件继承 */
    { ENTRY(KERNEL_MODULE_SRC,          F, T, F, F, F, F, F, T, T) },
    /* 编译后的内核模块（.ko 文件）：不区分架构，可安装，冲突标记 */
    { ENTRY(KERNEL_MODULE,              F, T, F, F, F, F, F, T, F) },

    /* === CUDA 相关文件 === */
    /* CUDA ICD（可安装客户端驱动）配置文件 */
    { ENTRY(CUDA_ICD,                   F, T, F, F, F, F, F, T, F) },

    /* === OpenGL 库和符号链接 === */
    /* OpenGL 共享库：区分架构，是共享库，是 OpenGL 组件 */
    { ENTRY(OPENGL_LIB,                 T, T, F, F, T, T, F, T, F) },

    /* === Wine 兼容层 === */
    /* Wine 兼容层库文件：区分架构，不冲突 */
    { ENTRY(WINE_LIB,                   T, T, F, F, F, F, F, F, F) },

    /* === CUDA 库和符号链接 === */
    /* CUDA 库文件：区分架构，有自定义路径，是共享库 */
    { ENTRY(CUDA_LIB,                   T, T, T, F, T, F, F, T, F) },

    /* === OpenCL 库和符号链接 === */
    /* OpenCL 库文件：区分架构，有自定义路径，是共享库 */
    { ENTRY(OPENCL_LIB,                 T, T, T, F, T, F, F, T, F) },
    /* OpenCL 包装器库：区分架构，有自定义路径，是共享库，不冲突 */
    { ENTRY(OPENCL_WRAPPER_LIB,         T, T, T, F, T, F, F, F, F) },
    /* OpenCL 库的符号链接：不可直接安装（installable=F），是符号链接 */
    { ENTRY(OPENCL_LIB_SYMLINK,         T, F, T, T, F, F, F, T, F) },
    /* OpenCL 包装器库的符号链接：不冲突 */
    { ENTRY(OPENCL_WRAPPER_SYMLINK,     T, F, T, T, F, F, F, F, F) },

    /* === TLS（线程本地存储）库 === */
    /* TLS 库：区分架构，有自定义路径，是共享库，是 OpenGL 组件 */
    { ENTRY(TLS_LIB,                    T, T, T, F, T, T, F, T, F) },

    /* === 工具库和符号链接 === */
    /* 工具程序依赖的共享库（如 libnvidia-ml.so） */
    { ENTRY(UTILITY_LIB,                T, T, F, F, T, F, F, T, F) },

    /* === GBM（Generic Buffer Management）后端库 === */
    /* GBM 后端库：用于 Wayland 等显示服务器的缓冲区管理 */
    { ENTRY(GBM_BACKEND_LIB,            T, T, F, F, T, F, F, T, F) },

    /* === 文档和配置文件 === */
    /* 文档文件：不区分架构，有自定义安装路径 */
    { ENTRY(DOCUMENTATION,              F, T, T, F, F, F, F, T, F) },
    /* 应用程序性能配置文件（Application Profile） */
    { ENTRY(APPLICATION_PROFILE,        F, T, T, F, F, F, F, T, F) },
    /* man 手册页 */
    { ENTRY(MANPAGE,                    F, T, T, F, F, F, F, T, F) },
    /* 使用显式路径安装的文件：直接指定了完整的安装目标路径 */
    { ENTRY(EXPLICIT_PATH,              F, T, T, F, F, F, F, T, F) },

    /* === 各类符号链接（不可直接安装，installable=F） === */
    /* OpenGL 库的符号链接 */
    { ENTRY(OPENGL_SYMLINK,             T, F, F, T, F, T, F, T, F) },
    /* CUDA 库的符号链接 */
    { ENTRY(CUDA_SYMLINK,               T, F, T, T, F, F, F, T, F) },
    /* 工具库的符号链接 */
    { ENTRY(UTILITY_LIB_SYMLINK,        T, F, F, T, F, F, F, T, F) },
    /* GBM 后端库的符号链接 */
    { ENTRY(GBM_BACKEND_LIB_SYMLINK,    T, F, F, T, F, F, F, T, F) },

    /* === 二进制可执行文件 === */
    /* 安装器自身的二进制文件 */
    { ENTRY(INSTALLER_BINARY,           F, T, F, F, F, F, F, T, F) },
    /* 工具程序（如 nvidia-smi、nvidia-settings 等） */
    { ENTRY(UTILITY_BINARY,             F, T, F, F, F, F, F, T, F) },
    /* 工具程序的符号链接 */
    { ENTRY(UTILITY_BIN_SYMLINK,        F, F, F, T, F, F, F, T, F) },

    /* === 桌面集成文件 === */
    /* .desktop 桌面入口文件：是临时文件（安装后可能被桌面环境缓存替代） */
    { ENTRY(DOT_DESKTOP,                F, T, T, F, F, F, T, T, F) },
    /* 图标文件 */
    { ENTRY(ICON,                       F, T, T, F, F, F, F, T, F) },

    /* === X.Org 模块 === */
    /* X.Org 模块共享库（如输入/输出驱动模块） */
    { ENTRY(XMODULE_SHARED_LIB,         F, T, T, F, T, F, F, T, F) },
    /* GLX 模块共享库（libglx.so，X.Org 的 GLX 扩展模块） */
    { ENTRY(GLX_MODULE_SHARED_LIB,      F, T, T, F, T, T, F, T, F) },
    /* GLX 模块的符号链接 */
    { ENTRY(GLX_MODULE_SYMLINK,         F, F, T, T, F, T, F, T, F) },

    /* === VDPAU（视频解码与后处理加速 API）=== */
    /* VDPAU 库文件 */
    { ENTRY(VDPAU_LIB,                  T, T, T, F, T, F, F, T, F) },
    /* VDPAU 库的符号链接 */
    { ENTRY(VDPAU_SYMLINK,              T, F, T, T, F, F, F, T, F) },

    /* === NVCUVID（NVIDIA CUDA 视频解码器）=== */
    /* NVCUVID 库文件 */
    { ENTRY(NVCUVID_LIB,                T, T, F, F, T, F, F, T, F) },
    /* NVCUVID 库的符号链接 */
    { ENTRY(NVCUVID_LIB_SYMLINK,        T, F, F, T, F, F, F, T, F) },

    /* === NVENC（视频编码 API）=== */
    /* 视频编码 API 库 */
    { ENTRY(ENCODEAPI_LIB,              T, T, F, F, T, F, F, T, F) },
    /* 视频编码 API 库的符号链接 */
    { ENTRY(ENCODEAPI_LIB_SYMLINK,      T, F, F, T, F, F, F, T, F) },

    /* === VGX（虚拟 GPU 技术）=== */
    /* VGX 库文件：不区分架构 */
    { ENTRY(VGX_LIB,                    F, T, F, F, T, F, F, T, F) },
    /* VGX 库的符号链接 */
    { ENTRY(VGX_LIB_SYMLINK,            F, F, F, T, F, F, F, T, F) },

    /* === GRID（企业级虚拟化）=== */
    /* GRID 库文件 */
    { ENTRY(GRID_LIB,                   F, T, T, F, T, F, F, T, F) },
    /* GRID 库的符号链接 */
    { ENTRY(GRID_LIB_SYMLINK,           F, F, T, T, F, F, F, T, F) },

    /* === nvidia-modprobe 工具 === */
    /* nvidia-modprobe：用于自动创建 /dev/nvidia* 设备节点并加载内核模块 */
    { ENTRY(NVIDIA_MODPROBE,            F, T, T, F, F, F, F, T, F) },
    /* nvidia-modprobe 的 man 手册页 */
    { ENTRY(NVIDIA_MODPROBE_MANPAGE,    F, T, T, F, F, F, F, T, F) },

    /* === 内核模块签名 === */
    /* 内核模块签名密钥文件：临时文件（仅在签名阶段使用） */
    { ENTRY(MODULE_SIGNING_KEY,         F, T, F, F, F, F, T, T, F) },

    /* === Xorg 配置 === */
    /* Xorg OutputClass 配置文件：用于 xorg.conf.d 目录下的自动配置 */
    { ENTRY(XORG_OUTPUTCLASS_CONFIG,    F, T, F, F, F, F, F, T, F) },

    /* === DKMS（Dynamic Kernel Module Support）=== */
    /* DKMS 配置文件（dkms.conf）：临时文件，路径从关联文件继承 */
    { ENTRY(DKMS_CONF,                  F, T, F, F, F, F, T, T, T) },

    /* === GLVND（GL Vendor-Neutral Dispatch）=== */
    /* GLVND 库：实现 OpenGL 厂商中立调度，是共享库且属于 OpenGL 组件 */
    { ENTRY(GLVND_LIB,                  T, T, F, F, T, T, F, T, F) },
    /* GLVND 库的符号链接 */
    { ENTRY(GLVND_SYMLINK,              T, F, F, T, F, T, F, T, F) },

    /* === GLX 客户端库 === */
    /* GLX 客户端库：GLX 协议的客户端实现，属于 OpenGL 组件 */
    { ENTRY(GLX_CLIENT_LIB,             T, T, F, F, T, T, F, T, F) },
    /* GLX 客户端库的符号链接 */
    { ENTRY(GLX_CLIENT_SYMLINK,         T, F, F, T, F, T, F, T, F) },

    /* === Vulkan 相关配置 === */
    /* Vulkan ICD JSON 配置文件：描述 Vulkan 可安装客户端驱动的位置 */
    { ENTRY(VULKAN_ICD_JSON,            F, T, T, F, F, F, F, T, F) },

    /* === EGL 相关文件 === */
    /* GLVND EGL ICD JSON 配置：描述 EGL 厂商驱动的位置 */
    { ENTRY(GLVND_EGL_ICD_JSON,         F, T, F, F, F, T, F, T, F) },
    /* EGL 客户端库：EGL API 的实现，属于 OpenGL 组件 */
    { ENTRY(EGL_CLIENT_LIB,             T, T, F, F, T, T, F, T, F) },
    /* EGL 客户端库的符号链接 */
    { ENTRY(EGL_CLIENT_SYMLINK,         T, F, F, T, F, T, F, T, F) },
    /* EGL 外部平台 JSON 配置：如 Wayland EGL 平台支持 */
    { ENTRY(EGL_EXTERNAL_PLATFORM_JSON, F, T, F, F, F, T, F, T, F) },

    /* === Flexera 许可证管理 === */
    /* Flexera 许可证管理库：不冲突（可能为可选的企业功能） */
    { ENTRY(FLEXERA_LIB,                F, T, T, F, T, F, F, F, F) },
    /* Flexera 库的符号链接 */
    { ENTRY(FLEXERA_LIB_SYMLINK,        F, F, T, T, F, F, F, F, F) },

    /* === 内部工具 === */
    /* 内部工具程序：区分架构，不对外暴露 */
    { ENTRY(INTERNAL_UTILITY_BINARY,    T, T, F, F, F, F, F, T, F) },
    /* 内部工具库 */
    { ENTRY(INTERNAL_UTILITY_LIB,       T, T, F, F, T, F, F, T, F) },
    /* 内部工具数据文件 */
    { ENTRY(INTERNAL_UTILITY_DATA,      F, T, F, F, F, F, F, T, F) },

    /* === 固件文件 === */
    /* GPU 固件文件：不冲突，路径从关联文件继承 */
    { ENTRY(FIRMWARE,                   F, T, F, F, F, F, F, F, T) },

    /* === systemd 服务管理 === */
    /* systemd 服务单元文件（.service 等） */
    { ENTRY(SYSTEMD_UNIT,               F, T, F, F, F, F, F, F, F) },
    /* systemd 服务单元的符号链接：有自定义路径 */
    { ENTRY(SYSTEMD_UNIT_SYMLINK,       F, F, T, T, F, F, F, F, F) },
    /* systemd 睡眠/唤醒钩子脚本 */
    { ENTRY(SYSTEMD_SLEEP_SCRIPT,       F, T, F, F, F, F, F, F, F) },

    /* === OpenGL 数据文件 === */
    /* OpenGL 数据文件（非库文件，如着色器缓存或配置数据） */
    { ENTRY(OPENGL_DATA,                F, T, T, F, F, T, F, T, F) },

    /* === Vulkan SC（Safety Critical）=== */
    /* Vulkan SC ICD JSON 配置：用于安全关键场景的 Vulkan 驱动配置 */
    { ENTRY(VULKANSC_ICD_JSON,          F, T, T, F, F, F, F, T, F) },

    /* === 沙箱工具 === */
    /* 沙箱工具文件列表 JSON：描述沙箱环境中需要的文件，不冲突 */
    { ENTRY(SANDBOXUTILS_FILELIST_JSON, F, T, T, F, F, F, F, F, F) },
};

/*
 * get_file_type_capabilities() -- 根据文件类型枚举值获取其能力标志
 *
 * 功能：在 packageEntryFileTypeTable[] 中线性查找给定的文件类型枚举值，
 *       如果找到则返回该类型对应的能力标志（PackageEntryFileCapabilities）。
 *
 * 参数：
 *   type - 要查询的文件类型枚举值（如 FILE_TYPE_OPENGL_LIB）
 *
 * 返回值：
 *   - 找到时：返回该类型的 PackageEntryFileCapabilities 结构体
 *   - 未找到时：返回全 FALSE 的空能力标志（nullCaps），表示该类型无任何能力
 *
 * 处理流程：
 *   1. 初始化一个全为 FALSE 的空能力标志结构体作为默认返回值
 *   2. 遍历 packageEntryFileTypeTable[] 表中所有条目
 *   3. 将传入的 type 与每个条目的 type 字段比较
 *   4. 匹配成功则立即返回该条目的 caps（能力标志）
 *   5. 遍历完毕未匹配则返回空能力标志
 *
 * 注意：如果 type 不在表中（例如 FILE_TYPE_NONE 或无效值），将返回全 FALSE
 * 的能力标志，调用者可据此判断类型是否有效。
 */
PackageEntryFileCapabilities get_file_type_capabilities(
    PackageEntryFileType type
)
{
    int i;
    /* 初始化全 FALSE 的空能力标志，作为未找到时的默认返回值 */
    PackageEntryFileCapabilities nullCaps = { F, F, F, F, F, F, F, F, F };

    /* 线性扫描文件类型定义表，ARRAY_LEN 宏计算数组元素个数 */
    for (i = 0; i < ARRAY_LEN(packageEntryFileTypeTable); i++) {
        if (type == packageEntryFileTypeTable[i].type) {
            return packageEntryFileTypeTable[i].caps;
        }
    }

    /* 未找到匹配的类型，返回空能力标志 */
    return nullCaps;
}

/*
 * parse_manifest_file_type() -- 将 .manifest 中的文件类型字符串解析为枚举值
 *
 * 功能：在 packageEntryFileTypeTable[] 中查找与给定字符串匹配的类型名称，
 *       如果找到则返回对应的枚举值并通过输出参数返回能力标志。
 *
 * 参数：
 *   str  - .manifest 文件中的文件类型字符串（如 "OPENGL_LIB"、"CUDA_SYMLINK" 等）
 *   caps - [输出参数] 指向 PackageEntryFileCapabilities 的指针，
 *          找到匹配时将被设置为该类型的能力标志；未找到时不修改。
 *
 * 返回值：
 *   - 找到时：返回对应的 PackageEntryFileType 枚举值
 *   - 未找到时：返回 FILE_TYPE_NONE（值为 0），表示无法识别的类型
 *
 * 处理流程：
 *   1. 遍历 packageEntryFileTypeTable[] 表中所有条目
 *   2. 使用 strcmp() 将传入字符串与每个条目的 name 字段进行精确比较
 *   3. 匹配成功时：将该条目的能力标志写入 *caps，并返回对应的枚举值
 *   4. 遍历完毕未匹配：返回 FILE_TYPE_NONE
 *
 * 使用场景：解析 .manifest 文件时，每行包含文件类型字符串，调用此函数
 * 将字符串转换为内部枚举值以便后续处理。
 */
PackageEntryFileType parse_manifest_file_type(
    const char *str,
    PackageEntryFileCapabilities *caps
)
{
    int i;

    /* 线性扫描表，使用字符串精确匹配 */
    for (i = 0; i < ARRAY_LEN(packageEntryFileTypeTable); i++) {
        if (strcmp(str, packageEntryFileTypeTable[i].name) == 0) {
            /* 匹配成功：通过输出参数返回能力标志，并返回枚举值 */
            *caps = packageEntryFileTypeTable[i].caps;
            return packageEntryFileTypeTable[i].type;
        }
    }

    /* 未匹配任何已知类型，返回 FILE_TYPE_NONE */
    return FILE_TYPE_NONE;
}

/*
 * get_installable_file_type_list() -- 根据用户选项生成可安装文件类型列表
 *
 * 功能：遍历 packageEntryFileTypeTable[]，根据当前安装选项（Options）过滤出
 *       需要安装的文件类型，结果以位图形式存入 installable_file_types。
 *
 * 参数：
 *   op                    - 指向 Options 结构体的指针，包含用户通过命令行或
 *                           交互界面设置的所有安装选项
 *   installable_file_types - [输出参数] 指向 PackageEntryFileTypeList 的指针，
 *                            函数将可安装的文件类型标记在 types[] 数组中
 *                            （types[FILE_TYPE_XXX] = 1 表示该类型需要安装）
 *
 * 处理流程：
 *   1. 将输出列表清零（memset），确保初始状态为"全部不安装"
 *   2. 遍历文件类型定义表中的每个条目，依次进行以下过滤：
 *
 *      a. 如果用户指定了 --no-kernel-module-source 选项，则跳过
 *         KERNEL_MODULE_SRC 和 DKMS_CONF 类型（不安装内核模块源码及 DKMS 配置）
 *
 *      b. 如果模块已通过 DKMS 注册（dkms_registered 为真），则跳过
 *         KERNEL_MODULE 类型（无需再安装编译后的内核模块）
 *
 *      c. 如果用户未指定 --nvidia-modprobe 选项，则跳过
 *         NVIDIA_MODPROBE 和 NVIDIA_MODPROBE_MANPAGE 类型
 *
 *      d. 如果该文件类型的 installable 标志为 FALSE，则跳过
 *         （符号链接等不可直接安装的类型在此被过滤）
 *
 *      e. 如果 Xorg 不支持 OutputClass 配置，则跳过
 *         XORG_OUTPUTCLASS_CONFIG 类型
 *
 *   3. 通过以上全部检查的类型，在输出列表中标记为可安装（types[type] = 1）
 *
 * 注意：此函数决定了哪些文件会被实际安装到系统中，是安装流程的关键过滤步骤。
 * 符号链接类型（installable=F）不会被此函数加入列表，它们由
 * add_symlinks_to_file_type_list() 单独处理。
 */
void get_installable_file_type_list(
    Options *op,
    PackageEntryFileTypeList *installable_file_types
)
{
    int i;

    /* 将输出列表全部清零，初始状态为"所有类型均不安装" */
    memset(installable_file_types, 0, sizeof(*installable_file_types));

    for (i = 0; i < ARRAY_LEN(packageEntryFileTypeTable); i++) {

        PackageEntryFileType type = packageEntryFileTypeTable[i].type;

        /*
         * 过滤条件 1：用户选择不安装内核模块源代码时，
         * 跳过内核模块源码和 DKMS 配置文件。
         * DKMS 依赖内核模块源码进行自动编译，因此一并跳过。
         */
        if (((type == FILE_TYPE_KERNEL_MODULE_SRC) ||
             (type == FILE_TYPE_DKMS_CONF)) &&
            op->no_kernel_module_source) {
            continue;
        }

        /*
         * 过滤条件 2：如果模块已通过 DKMS 注册，则无需安装预编译的内核模块，
         * 因为 DKMS 会在需要时自动从源码编译。
         */
        if (type == FILE_TYPE_KERNEL_MODULE && op->dkms_registered) {
            continue;
        }

        /*
         * 过滤条件 3：nvidia-modprobe 是可选组件，仅在用户明确指定
         * --nvidia-modprobe 选项时才安装。nvidia-modprobe 用于在非 root
         * 环境下自动创建 /dev/nvidia* 设备节点。
         */
        if (((type == FILE_TYPE_NVIDIA_MODPROBE) ||
             (type == FILE_TYPE_NVIDIA_MODPROBE_MANPAGE)) &&
            !op->nvidia_modprobe) {
            continue;
        }

        /*
         * 过滤条件 4：跳过 installable 标志为 FALSE 的类型。
         * 符号链接类型（如 OPENGL_SYMLINK、CUDA_SYMLINK 等）的 installable
         * 均为 FALSE，它们不通过此函数安装，而是由专门的符号链接创建逻辑处理。
         */
        if (!packageEntryFileTypeTable[i].caps.installable) {
            continue;
        }

        /*
         * 过滤条件 5：如果当前 Xorg 版本不支持 OutputClass 配置机制
         *（即不支持 xorg.conf.d 目录下的自动配置），则跳过该配置文件。
         * OutputClass 是较新版本 Xorg 引入的特性。
         */
        if ((type == FILE_TYPE_XORG_OUTPUTCLASS_CONFIG) &&
            !op->xorg_supports_output_class) {
            continue;
        }

        /* 通过所有过滤条件，将该类型标记为可安装 */
        installable_file_types->types[type] = 1;
    }
}

/*
 * add_symlinks_to_file_type_list() -- 将所有符号链接类型添加到文件类型列表中
 *
 * 功能：遍历 packageEntryFileTypeTable[]，将所有 is_symlink 标志为 TRUE 的
 *       文件类型添加到给定的文件类型列表中。
 *
 * 参数：
 *   file_type_list - [输入/输出参数] 指向 PackageEntryFileTypeList 的指针。
 *                    函数会在现有列表基础上追加符号链接类型，不会清除已有条目。
 *
 * 使用场景：
 *   在构建需要删除的已存在文件列表时使用。安装新版本驱动前，需要删除旧版本
 *   的所有文件，包括实际文件和符号链接。此函数确保符号链接类型也被包含在
 *   删除列表中，以避免残留的旧版本符号链接导致版本混淆或加载错误。
 *
 * 处理流程：
 *   1. 遍历文件类型定义表中的每个条目
 *   2. 检查该条目的 is_symlink 能力标志
 *   3. 如果不是符号链接类型则跳过
 *   4. 如果是符号链接类型，则在列表中将对应位置标记为 1
 *
 * 注意：此函数不会清除列表中已有的标记，仅追加。通常在调用
 * get_installable_file_type_list() 之后调用，以补充符号链接类型。
 */
void add_symlinks_to_file_type_list(PackageEntryFileTypeList *file_type_list)
{
    int i;

    for (i = 0; i < ARRAY_LEN(packageEntryFileTypeTable); i++) {

        PackageEntryFileType type = packageEntryFileTypeTable[i].type;

        /* 跳过非符号链接类型 */
        if (!packageEntryFileTypeTable[i].caps.is_symlink) {
            continue;
        }

        /* 将符号链接类型标记到列表中 */
        file_type_list->types[type] = 1;
    }
}

/*
 * remove_file_type_from_file_type_list() -- 从文件类型列表中移除指定类型
 *
 * 功能：将文件类型列表中指定类型的标记清除为 0（即标记为"不需要"）。
 *
 * 参数：
 *   list - [输入/输出参数] 指向 PackageEntryFileTypeList 的指针，
 *          函数将修改其中指定类型的标记位
 *   type - 要移除的文件类型枚举值
 *
 * 使用场景：
 *   当某些条件使特定文件类型不再需要安装或删除时，调用此函数将其从列表中移除。
 *   例如，在安装过程中动态决定跳过某种文件类型。
 *
 * 注意：此函数直接使用枚举值作为数组下标访问 types[]，因此 type 的值必须在
 * [0, FILE_TYPE_MAX) 范围内，否则会导致数组越界。调用者需确保 type 的有效性。
 */
void remove_file_type_from_file_type_list(PackageEntryFileTypeList *list,
                                          PackageEntryFileType type)
{
    /* 将指定类型的标记清零，表示该类型不在列表中 */
    list->types[type] = 0;
}
