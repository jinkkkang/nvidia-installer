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
 * 【文件说明】杂项工具函数头文件。
 * 声明了安装器中大量通用工具函数，包括：
 *   - 命令执行（run_command）
 *   - 系统工具查找（find_system_utils、find_module_utils）
 *   - 系统环境检测（SELinux、X Server、nouveau、Secure Boot 等）
 *   - PCI 设备扫描
 *   - DKMS 管理
 *   - CRC 校验
 *   - 发行版钩子脚本执行
 *   - 各种辅助判断和字符串处理
 *
 * misc.h
 */

#ifndef __NVIDIA_INSTALLER_MISC_H__
#define __NVIDIA_INSTALLER_MISC_H__

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "nvidia-installer.h"
#include "command-list.h"
#include "user-interface.h"

/*
 * 发行版钩子脚本执行状态枚举。
 * 安装/卸载过程中可运行发行版特定的钩子脚本（如 pre-install、post-install）。
 */

typedef enum {
    HOOK_SCRIPT_FAIL = 0,    /* 钩子脚本执行失败 */
    HOOK_SCRIPT_SUCCESS,     /* 钩子脚本执行成功 */
    HOOK_SCRIPT_NO_RUN,      /* 钩子脚本未执行（脚本不存在或被禁用） */
} HookScriptStatus;

/*
 * ELF 文件架构枚举。
 * 用于检测二进制文件/共享库是 32 位还是 64 位。
 */
typedef enum {
    ELF_INVALID_FILE,          /* 无效文件（非 ELF 格式） */
    ELF_ARCHITECTURE_UNKNOWN,  /* 未知架构 */
    ELF_ARCHITECTURE_32,       /* 32 位 ELF */
    ELF_ARCHITECTURE_64,       /* 64 位 ELF */
} ElfFileType;

/*
 * RunCommandOutputMatch：命令输出匹配规则。
 * 传递给 run_command() 的 { 0 } 结尾数组，
 * 用于在命令执行时根据输出内容更新进度条。
 */
typedef struct {
    int lines;              /* 预期匹配的输出行数 */
    char *initial_match;    /* 行首匹配字符串（NULL 表示匹配所有行） */
} RunCommandOutputMatch;

/* ===== 文本解析工具 ===== */

/* 从缓冲区读取下一个空白分隔的单词 */
char *read_next_word (char *buf, char **e);

/* ===== 系统环境检查 ===== */

/* 检查是否以 root 权限运行（EUID == 0） */
int check_euid(Options *op);
/* 调整当前工作目录到安装包所在目录 */
int adjust_cwd(Options *op, const char *program_name);
/* 从缓冲区获取下一行文本 */
char *get_next_line(char *buf, char **e, char *start, int length);
/* 执行外部命令，支持输出捕获、进度匹配、重定向（参数列表以 NULL 结尾） */
__attribute__((sentinel))
int run_command(Options *op, char **data, int output,
                const RunCommandOutputMatch *match, int redirect,
                const char *cmd_start, ...);
/* 读取文本文件全部内容到缓冲区 */
int read_text_file(const char *filename, char **buf);
/* 在 PATH 中查找指定系统工具的完整路径 */
char *find_system_util(const char *util);
/* 查找所有必需的系统工具 */
int find_system_utils(Options *op);
/* 查找所有内核模块管理工具 */
int find_module_utils(Options *op);
/* 检查 SELinux 状态并设置相关选项 */
int check_selinux(Options *op);
/* 检查 /proc/sys/kernel/modprobe 路径是否有效 */
int check_proc_modprobe_path(Options *op);
/* 检查编译内核模块所需的开发工具是否存在 */
int check_development_tools(Options *op, Package *p);
/* 检查处理预编译内核接口所需的工具是否存在 */
int check_precompiled_kernel_interface_tools(Options *op);
/* 从字符串中提取版本号 */
char *extract_version_string(const char *str);
/* 询问用户是否在错误发生后继续操作 */
int continue_after_error(Options *op, const char *fmt, ...) NV_ATTRIBUTE_PRINTF(2, 3);
/* 执行安装操作（构建命令列表并执行） */
int do_install(Options *op, Package *p, CommandList *c);
/* 决定是否安装 32 位兼容库 */
void should_install_compat32_files(Options *op, Package *p);
/* 决定是否安装可选内核模块（UVM、DRM、peermem） */
void should_install_optional_modules(Options *op, Package *p,
                                     const KernelModuleInfo *optional_modules,
                                     int num_optional_modules);
/* 检查已安装包中文件的完整性 */
void check_installed_files_from_package(Options *op, Package *p);
/* 检查单个已安装文件的权限和 CRC */
int check_installed_file(Options*, const char*, const mode_t, const uint32,
                         ui_message_func *logwarn);
/* 检查 X Server 是否正在运行 */
int check_for_running_x(Options *op);
/* 查询已安装 Xorg 的版本信息 */
void query_xorg_version(Options *op);
/* 扫描 PCI 总线上的 NVIDIA GPU 设备 */
void pci_device_scan(Options *op);
/* 检查是否存在 NVIDIA 显卡（并判断是否为 legacy 系列） */
void check_for_nvidia_graphics_devices(Options *op, Package *p);
/* 运行 nvidia-xconfig 配置 X Server */
int run_nvidia_xconfig(Options *op, int restore, const char *question, int answer);
/* 运行发行版特定的钩子脚本 */
HookScriptStatus run_distro_hook(Options *op, const char *hook);
/* 检查是否存在通过其他方式安装的 NVIDIA 驱动（如包管理器） */
int check_for_alternate_install(Options *op);
/* 检查 nouveau 开源驱动是否已加载 */
int check_for_nouveau(Options *op);
/* 检查指定内核的 DKMS 模块是否已安装 */
int dkms_module_installed(Options *op, const char *module, const char *kernel);
/* 将内核模块注册到 DKMS */
void dkms_register_module(Options *op, Package *p, const char *kernel);
/* 从 DKMS 移除指定版本的模块 */
int dkms_remove_module(Options *op, const char *version);
/* 验证文件的 CRC32 校验和 */
int verify_crc(Options *op, const char *filename, unsigned int crc,
               unsigned int *actual_crc);
/* 检测系统是否启用了 Secure Boot */
int secure_boot_enabled(void);
/* 获取 ELF 文件的架构类型（32 位或 64 位） */
ElfFileType get_elf_architecture(const char *filename);
/* 设置编译并发级别（make -j） */
void set_concurrency_level(Options *op);
/* 通过 pkg-config 获取指定包的变量值 */
char *get_pkg_config_variable(Options *op,
                              const char *pkg, const char *variable);
/* 检查系统是否使用 systemd */
int check_systemd(Options *op);
/* 检查指定命令是否支持某个选项 */
int option_is_supported(Options *op, const char *cmd, const char *help,
                        const char *option);
/* 检查系统中是否安装了 Vulkan 加载器 */
void check_for_vulkan_loader(Options *op);
/* 向带项目符号的列表字符串追加新条目 */
void add_bullet_list_item(const char *new, char **orig);
/* 建议用户重启系统 */
void suggest_reboot(Options *op);
/* 检查 nouveau 内核模块是否当前已加载 */
int nouveau_is_present(void);

#endif /* __NVIDIA_INSTALLER_MISC_H__ */
