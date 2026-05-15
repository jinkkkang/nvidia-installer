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
 * 【文件说明】文件操作子系统头文件。
 * 声明了安装器中所有与文件系统操作相关的函数，包括：
 *   - 文件/目录的创建、复制、删除、重命名
 *   - 安装目标路径计算（set_destinations、get_prefixes）
 *   - 包条目过滤（移除不需要的文件类型）
 *   - 权限处理、符号链接管理
 *   - 模板文件处理（如 .desktop 文件、dkms.conf）
 *   - SELinux 安全上下文设置
 *   - 预编译接口打包
 */

#ifndef __NVIDIA_INSTALLER_FILES_H__
#define __NVIDIA_INSTALLER_FILES_H__

#include "nvidia-installer.h"
#include "precompiled.h"

/* ===== 目录操作 ===== */

/* 递归删除目录及其内容 */
int remove_directory(Options *op, const char *victim);
/* 创建目录（如果不存在）或更新其时间戳 */
int touch_directory(Options *op, const char *victim);

/* ===== 文件操作 ===== */

/* 复制文件并设置权限 */
int copy_file(Options *op, const char *srcfile,
              const char *dstfile, mode_t mode);
/* 将数据写入临时文件，返回临时文件路径 */
char *write_temp_file(Options *op, const int len,
                      const void *data, mode_t perm);

/* ===== 安装路径和目标计算 ===== */

/* 为所有包条目计算安装目标绝对路径 */
int set_destinations(Options *op, Package *p);
/* 获取各类文件的安装前缀路径 */
int get_prefixes(Options *op);

/* ===== 包条目管理 ===== */

/* 将编译好的内核模块添加到包的条目列表中 */
void add_kernel_modules_to_package(Options *op, Package *p);
/* 从包中移除所有非内核模块文件（--kernel-modules-only 模式） */
void remove_non_kernel_module_files_from_package(Package *p);
/* 从包中移除未安装的内核模块源文件 */
void remove_non_installed_kernel_module_source_files_from_package(Package *p);
/* 从包中移除所有 OpenGL 文件（--no-opengl-files） */
void remove_opengl_files_from_package(Package *p);
/* 从包中移除所有 Wine 文件（--no-wine-files） */
void remove_wine_files_from_package(Package *p);
/* 从包中移除所有 systemd 文件 */
void remove_systemd_files_from_package(Package *p);

/* ===== 权限处理 ===== */

/* 将权限字符串（如 "0755"）转换为 mode_t */
int mode_string_to_mode(Options *op, char *s, mode_t *mode);
/* 将 mode_t 转换为人类可读的权限字符串（如 "rwxr-xr-x"） */
char *mode_to_permission_string(mode_t mode);

/* ===== 路径和目录工具 ===== */

/* 确认路径存在或询问用户是否创建 */
int confirm_path(Options *op, const char *path);
/* 递归创建目录 */
int mkdir_recursive(Options *op, const char *path, const mode_t mode, int log);
/* 创建目录并记录到备份日志 */
int mkdir_with_log(Options *op, const char *path, const mode_t mode);

/* ===== 符号链接操作 ===== */

/* 获取符号链接的目标路径（readlink） */
char *get_symlink_target(Options *op, const char *filename);
/* 获取符号链接的解析后目标路径（realpath） */
char *get_resolved_symlink_target(Options *op, const char *filename);

/* ===== 安装操作 ===== */

/* 安装文件：复制到目标位置并设置权限 */
int install_file(Options *op, const char *srcfile,
                 const char *dstfile, mode_t mode);
/* 安装符号链接：创建指向目标的符号链接 */
int install_symlink(Options *op, const char *linkname, const char *dstfile);

/* ===== 文件大小查询 ===== */

/* 通过文件名获取文件大小 */
size_t get_file_size(Options *op, const char *filename);
/* 通过文件描述符获取文件大小 */
size_t fget_file_size(Options *op, const int fd);

/* ===== 临时目录管理 ===== */

/* 获取临时目录路径 */
char *get_tmpdir(Options *op);
/* 创建临时目录并返回路径 */
char *make_tmpdir(Options *op);

/* ===== 其他文件工具 ===== */

/* 重命名文件（跨设备时使用复制+删除） */
int nvrename(Options *op, const char *src, const char *dst);
/* 检查系统中是否存在冲突的 RPM 包 */
int check_for_existing_rpms(Options *op);
/* 复制目录内容到另一个目录 */
int copy_directory_contents(Options *op, const char *src, const char *dst);
/* 将预编译文件打包为预编译接口包 */
int pack_precompiled_files(Options *op, Package *p, int num_files,
                           PrecompiledFileInfo *files);

/* ===== 模板和配置文件处理 ===== */

/* 处理模板文件（替换占位符令牌） */
char *process_template_file(Options *op, PackageEntry *pe,
                            char **tokens, char **replacements);
/* 处理 .desktop 桌面入口文件的令牌替换 */
void process_dot_desktop_files(Options *op, Package *p);
/* 处理 dkms.conf 配置文件的令牌替换 */
void process_dkms_conf(Options *op, Package *p);

/* ===== SELinux 和安全 ===== */

/* 设置文件的 SELinux 安全上下文 */
int set_security_context(Options *op, const char *filename, const char *type);

/* ===== 路径和前缀 ===== */

/* 获取各组件的默认安装前缀和路径 */
void get_default_prefixes_and_paths(Options *op);
/* 获取 32 位兼容库的安装路径 */
void get_compat32_path(Options *op);

/* ===== 字符串和路径工具 ===== */

/* 字符串替换：将 src 中所有 orig 替换为 replace */
char *nv_strreplace(const char *src, const char *orig, const char *replace);
/* 获取文件名（通过 UI 询问用户或使用默认值） */
char *get_filename(Options *op, const char *def, const char *msg);
/* 安全删除文件（擦除内容后删除，用于密钥文件） */
int secure_delete(Options *op, const char *file);
/* 将包条目标记为无效（设置类型为 NONE） */
void invalidate_package_entry(PackageEntry *entry);
/* 判断 subdir 是否为 dir 的子目录 */
int is_subdirectory(const char *dir, const char *subdir, int *is_subdir);
/* 添加 libGL ABI 兼容符号链接 */
void add_libgl_abi_symlink(Options *op, Package *p);

/* 检查并处理 GLVND 相关文件的安装 */
int check_libglvnd_files(Options *op, Package *p);

#endif /* __NVIDIA_INSTALLER_FILES_H__ */
