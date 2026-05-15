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
 * 【文件说明】内核模块管理头文件。
 * 声明了内核模块编译、安装、签名、加载、测试等所有内核相关操作的函数。
 * 这是安装器中最核心的子系统之一，负责：
 *   - 确定内核源码路径和模块安装路径
 *   - 编译内核模块（或查找/使用预编译接口）
 *   - 对内核模块进行签名（Secure Boot 支持）
 *   - 加载和测试内核模块
 */

#ifndef __NVIDIA_INSTALLER_KERNEL_H__
#define __NVIDIA_INSTALLER_KERNEL_H__

#include "nvidia-installer.h"
#include "precompiled.h"

/*
 * 内核配置选项状态枚举。
 * 用于检查内核 .config 中某个选项（如 CONFIG_MODULE_SIG）的状态。
 */
typedef enum {
    KERNEL_CONFIG_OPTION_NOT_DEFINED = 0, /* 选项未定义（未设置） */
    KERNEL_CONFIG_OPTION_DEFINED,         /* 选项已定义（=y 或 =m） */
    KERNEL_CONFIG_OPTION_UNKNOWN          /* 无法确定（如 .config 文件不存在） */
} KernelConfigOptionStatus;

/*
 * 内核模块类型枚举。
 * 枚举值对应首字母，用于 --kernel-module-type 命令行选项匹配。
 * NVIDIA 驱动可以选择使用闭源（proprietary）或开源（open）内核模块。
 */
typedef enum {
    PROPRIETARY = 'p',  /* 闭源/专有内核模块 */
    OPEN = 'o',         /* 开源内核模块（kernel-open） */
} KernelModuleType;
#define NUM_KERNEL_MODULE_TYPES 2  /* 模块类型总数 */

/*
 * 模块类型信息结构。
 * 存储可用的内核模块类型（闭源/开源）及其对应的目录和许可证信息。
 */
struct module_type_info {
    int default_entry;                          /* 默认选择的模块类型索引 */
    char types[NUM_KERNEL_MODULE_TYPES];        /* 模块类型字符数组（'p'/'o'） */
    const char *dirs[NUM_KERNEL_MODULE_TYPES];  /* 各类型模块的源码目录 */
    const char *licenses[NUM_KERNEL_MODULE_TYPES]; /* 各类型模块的许可证名称 */
};

/* ===== 内核模块管理函数声明 ===== */

/* 确定内核模块安装路径（如 /lib/modules/<kernel>/kernel/drivers/video） */
int determine_kernel_module_installation_path      (Options*);
/* 确定内核源码路径（检查 /lib/modules/<kernel>/build 等） */
int determine_kernel_source_path                   (Options*, Package*);
/* 确定内核构建输出路径（对于 out-of-tree 编译的内核） */
int determine_kernel_output_path                   (Options*);
/* 解压预编译的内核模块到指定目录 */
int unpack_kernel_modules                          (Options*, Package*,
                                                    const char *,
                                                    const PrecompiledFileInfo *);
/* 从源码编译内核模块（make modules） */
int build_kernel_modules                           (Options*, Package*);
/* 编译内核接口文件，用于生成预编译接口包 */
int build_kernel_interfaces                        (Options*, Package*,
                                                    PrecompiledFileInfo **);
/* 测试编译好的内核模块（尝试 insmod 加载） */
int test_kernel_modules                            (Options*, Package*);
/* 加载指定的内核模块（modprobe） */
int load_kernel_module                             (Options*, const char*);
/* 检查 NVIDIA 内核模块是否未加载（用于安装后验证） */
int check_for_unloaded_kernel_module               (Options*);
/* 查找与当前内核匹配的预编译内核接口 */
PrecompiledInfo *find_precompiled_kernel_interface (Options*, Package*);
/* 获取目标内核版本名称（uname -r 或 --kernel-name 参数） */
char *get_kernel_name                              (Options*);
/* 获取当前机器架构（如 x86_64、aarch64） */
const char *get_machine_arch                       (Options*);
/* 测试内核配置中某个选项是否已定义 */
KernelConfigOptionStatus test_kernel_config_option (Options*, Package*,
                                                    const char*);
/* 对内核模块进行数字签名（Secure Boot 支持） */
int sign_kernel_module                             (Options*, const char*,
                                                    const char*, int);
/* 猜测内核模块签名使用的哈希算法 */
char *guess_module_signing_hash                    (Options*, const char*);
/* 从包中移除指定的内核模块 */
int remove_kernel_module_from_package              (Package*, const char*);
/* 释放 KernelModuleInfo 结构体中的动态内存 */
void free_kernel_module_info                       (KernelModuleInfo);
/* 检查包中是否包含指定名称的内核模块 */
int package_includes_kernel_module                 (const Package*,
                                                    const char *);
/* 卸载已加载的内核模块（rmmod） */
int rmmod_kernel_module                            (Options*, const char *);
/* 运行 conftest.sh 进行内核配置兼容性检查 */
int conftest_sanity_check                          (Options*, const char *,
                                                    const char *, const char *);
/* 获取预编译内核接口文件的搜索路径 */
char *precompiled_kernel_interface_path            (const Package*);
/* 确定可用的内核模块类型（闭源/开源）及默认选择 */
int valid_kernel_module_types                      (Options*,
                                                    struct module_type_info*,
                                                    int allow_missing_directory);
/* 覆盖内核模块构建目录 */
int override_kernel_module_build_directory         (Options*, const char*);
/* 覆盖内核模块类型选择 */
int override_kernel_module_type                    (Options*, const char*);

/* 某些旧版内核头文件可能未定义 ENOKEY 错误码 */
#ifndef ENOKEY
#define	ENOKEY		126	/* 所需密钥不可用（内核模块签名验证失败时返回） */
#endif

#endif /* __NVIDIA_INSTALLER_KERNEL_H__ */
