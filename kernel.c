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
 */

/*
 * 【文件说明】kernel.c - NVIDIA 安装器的内核模块管理实现文件
 *
 * 本文件是 nvidia-installer 项目中最核心的源文件之一，负责所有与 Linux 内核模块
 * 相关的操作，包括：
 *
 * 1. 内核路径管理：
 *    - determine_kernel_source_path()：定位内核源码树（/lib/modules/<ver>/build 等）
 *    - determine_kernel_output_path()：定位内核构建输出目录（支持 out-of-tree 构建）
 *    - determine_kernel_module_installation_path()：确定模块安装目标路径
 *    - default_kernel_source_path() / default_kernel_module_installation_path()：
 *      提供默认路径的查找逻辑
 *
 * 2. 内核模块编译：
 *    - build_kernel_modules()：从源码编译所有内核模块（调用 make）
 *    - build_kernel_interfaces()：编译内核接口文件（用于生成预编译包）
 *    - run_make()：封装 make 命令执行，支持进度条显示
 *    - run_conftest()：执行 conftest.sh 脚本进行内核配置兼容性检测
 *
 * 3. 预编译内核接口：
 *    - find_precompiled_kernel_interface()：查找与当前内核匹配的预编译接口
 *    - unpack_kernel_modules()：解压并链接预编译接口文件
 *    - scan_dir()：扫描目录查找匹配的预编译文件
 *    - pack_kernel_interface() / pack_kernel_module()：打包编译好的接口/模块
 *
 * 4. 内核模块签名（Secure Boot 支持）：
 *    - sign_kernel_module()：使用 scripts/sign-file 对模块进行数字签名
 *    - create_detached_signature()：创建分离签名（用于预编译包）
 *    - attach_signature()：将分离签名附加到链接后的模块
 *
 * 5. 内核模块加载与测试：
 *    - test_kernel_modules()：通过 SYS_init_module 系统调用测试模块是否可加载
 *    - load_kernel_module()：使用 modprobe 加载模块到运行中的内核
 *    - do_insmod()：通过 mmap + init_module 系统调用直接加载模块
 *    - check_for_loaded_kernel_module()：检查指定模块是否已加载（通过 lsmod）
 *    - rmmod_kernel_module()：卸载指定的内核模块
 *
 * 6. 模块类型管理：
 *    - valid_kernel_module_types()：确定系统支持的模块类型（闭源/开源）
 *    - override_kernel_module_build_directory()：覆盖模块构建目录
 *    - override_kernel_module_type()：覆盖模块类型选择
 *
 * 7. 辅助功能：
 *    - get_kernel_name()：获取目标内核版本名（uname -r 或命令行指定）
 *    - get_machine_arch()：获取机器架构（x86_64、aarch64 等）
 *    - kernel_configuration_conflict()：检测内核配置冲突
 *    - check_for_warning_messages()：从 /proc 读取内核模块的警告消息
 */

/* 标准C库头文件 */
#include <string.h>       /* 字符串操作：strcmp, strlen, strncpy 等 */
#include <unistd.h>       /* POSIX API：access, close, read 等 */
#include <errno.h>        /* 错误码定义：errno, ENOENT, EEXIST 等 */
#include <sys/utsname.h>  /* uname 系统调用：获取内核版本等系统信息 */
#include <sys/types.h>    /* 基本系统数据类型：mode_t, pid_t 等 */
#include <sys/stat.h>     /* 文件状态：stat, fstat 结构体和函数 */
#include <ctype.h>        /* 字符分类：isspace, tolower 等 */
#include <stdlib.h>       /* 通用工具：getenv, free, malloc 等 */
#include <dirent.h>       /* 目录操作：opendir, readdir, closedir */
#include <fcntl.h>        /* 文件控制：open, O_RDONLY 等 */
#include <sys/mman.h>     /* 内存映射：mmap, munmap（用于 do_insmod 加载模块） */
#include <string.h>       /* 重复包含（可能是无意的，但保留原样不修改） */
#include <limits.h>       /* 整数限制：INT_MAX, PATH_MAX 等 */
#include <fts.h>          /* 文件树遍历：fts_open, fts_read（用于扫描 /proc 警告） */
#include <syscall.h>      /* 系统调用号定义：SYS_init_module（直接加载内核模块） */

/* nvidia-installer 项目内部头文件 */
#include "nvidia-installer.h"         /* 核心数据结构：Options、Package、KernelModuleInfo 等 */
#include "kernel.h"                   /* 本文件的头文件，声明对外暴露的函数 */
#include "user-interface.h"           /* 用户界面：ui_error, ui_log, ui_status_begin 等 */
#include "files.h"                    /* 文件操作工具：read_text_file, directory_exists 等 */
#include "misc.h"                     /* 杂项工具：find_system_util, nvstrcat 等 */
#include "precompiled.h"              /* 预编译接口：PrecompiledInfo, PrecompiledFileInfo 等 */
#include "crc.h"                      /* CRC校验：compute_crc, verify_crc */
#include "conflicting-kernel-modules.h" /* 冲突模块列表：conflicting_kernel_modules 数组 */

/* ===== 本文件内部静态函数的前向声明 ===== */

/* 获取默认的内核模块安装路径 */
static char *default_kernel_module_installation_path(Options *op);
/* 获取默认的内核源码路径 */
static char *default_kernel_source_path(Options *op);
/* 在字符串中查找模块名子串（忽略连字符和下划线的差异） */
static char *find_module_substring(char *string, const char *substring);
/* 检查指定的内核模块是否已加载到内核中 */
static int check_for_loaded_kernel_module(Options *op, const char *);
/* 检查 /proc/driver/nvidia/warnings 下的警告消息 */
static void check_for_warning_messages(Options *op);

/* 扫描指定目录，查找匹配的预编译内核接口文件 */
static PrecompiledInfo *scan_dir(Options *op, Package *p,
                                 const char *directory_name,
                                 const char *proc_version_string,
                                 char *const *search_filelist);

/* 构建发行版预编译内核接口目录路径 */
static char *build_distro_precompiled_kernel_interface_dir(Options *op);
/* 将 --kernel-include-path 转换为内核源码路径（去掉末尾的 include 部分） */
static char *convert_include_path_to_source_path(const char *inc);
/* 运行 conftest.sh 脚本进行内核配置兼容性测试 */
static int run_conftest(Options *op, const char *dir, const char *args,
                        char **result);
/* 执行 make 命令编译内核模块，支持进度条显示 */
static int run_make(Options *op, Package *p, const char *dir,
                    const char *cli_options, const char *status,
                    const RunCommandOutputMatch *match);
/* 静默加载内核模块（不显示错误信息） */
static void load_kernel_module_quiet(Options *op, const char *module_name);
/* 静默使用 modprobe -r 卸载内核模块 */
static void modprobe_remove_kernel_module_quiet(Options *op, const char *name);
/* 检测内核配置是否存在冲突（如 POWER9 上的内存自动上线问题） */
static int kernel_configuration_conflict(Options *op, Package *p,
                                         int target_system_checks);

/*
 * 多个错误消息共用的提示文本。
 * 当无法找到内核源码时，向用户显示此提示信息，建议安装 kernel-source 或
 * kernel-devel 包，或者使用 --kernel-source-path 手动指定路径。
 */

static const char install_your_kernel_source[] =
"Please make sure you have installed the kernel source files for "
"your kernel and that they are properly configured; on Red Hat "
"Linux systems, for example, be sure you have the 'kernel-source' "
"or 'kernel-devel' RPM installed.  If you know the correct kernel "
"source files are installed, you may specify the kernel source "
"path with the '--kernel-source-path' command line option.";

 


/*
 * determine_kernel_module_installation_path() - 确定内核模块的安装路径。
 *
 * 路径确定的优先级如下：
 * 1. 如果 op->kernel_module_installation_path 已经被设置（通过命令行参数
 *    --kernel-module-installation-path），则直接使用该值。
 * 2. 调用 default_kernel_module_installation_path() 获取默认路径
 *    （通常为 /lib/modules/<kernel>/kernel/drivers/video）。
 * 3. 如果处于专家模式（--expert），则提示用户输入自定义路径。
 *
 * 参数：
 *   op - 全局选项结构体指针，包含命令行参数和运行时状态
 *
 * 返回值：
 *   TRUE  - 成功确定安装路径
 *   FALSE - 无法确定安装路径（路径无效或创建目录失败）
 */

int determine_kernel_module_installation_path(Options *op)
{
    char *result;
    int count = 0;

    /* 如果路径已通过命令行参数指定，直接返回成功 */
    if (op->kernel_module_installation_path) return TRUE;

    /* 获取默认的内核模块安装路径 */
    op->kernel_module_installation_path =
        default_kernel_module_installation_path(op);

    /* 如果无法确定默认路径，返回失败 */
    if (!op->kernel_module_installation_path) return FALSE;

    /* 专家模式下允许用户手动输入安装路径 */
    if (op->expert) {

    ask_for_kernel_install_path:

        /* 弹出输入框，显示当前默认路径，让用户确认或修改 */
        result = ui_get_input(op, op->kernel_module_installation_path,
                              "Kernel module installation path");
        if (result && result[0]) {
            /* 用户输入了有效路径，替换默认路径 */
            free(op->kernel_module_installation_path);
            op->kernel_module_installation_path = result;
            /* 让用户确认该路径（可能涉及创建不存在的目录等） */
            if (!confirm_path(op, op->kernel_module_installation_path)) {
                return FALSE;
            }
        } else {
            /* 用户输入为空或无效 */
            if (result) free(result);

            /* 允许用户重试 NUM_TIMES_QUESTIONS_ASKED 次 */
            if (++count < NUM_TIMES_QUESTIONS_ASKED) {
                ui_warn(op, "Invalid kernel module installation path.");
                goto ask_for_kernel_install_path;
            } else {
                ui_error(op, "Unable to determine kernel module "
                         "installation path.");

                return FALSE;
            }
        }
    }

    /* 创建安装路径目录（如果不存在），权限设为 0755 */
    if (!mkdir_with_log(op, op->kernel_module_installation_path, 0755))
        return FALSE;

    /* 在专家模式下记录最终确定的安装路径 */
    ui_expert(op, "Kernel module installation path: %s",
              op->kernel_module_installation_path);

    return TRUE;

} /* determine_kernel_module_installation_path() */



/*
 * run_conftest() - 运行 conftest.sh 脚本进行内核配置兼容性检测。
 *
 * conftest.sh 是 NVIDIA 驱动源码中的配置检测脚本，用于检测目标内核是否
 * 支持所需的内核API和配置选项。每个检测项（conftest）都对应一个特定的
 * 内核特性，例如检查某个函数是否存在、某个宏是否定义等。
 *
 * 该脚本的调用格式为：
 *   sh conftest.sh <编译器> <架构> <内核源码路径> <内核输出路径> <测试参数>
 *
 * 参数：
 *   op     - 全局选项结构体
 *   dir    - conftest.sh 所在的目录路径
 *   args   - 传递给 conftest.sh 的附加参数（指定要运行的检测项）
 *   result - 输出参数，存储脚本执行的输出内容；可以为 NULL
 *
 * 返回值：
 *   TRUE  - conftest.sh 执行成功（返回码为0）
 *   FALSE - 执行失败（检测不通过或脚本出错）
 */

static int run_conftest(Options *op, const char *dir, const char *args,
                        char **result)
{
    char *kernel_source_path, *kernel_output_path;
    const char *arch;
    int ret;

    /* 初始化输出参数 */
    if (result) {
        *result = NULL;
    }

    /* 获取机器架构（如 x86_64），conftest.sh 需要此信息 */
    arch = get_machine_arch(op);
    if (!arch) {
        return FALSE;
    }

    /*
     * 某些 conftest 检测项不需要内核源码/输出路径；
     * 如果 run_conftest() 在路径确定之前就被调用，这些字段可能还是 NULL。
     * 使用占位符字符串代替 NULL，以防止 nvstrcat() 提前终止字符串拼接
     * （nvstrcat 遇到 NULL 参数会停止拼接）。
     */
    kernel_source_path = kernel_output_path = "DIRECTORY_PLACEHOLDER";
    if (op->kernel_source_path) {
        kernel_source_path = op->kernel_source_path;
    }
    if (op->kernel_output_path) {
        kernel_output_path = op->kernel_output_path;
    }

    /* 执行 conftest.sh 脚本，传入编译器、架构、内核路径和测试参数 */
    ret = run_command(op, result, FALSE, 0, TRUE,
                      "sh \"", dir, "/conftest.sh\" \"",
                      op->utils[CC], "\" \"",
                      arch, "\" \"",
                      kernel_source_path, "\" \"",
                      kernel_output_path, "\" ",
                      args, NULL);

    return ret == 0;
} /* run_conftest() */



/*
 * determine_kernel_source_path() - 查找内核源码树的完整路径。
 *
 * 当需要编译内核接口文件时，由 install_from_cwd() 调用此函数。
 * 成功时将路径赋值给 op->kernel_source_path 并返回 TRUE。
 * 如果找不到内核源码树，则返回 FALSE。
 *
 * 处理流程：
 * 1. 调用 default_kernel_source_path() 获取默认路径
 * 2. 专家模式下允许用户手动修改路径
 * 3. 对路径进行多项有效性校验：
 *    - 不能是 /usr（无效路径）
 *    - 目录必须存在
 *    - 必须包含 include/linux/kernel.h 头文件
 *    - 必须包含 version.h（表明内核源码已配置）
 * 4. 确定内核输出路径（可能与源码路径不同）
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体（此处未直接使用，但传递给子函数）
 *
 * 返回值：
 *   TRUE  - 成功找到有效的内核源码路径
 *   FALSE - 无法找到或路径无效
 */

int determine_kernel_source_path(Options *op, Package *p)
{
    char *result;
    char *version_h, *uapi_version_h;
    int ret, count = 0;

    /* 获取默认的内核源码路径 */
    op->kernel_source_path = default_kernel_source_path(op);

    /* 专家模式下允许用户手动指定路径 */
    if (op->expert) {

    ask_for_kernel_source_path:

        result = ui_get_input(op, op->kernel_source_path,
                              "Kernel source path");
        if (result && result[0]) {
            /* 用户输入了路径，检查目录是否存在 */
            if (!directory_exists(result)) {
                ui_warn(op, "Kernel source path '%s' does not exist.",
                        result);
                free(result);

                /* 允许重试有限次数 */
                if (++count < NUM_TIMES_QUESTIONS_ASKED) {
                    goto ask_for_kernel_source_path;
                } else {
                    op->kernel_source_path = NULL;
                }
            } else {
                op->kernel_source_path = result;
            }
        } else {
            /* 用户输入为空 */
            ui_warn(op, "Invalid kernel source path.");
            if (result) free(result);

            if (++count < NUM_TIMES_QUESTIONS_ASKED) {
                goto ask_for_kernel_source_path;
            } else {
                op->kernel_source_path = NULL;
            }
        }
    }

    /* 如果经过上述所有尝试仍然没有找到内核源码路径，放弃 */
    if (!op->kernel_source_path) {
        ui_error(op, "Unable to find the kernel source tree for the "
                 "currently running kernel.  %s", install_your_kernel_source);

        /*
         * 这里本可以再次询问用户内核源码路径，但用户已经有多种方式
         * 来指定路径（命令行参数、环境变量、交互输入），所以不再额外询问。
         */

        return FALSE;
    }

    /* 拒绝 /usr 作为内核源码路径（这是一个明显的错误路径） */
    if (!strcmp(op->kernel_source_path, "/usr") ||
            !strcmp(op->kernel_source_path, "/usr/")) {
        ui_error (op, "The kernel source path '%s' is invalid.  %s",
                  op->kernel_source_path, install_your_kernel_source);
        op->kernel_source_path = NULL;
        return FALSE;
    }

    /* 验证内核源码路径对应的目录存在 */
    if (!directory_exists(op->kernel_source_path)) {
        ui_error (op, "The kernel source path '%s' does not exist.  %s",
                  op->kernel_source_path, install_your_kernel_source);
        op->kernel_source_path = NULL;
        return FALSE;
    }

    /*
     * 验证 <source_path>/include/linux/kernel.h 存在。
     * 这是内核源码树的一个基本标志文件，如果不存在则说明路径可能不正确。
     */
    result = nvstrcat(op->kernel_source_path, "/include/linux/kernel.h", NULL);
    if (access(result, F_OK) == -1) {
        ui_error(op, "The kernel header file '%s' does not exist.  "
                 "The most likely reason for this is that the kernel source "
                 "path '%s' is incorrect.  %s", result,
                 op->kernel_source_path, install_your_kernel_source);
        free(result);
        return FALSE;
    }
    free(result);

    /* 确定内核构建输出路径（可能与源码路径不同，例如 out-of-tree 构建） */
    if (!determine_kernel_output_path(op)) return FALSE;

    /*
     * 检查内核输出目录中是否存在 version.h 文件。
     * 较新的内核使用 include/generated/uapi/linux/version.h，
     * 较旧的内核使用 include/linux/version.h。
     * 至少需要存在其中一个，表明内核源码已经过配置（make *config）。
     */

#define VERSION_H_PATH "/include/linux/version.h"
#define UAPI_VERSION_H_PATH "/include/generated/uapi/linux/version.h"

    version_h = nvstrcat(op->kernel_output_path, VERSION_H_PATH, NULL);
    uapi_version_h = nvstrcat(op->kernel_output_path, UAPI_VERSION_H_PATH,
                              NULL);

    ret = access(version_h, F_OK) == 0 || access(uapi_version_h, F_OK) == 0;

    if (ret) {
        /* 找到了已配置的内核源码树，记录路径信息 */
        ui_log(op, "Kernel source path: '%s'\n", op->kernel_source_path);
        ui_log(op, "Kernel output path: '%s'\n", op->kernel_output_path);
    } else {
        /* version.h 不存在，内核源码可能未配置 */
        ui_error(op, "Neither the '%s' nor the '%s' kernel header file exists. "
                 "The most likely reason for this is that the kernel "
                 "source files in '%s' have not been configured.",
                 version_h, uapi_version_h, op->kernel_output_path);
    }

    free(version_h);
    free(uapi_version_h);

    return ret;

} /* determine_kernel_source_path() */


/*
 * determine_kernel_output_path() - 确定内核构建输出路径。
 *
 * Linux 内核支持 out-of-tree 构建（O= 参数），此时构建输出目录与源码目录不同。
 * 典型场景：内核源码在 /lib/modules/<ver>/source，构建输出在 /lib/modules/<ver>/build。
 *
 * 如果未通过任何方式指定，则默认假设输出路径与源码路径相同。
 *
 * 路径确定的优先级：
 * 1. --kernel-output-path 命令行参数
 * 2. SYSOUT 环境变量
 * 3. 如果源码路径是 /lib/modules/<ver>/source（或 .../build/source），
 *    则输出路径设为 /lib/modules/<ver>/build
 * 4. 回退到与源码路径相同
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   TRUE  - 成功确定输出路径
 *   FALSE - 指定的输出路径不存在
 */

int determine_kernel_output_path(Options *op)
{
    char *str, *tmp;

    /* 优先级1：检查 --kernel-output-path 命令行参数 */
    if (op->kernel_output_path) {
        ui_log(op, "Using the kernel output path '%s' as specified by the "
               "'--kernel-output-path' commandline option.",
               op->kernel_output_path);

        if (!directory_exists(op->kernel_output_path)) {
            ui_error(op, "The kernel output path '%s' does not exist.",
                     op->kernel_output_path);
            op->kernel_output_path = NULL;
            return FALSE;
        }

        return TRUE;
    }

    /* 优先级2：检查 SYSOUT 环境变量 */
    str = getenv("SYSOUT");
    if (str) {
        ui_log(op, "Using the kernel output path '%s', as specified by the "
               "SYSOUT environment variable.", str);
        op->kernel_output_path = str;

        if (!directory_exists(op->kernel_output_path)) {
            ui_error(op, "The kernel output path '%s' does not exist.",
                     op->kernel_output_path);
            op->kernel_output_path = NULL;
            return FALSE;
        }

        return TRUE;
    }

    /*
     * 优先级3：检查 /lib/modules/`uname -r`/{source,build} 的关系。
     * 如果内核源码路径以 /lib/modules/<ver>/source 或
     * /lib/modules/<ver>/build/source 开头，则推断输出路径为
     * /lib/modules/<ver>/build。
     * 这是大多数发行版的标准布局。
     */
    tmp = get_kernel_name(op);

    if (tmp) {
        char *source_path, *build_source_path;
        int len_source_path, len_build_source_path;

        source_path = nvstrcat("/lib/modules/", tmp, "/source", NULL);
        len_source_path = strlen(source_path);

        build_source_path = nvstrcat("/lib/modules/", tmp, "/build/source", NULL);
        len_build_source_path = strlen(build_source_path);

        /* 检查内核源码路径是否匹配上述模式 */
        if ((!strncmp(op->kernel_source_path, source_path, len_source_path)) ||
            (!strncmp(op->kernel_source_path, build_source_path, len_build_source_path))) {
            nvfree(source_path);
            nvfree(build_source_path);
            str = nvstrcat("/lib/modules/", tmp, "/build", NULL);

            if (directory_exists(str)) {
                op->kernel_output_path = str;
                return TRUE;
            }
        }
        else  {
            nvfree(source_path);
            nvfree(build_source_path);
        }
        nvfree(str);
    }

    /* 优先级4：回退 - 输出路径与源码路径相同 */
    op->kernel_output_path = op->kernel_source_path;
    return TRUE;
}


/*
 * attach_signature() - 将分离签名附加到已链接的内核模块。
 *
 * 预编译内核接口包中可以包含分离签名（detached signature）。当使用预编译接口
 * 时，需要先验证链接后模块的 CRC 校验和是否与签名时的模块一致，然后将签名
 * 数据追加到模块文件末尾。
 *
 * 如果 CRC 不匹配（通常是因为目标系统的链接器版本与构建系统不同），或者
 * 无法打开模块文件，则向用户提供选择：安装未签名模块或中止安装。
 *
 * 参数：
 *   op          - 全局选项结构体
 *   p           - 安装包结构体，包含模块构建目录等信息
 *   fileInfo    - 预编译文件信息，包含签名数据、CRC 等
 *   module_name - 链接后的模块文件名（如 nvidia.ko）
 *
 * 返回值：
 *   TRUE  - 签名附加成功，或用户选择安装未签名模块
 *   FALSE - 签名附加失败且用户选择中止安装
 */
static int attach_signature(Options *op, Package *p,
                            const PrecompiledFileInfo *fileInfo,
                            const char *module_name) {
    uint32 actual_crc;
    char *module_path;
    int ret = FALSE, command_ret;

    /* 当签名附加失败时向用户展示的选项 */
    const char *choices[2] = {
        "Install unsigned kernel module",
        "Abort installation"
    };

    ui_log(op, "Attaching module signature to linked kernel module.");

    /* 构建模块文件的完整路径 */
    module_path = nvstrcat(p->kernel_module_build_directory, "/",
                           fileInfo->target_directory, "/", module_name, NULL);

    /* 验证链接后模块的 CRC 是否与预编译包中记录的 CRC 一致 */
    command_ret = verify_crc(op, module_path, fileInfo->linked_module_crc,
                             &actual_crc);

    if (command_ret) {
        /* CRC 验证通过，尝试将签名追加到模块文件末尾 */
        FILE *module_file;

        module_file = fopen(module_path, "a+");

        if (module_file && fileInfo->signature_size) {
            /* 将签名数据写入模块文件末尾 */
            command_ret = fwrite(fileInfo->signature, 1,
                                 fileInfo->signature_size, module_file);
            if (command_ret == fileInfo->signature_size) {
                /* 写入字节数与签名大小一致，标记模块已签名 */
                op->kernel_module_signed = ret = !ferror(module_file);
            }
        } else {
            /* 无法打开模块文件或签名数据为空，让用户决定是否继续 */
            ret = (ui_multiple_choice(op, choices, 2, 1,
                                      "A detached signature was included with "
                                      "the precompiled interface, but opening "
                                      "the linked kernel module and/or the "
                                      "signature file failed.\n\nThe detached "
                                      "signature will not be added; would you "
                                      "still like to install the unsigned "
                                      "kernel module?") == 0);
        }

        if (module_file) {
            fclose(module_file);
        }
    } else {
        /*
         * CRC 验证失败：链接后的模块校验和与预编译包中记录的不一致。
         * 这通常发生在目标系统使用了不同版本的链接器时。
         * 让用户决定是否安装未签名的模块。
         */
        ret = (ui_multiple_choice(op, choices, 2, 1,
                                  "A detached signature was included with the "
                                  "precompiled interface, but the checksum of "
                                  "the linked kernel module (%d) did not match "
                                  "the checksum of the the kernel module for "
                                  "which the detached signature was generated "
                                  "(%d).\n\nThis can happen if the linker on "
                                  "the installation target system is not the "
                                  "same as the linker on the system that built "
                                  "the precompiled interface.\n\nThe detached "
                                  "signature will not be added; would you "
                                  "still like to install the unsigned kernel "
                                  "module?", actual_crc,
                                  fileInfo->linked_module_crc) == 0);
    }

    /* 记录最终结果 */
    if (ret) {
        if (op->kernel_module_signed) {
            ui_log(op, "Signature attached successfully.");
        } else {
            ui_log(op, "Signature not attached.");
        }
    } else {
        ui_error(op, "Failed to attach signature.");
    }

    nvfree(module_path);
    return ret;
} /* attach_signature() */


/*
 * unpack_kernel_modules() - 解压预编译文件包，并将预编译内核接口与
 * 对应的纯二进制核心目标文件链接，生成完整的内核模块。
 *
 * NVIDIA 驱动由两部分组成：
 * - 内核接口文件（如 nv-linux.o）：依赖内核版本，需要针对目标内核编译
 * - 核心目标文件（如 nv-kernel.o_binary）：纯二进制，与内核版本无关
 *
 * 链接命令示例：
 *   ld -r -o nvidia.ko nv-linux.o nvidia/nv-kernel.o_binary
 *
 * 如果预编译文件本身就是完整的内核模块（而非接口文件），
 * 则解压后不需要额外的链接步骤。
 *
 * 参数：
 *   op              - 全局选项结构体
 *   p               - 安装包结构体
 *   build_directory - 解压和构建的目标目录
 *   fileInfo        - 预编译文件信息（包含文件类型、名称、签名等）
 *
 * 返回值：
 *   TRUE  - 解压（和链接）成功
 *   FALSE - 解压或链接失败
 */

int unpack_kernel_modules(Options *op, Package *p, const char *build_directory,
                          const PrecompiledFileInfo *fileInfo)
{
    int ret;
    uint32 attrmask;

    /* 验证文件类型：只接受接口文件或完整模块文件 */
    if (fileInfo->type != PRECOMPILED_FILE_TYPE_INTERFACE &&
        fileInfo->type != PRECOMPILED_FILE_TYPE_MODULE) {
        ui_error(op, "The file does not appear to be a valid precompiled "
                 "kernel interface or module.");
        return FALSE;
    }

    /* 解压预编译文件到构建目录 */
    ret = precompiled_file_unpack(op, fileInfo, build_directory);
    if (!ret) {
        ui_error(op, "Failed to unpack the precompiled file.");
        return FALSE;
    } else if (fileInfo->type == PRECOMPILED_FILE_TYPE_MODULE) {
        /* 如果是完整模块，解压即可，无需链接 */
        ui_log(op, "Kernel module unpacked successfully.");
        return TRUE;
    }

    /*
     * 文件是接口文件，需要将其与核心目标文件链接。
     * 使用 ld -r（可重定位链接）生成最终的 .ko 文件。
     * LD_OPTIONS 通常包含 "-r"（可重定位输出）。
     */
    ret = run_command(op, NULL, TRUE, 0, TRUE,
                     "cd ", build_directory,
                     "; ", op->utils[LD], " ", LD_OPTIONS, " -o ",
                     fileInfo->linked_module_name, " ",
                     fileInfo->target_directory, "/", fileInfo->name, " ",
                     fileInfo->target_directory, "/", fileInfo->core_object_name,
                     NULL);

    if (ret != 0) {
        ui_error(op, "Unable to link kernel module.");
        return FALSE;
    }

    ui_log(op, "Kernel module linked successfully.");

    /*
     * 如果预编译文件包含分离签名和链接模块CRC，
     * 则将签名附加到链接后的模块文件。
     */
    attrmask = PRECOMPILED_ATTR(DETACHED_SIGNATURE) |
               PRECOMPILED_ATTR(LINKED_MODULE_CRC);

    if ((fileInfo->attributes & attrmask) == attrmask) {
        return attach_signature(op, p, fileInfo,
                                fileInfo->linked_module_name);
    }

    return TRUE;

}

/*
 * count_lines() - 估算编译内核模块时预期产生的输出行数。
 *
 * 此函数用于进度条显示：通过预先估算 make 命令的输出行数，
 * 可以在编译过程中显示合理的进度百分比。
 *
 * 估算方法是运行一个专用的 count-lines.mk Makefile，该 Makefile
 * 会计算 conftest 检测项数量、需编译的目标文件数量和模块数量，
 * 然后据此计算各类输出行的预期数量。
 *
 * 参数：
 *   op            - 全局选项结构体
 *   p             - 安装包结构体
 *   dir           - 内核模块构建目录
 *   single_module - 如果非 NULL，则只估算编译该单个模块的行数
 *                  （用于重试编译失败的单个模块时）
 *
 * 返回值：
 *   动态分配的 RunCommandOutputMatch 数组，用于 run_command() 的进度匹配。
 *   数组中每个条目包含一个匹配前缀和预期行数。
 *   调用者负责释放此数组。
 */
static RunCommandOutputMatch *count_lines(Options *op, Package *p,
                                          const char *dir,
                                          const char *single_module)
{
    RunCommandOutputMatch *ret = nvalloc(sizeof(*ret) * 5);
    int conftest_count, object_count, module_count, count_success = FALSE;
    char *data = NULL;

    /*
     * 构建 make 命令行。这里刻意不使用 run_make()，因为 count-lines.mk
     * 的输出不应该被记录到日志或显示给用户——它只是用于内部估算。
     */
    if (run_command(op, &data, FALSE, NULL, TRUE,
                    "cd ", dir, "; ",
                    op->utils[MAKE], " -f count-lines.mk count "
                    "NV_EXCLUDE_KERNEL_MODULES=", p->excluded_kernel_modules,
                    single_module ? "" : NULL,
                    " NV_KERNEL_MODULES=", single_module,
                    NULL) == 0) {
        /* 解析输出，格式为："conftests:<n> objects:<n> modules:<n>" */
        if (sscanf(data, "conftests:%d objects:%d modules:%d",
                   &conftest_count, &object_count, &module_count) == 3) {
            count_success = TRUE;
        }
    }

    if (!count_success) {
        /*
         * 估算失败。由于行数计数仅用于进度条显示（纯装饰性目的），
         * 记录错误后使用近似的默认值即可，不需要导致安装失败。
         */
        ui_log(op, "Failed to estimate output lines: %s", data);
        conftest_count = 250; object_count = 200; module_count = 5;
    }

    /*
     * 匹配规则0：Kbuild 进入/离开源码目录和输出目录时各输出一行，共4行。
     * 输出格式例如："make[1]: Entering directory '/lib/modules/...'"
     */
    ret[0].lines = 4;
    ret[0].initial_match = "make[";

    /*
     * 匹配规则1：每个 C 源文件编译成目标文件时输出一行 "  CC [M] ..."。
     * 此外，每个模块还会编译一个 $module_name.mod.o 文件。
     * 旧内核输出 "  CC "，新内核输出 "  CC [M] "。
     */
    ret[1].lines = object_count + module_count;
    ret[1].initial_match = "  CC ";

    /*
     * 匹配规则2：每个模块在 MODPOST 前后各有一次链接操作：
     * - MODPOST 前链接 $module_name.o
     * - MODPOST 后链接 $module_name.ko
     * 每次链接输出一行 "  LD [M] ..."
     */
    ret[2].lines = module_count * 2;
    ret[2].initial_match = "  LD [M] ";

    /*
     * 匹配规则3：每个 conftest 检测项输出一行 " CONFTEST: ..."。
     * 当重试编译单个模块时（single_module != NULL），假设所有 conftest
     * 已在之前运行过，因此不再计算。
     */
    if (single_module == NULL) {
        ret[3].lines = conftest_count;
        ret[3].initial_match = " CONFTEST: ";
    }

    return ret;
}


/*
 * check_file() - 检查指定的内核模块 .ko 文件是否已成功生成。
 *
 * 如果文件不存在，会尝试单独重新编译该模块。这是因为在多模块编译中，
 * 一个模块的编译失败可能影响其他模块的生成。通过单独重试，可以隔离
 * 出真正失败的模块。
 *
 * 参数：
 *   op      - 全局选项结构体
 *   p       - 安装包结构体
 *   dir     - 构建目录
 *   modname - 模块名称（不含 .ko 后缀）
 *
 * 返回值：
 *   TRUE  - 模块文件存在（编译成功）
 *   FALSE - 模块文件不存在（编译失败）
 */
static int check_file(Options *op, Package *p, const char *dir,
                      const char *modname)
{
    int ret;
    char *path;

    /* 构建 .ko 文件的完整路径 */
    path = nvstrcat(dir, "/", modname, ".ko", NULL);
    ret = access(path, F_OK);

    if (ret == -1) {
        /*
         * .ko 文件不存在。尝试单独编译该模块，以排除其他模块编译失败
         * 对本模块的影响。通过设置 NV_KERNEL_MODULES 变量限定只编译
         * 指定的模块。
         */
        char *single_module_list = nvstrcat("NV_KERNEL_MODULES=\"", modname,
                                            "\"", NULL);
        char *rebuild_msg = nvstrcat("Checking to see whether the ", modname,
                                     " kernel module was successfully built",
                                     NULL);
        RunCommandOutputMatch *match = count_lines(op, p, dir, modname);
        run_make(op, p, dir, single_module_list, rebuild_msg, match);
        nvfree(single_module_list);
        nvfree(rebuild_msg);
        nvfree(match);

        /* 重新检查文件是否已生成 */
        ret = access(path, F_OK);
    }

    nvfree(path);

    if (ret == -1) {
        ui_error(op, "The %s kernel module was not created.", modname);
    }

    return ret != -1;
}



/*
 * build_kernel_modules() - 编译安装包中的所有内核模块。
 *
 * 这是编译内核模块的主入口函数。它实际上是 build_kernel_interfaces() 的
 * 简单封装——当 fileInfos 参数为 NULL 时，build_kernel_interfaces() 只编译
 * 模块而不打包接口文件。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体，包含所有内核模块的信息
 *
 * 返回值：
 *   成功编译的模块数量（非零为成功），0 表示失败
 */

int build_kernel_modules(Options *op, Package *p)
{
    return build_kernel_interfaces(op, p, NULL);
}



/*
 * test_sign_file() - 检查指定目录下是否存在可执行的 scripts/sign-file 工具。
 *
 * scripts/sign-file 是 Linux 内核源码中的模块签名工具，用于为内核模块
 * 生成数字签名（Secure Boot 环境下必需）。
 *
 * 参数：
 *   dir - 要搜索的目录路径（通常是内核源码或输出路径）
 *
 * 返回值：
 *   成功：返回 sign-file 的完整路径（调用者负责释放）
 *   失败：返回 NULL
 */
static char *test_sign_file(const char *dir)
{
    char *path = nvstrcat(dir, "/scripts/sign-file", NULL);
    struct stat st;

    /* 检查文件是否存在且具有可执行权限（任意用户/组/其他） */
    if (stat(path, &st) == 0 &&
        (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) != 0) {
        return path;
    }

    nvfree(path);
    return NULL;
}



/*
 * sign_kernel_module() - 对内核模块进行数字签名。
 *
 * 在启用 Secure Boot 的系统上，内核模块必须经过签名才能加载。
 * 此函数使用内核源码中的 scripts/sign-file 工具对模块进行签名。
 *
 * 签名命令格式：
 *   sign-file <hash_algo> <private_key> <public_key> <module_file>
 *
 * 调用者需要确保：
 * 1. 内核模块已成功编译
 * 2. op->module_signing_secret_key（私钥）和
 *    op->module_signing_public_key（公钥）已设置
 *
 * 参数：
 *   op               - 全局选项结构体
 *   build_directory  - 模块文件所在的目录
 *   module_filename  - 模块文件名（如 nvidia.ko）
 *   status           - 是否显示进度条（TRUE=显示，FALSE=仅记录日志）
 *
 * 返回值：
 *   TRUE  - 签名成功
 *   FALSE - 签名失败（找不到 sign-file、缺少哈希算法、或签名命令执行失败）
 */
int sign_kernel_module(Options *op, const char *build_directory,
                       const char *module_filename, int status) {
    const RunCommandOutputMatch output_match[] = {
        { .lines = 1, .initial_match = NULL },
        { 0 }
    };
    int success;

    /*
     * 延迟初始化签名脚本路径。
     * 优先在内核输出路径中查找 scripts/sign-file，
     * 如果找不到则在内核源码路径中查找。
     */
    if (!op->module_signing_script) {
        op->module_signing_script = test_sign_file(op->kernel_output_path);
        if (!op->module_signing_script) {
            op->module_signing_script = test_sign_file(op->kernel_source_path);
        }
    }

    /* 如果仍然找不到 sign-file 工具，报错并返回 */
    if (!op->module_signing_script) {
        ui_error(op, "nvidia-installer cannot sign %s without the `sign-file` "
                 "module signing program, and was unable to automatically "
                 "locate it. If you need to sign the NVIDIA kernel modules, "
                 "please try again and set the '--module-signing-script' "
                 "option on the installer's command line.", module_filename);
        return FALSE;
    }

    if (status) {
        ui_status_begin(op, "Signing kernel module:", "Signing");
    }

    /* 延迟初始化签名哈希算法（通过 conftest 自动检测） */
    if (!op->module_signing_hash) {
        op->module_signing_hash = guess_module_signing_hash(op,
                                                            build_directory);
    }

    if (!op->module_signing_hash) {
        ui_error(op, "The installer cannot sign %s without specifying a hash "
                 "algorithm on the command line to %s, and was also unable to "
                 "automatically detect the hash. If you need to sign the "
                 "NVIDIA kernel modules, please try again and set the "
                 "'--module-signing-hash' option on the installer's command "
                 "line.", module_filename, op->module_signing_script);
    }

    /* 执行签名命令：sign-file <hash> <secret_key> <public_key> <module> */
    success = (run_command(op, NULL, TRUE, output_match, TRUE,
                           "\"", op->module_signing_script, "\" ",
                           op->module_signing_hash, " \"",
                           op->module_signing_secret_key, "\" \"",
                           op->module_signing_public_key, "\" \"",
                           build_directory, "/", module_filename, "\"", NULL) == 0);

    /* 更新进度条或记录日志 */
    if (status) {
        ui_status_end(op, success ? "done." : "Failed to sign kernel module.");
    } else {
        ui_log(op, success ? "Signed kernel module." : "Module signing failed");
    }

    /* 记录模块签名状态到全局选项，供后续步骤使用 */
    op->kernel_module_signed = success;
    return success;
}



/*
 * create_detached_signature() - 创建分离签名。
 *
 * 分离签名的创建流程：
 * 1. 将预编译接口链接成完整模块
 * 2. 计算链接后（未签名）模块的 CRC 校验和
 * 3. 对模块进行签名（签名会被追加到模块文件末尾）
 * 4. 提取签名部分（文件中超出原始大小的部分即为签名）
 * 5. 将 CRC 和签名存储到 PrecompiledFileInfo 中
 *
 * 分离签名的好处是：签名可以与预编译接口一起分发，
 * 安装时无需重新签名就能获得已签名的模块。
 *
 * 参数：
 *   op              - 全局选项结构体
 *   p               - 安装包结构体
 *   build_dir       - 构建目录
 *   fileInfo        - 预编译文件信息（输入/输出，签名和CRC会写入此结构）
 *   module_filename - 链接后的模块文件名
 *
 * 返回值：
 *   TRUE  - 分离签名创建成功
 *   FALSE - 过程中某个步骤失败
 */
static int create_detached_signature(Options *op, Package *p,
                                     const char *build_dir,
                                     PrecompiledFileInfo *fileInfo,
                                     const char *module_filename)
{
    int ret, command_ret;
    struct stat st;
    char *module_path = NULL, *error = NULL, *target_dir = NULL;

    ui_status_begin(op, "Creating a detached signature for the linked "
                    "kernel module:", "Linking module");

    /* 步骤1：解压接口文件并链接成完整模块 */
    ret = unpack_kernel_modules(op, p, build_dir, fileInfo);

    if (!ret) {
        ui_error(op, "Failed to link a kernel module for signing.");
        goto done;
    }

    /* 获取链接后模块的文件大小（用于后续提取签名） */
    target_dir = nvstrcat(build_dir, "/", fileInfo->target_directory, NULL);
    module_path = nvstrcat(target_dir, "/", module_filename, NULL);
    command_ret = stat(module_path, &st);

    if (command_ret != 0) {
        ret = FALSE;
        error = "Unable to determine size of linked module.";
        goto done;
    }

    /* 步骤2：计算未签名模块的 CRC 校验和 */
    ui_status_update(op, .25, "Generating module checksum");

    fileInfo->linked_module_crc = compute_crc(op, module_path);
    fileInfo->attributes |= PRECOMPILED_ATTR(LINKED_MODULE_CRC);

    /* 步骤3：对模块进行签名（签名数据被追加到文件末尾） */
    ui_status_update(op, .50, "Signing linked module");

    ret = sign_kernel_module(op, target_dir, module_filename, FALSE);

    if (!ret) {
        error = "Failed to sign the linked kernel module.";
        goto done;
    }

    /*
     * 步骤4：提取分离签名。
     * 签名前的文件大小为 st.st_size，签名后文件变大了。
     * byte_tail() 提取文件中从 st.st_size 偏移处开始到末尾的数据，
     * 即签名部分。
     */
    ui_status_update(op, .75, "Detaching module signature");

    fileInfo->signature_size = byte_tail(module_path, st.st_size,
                                         &(fileInfo->signature));

    if (!(fileInfo->signature) || fileInfo->signature_size == 0) {
        error = "Failed to detach the module signature";
        goto done;
    }

    /* 步骤5：标记预编译文件包含分离签名 */
    fileInfo->attributes |= PRECOMPILED_ATTR(DETACHED_SIGNATURE);

done:
    if (ret) {
        ui_status_end(op, "done.");
    } else {
        ui_status_end(op, "Error.");
        if (error) {
            ui_error(op, "%s", error);
        }
    }

    nvfree(module_path);
    nvfree(target_dir);
    return ret;
}


/*
 * pack_kernel_interface() - 将编译好的内核接口文件打包到 PrecompiledFileInfo 结构中。
 *
 * 打包流程：
 * 1. 读取接口文件内容到 PrecompiledFileInfo
 * 2. 如果提供了签名密钥，创建分离签名并附加到记录中
 *
 * 参数：
 *   op               - 全局选项结构体
 *   p                - 安装包结构体
 *   build_dir        - 构建目录
 *   fileInfo         - 输出参数，存储打包后的文件信息
 *   kernel_interface - 内核接口文件名（如 nv-linux.o）
 *   module_filename  - 最终链接后的模块文件名（如 nvidia.ko）
 *   core_file        - 核心目标文件名（如 nv-kernel.o_binary）
 *
 * 返回值：
 *   TRUE  - 打包成功
 *   FALSE - 打包失败
 */
static int pack_kernel_interface(Options *op, Package *p,
                                 const char *build_dir,
                                 PrecompiledFileInfo *fileInfo,
                                 const char *kernel_interface,
                                 const char *module_filename,
                                 const char *core_file)
{
    int command_ret;
    char *file_path = nvstrcat(build_dir, "/", kernel_interface, NULL);

    /* 读取接口文件，记录其与模块文件、核心文件的关联关系 */
    command_ret = precompiled_read_interface(fileInfo, file_path,
                                             module_filename,
                                             core_file, ".");

    nvfree(file_path);

    if (command_ret) {
        /* 如果用户提供了模块签名密钥，则为接口创建分离签名 */
        if (op->module_signing_secret_key && op->module_signing_public_key) {
            if (!create_detached_signature(op, p, build_dir, fileInfo,
                                           module_filename)) {
                return FALSE;
            }
        }
        return TRUE;
    }

    return FALSE;
}


/*
 * handle_optional_module_failure() - 处理可选模块编译/加载失败的情况。
 *
 * 某些内核模块是可选的（如 nvidia-peermem），当它们失败时，
 * 向用户提示可以使用对应的禁用选项来跳过该模块。
 *
 * 参数：
 *   op     - 全局选项结构体
 *   module - 失败的模块信息
 *   action - 失败的操作描述（"build" 或 "load"）
 */
static void handle_optional_module_failure(Options *op,
                                           KernelModuleInfo module,
                                           const char *action) {
    if (module.is_optional) {
        ui_error(op, "The %s kernel module failed to %s. This kernel module "
                 "is required for the proper operation of %s. If you do not "
                 "need to use %s, you can try to install this driver package "
                 "again with the '--%s' option.",
                 module.module_name, action, module.optional_module_dependee,
                 module.optional_module_dependee, module.disable_option);
    }
}


/*
 * pack_kernel_module() - 将完整的内核模块文件打包到 PrecompiledFileInfo 中。
 *
 * 与 pack_kernel_interface() 不同，此函数用于没有独立接口文件的模块
 * （即模块文件本身就是完整的，不需要与核心目标文件链接）。
 *
 * 如果提供了签名密钥，会先对模块进行嵌入式签名（签名直接嵌入模块文件），
 * 然后再读取签名后的模块进行打包。
 *
 * 参数：
 *   op              - 全局选项结构体
 *   build_dir       - 构建目录
 *   fileInfo        - 输出参数，存储打包后的文件信息
 *   module_filename - 模块文件名
 *
 * 返回值：
 *   TRUE  - 打包成功
 *   FALSE - 签名失败或读取模块失败
 */
static int pack_kernel_module(Options *op,
                              const char *build_dir,
                              PrecompiledFileInfo *fileInfo,
                              const char *module_filename)
{
    int command_ret;
    char *file_path = nvstrcat(build_dir, "/", module_filename, NULL);

    /* 如果提供了签名密钥，先对模块进行嵌入式签名 */
    if (op->module_signing_secret_key && op->module_signing_public_key) {
        if (sign_kernel_module(op, build_dir, module_filename, FALSE)) {
            /* 标记模块包含嵌入式签名 */
            fileInfo->attributes |= PRECOMPILED_ATTR(EMBEDDED_SIGNATURE);
        } else {
            ui_error(op, "Failed to sign precompiled kernel module %s!",
                     module_filename);
            return FALSE;
        }
    }

    /* 读取模块文件内容到 PrecompiledFileInfo */
    command_ret = precompiled_read_module(fileInfo, file_path, "");

    nvfree(file_path);

    if (command_ret) {
        return TRUE;
    }

    return FALSE;
}




/*
 * build_kernel_interfaces() - 编译内核模块和接口文件，并将预编译文件
 * 存储到新分配的 PrecompiledFileInfo 数组中。
 *
 * 此函数是内核模块编译的核心实现，有两种工作模式：
 *
 * 模式1（fileInfos == NULL）：仅编译内核模块
 *   - 由 build_kernel_modules() 调用
 *   - 在 p->kernel_module_build_directory 中直接编译
 *   - 编译后的 .ko 文件留在构建目录中供后续安装步骤使用
 *
 * 模式2（fileInfos != NULL）：编译并打包预编译接口
 *   - 用于生成预编译接口包（可分发给其他相同内核的系统）
 *   - 将源码复制到临时目录中编译，避免污染原始构建目录
 *   - 编译完成后将接口文件打包到 PrecompiledFileInfo 数组
 *   - 临时目录在函数返回前被清理
 *
 * 编译流程：
 * 1. 检查内核配置冲突
 * 2. 运行各项健全性检查（编译器、Dom0、Xen、PREEMPT_RT 等）
 * 3. 清理构建目录（make clean）
 * 4. 执行 make 编译所有模块
 * 5. 检查所有 .ko 文件是否成功生成
 * 6. 如果需要打包，则对每个模块执行打包操作
 *
 * 参数：
 *   op        - 全局选项结构体
 *   p         - 安装包结构体
 *   fileInfos - 输出参数：PrecompiledFileInfo 数组的指针的指针。
 *               如果为 NULL，则只编译不打包。
 *
 * 返回值：
 *   成功编译（和打包）的文件数量；0 表示失败
 */

int build_kernel_interfaces(Options *op, Package *p,
                            PrecompiledFileInfo ** fileInfos)
{
    char *tmpdir = NULL, *builddir;
    int ret, files_packaged = 0, i;
    RunCommandOutputMatch *match;

    /*
     * 编译前需要通过的健全性检查列表。
     * 每项检查对应 conftest.sh 中的一个检测函数。
     */
    struct {
        const char *sanity_check_name;  /* 检查名称（用于日志显示） */
        const char *conftest_name;      /* conftest.sh 中的检测函数名 */
    } sanity_checks[] = {
        { "Compiler", "cc_sanity_check" },       /* 编译器兼容性检查 */
        { "Dom0", "dom0_sanity_check" },          /* Xen Dom0 环境检查 */
        { "Xen", "xen_sanity_check" },            /* Xen 虚拟化环境检查 */
        { "PREEMPT_RT", "preempt_rt_sanity_check" }, /* 实时内核补丁检查 */
        { "vgpu_kvm", "vgpu_kvm_sanity_check" },    /* vGPU KVM 环境检查 */
    };

    /*
     * 检查内核配置冲突。当仅编译模块时（fileInfos == NULL），
     * 执行目标系统检查；当打包接口时不执行（因为可能不在目标系统上运行）。
     */
    if (kernel_configuration_conflict(op, p, fileInfos == NULL)) {
        return 0;
    }

    if (fileInfos) {
        *fileInfos = NULL;
    }

    /* 如果需要打包接口，创建临时目录作为构建目录 */
    if (fileInfos) {
        tmpdir = make_tmpdir(op);
        builddir = tmpdir;

        if (!tmpdir) {
            ui_error(op, "Unable to create a temporary build directory.");
            goto done;
        }

        /* 将内核模块源码复制到临时目录 */
        ui_log(op, "Copying kernel module sources to temporary directory.");

        if (!copy_directory_contents
            (op, p->kernel_module_build_directory, tmpdir)) {
            ui_error(op, "Unable to copy the kernel module sources to temporary "
                     "directory '%s'.", tmpdir);
            goto done;
        }
    } else {
        /* 仅编译模块时，直接在原始构建目录中操作 */
        builddir = p->kernel_module_build_directory;
    }

    /*
     * 注意：此处原本有 touch 操作以避免 make 时间偏差错误，
     * 但实际代码中该操作似乎已被移除（只保留了注释）。
     */

    /* 运行所有健全性检查 */
    for (i = 0; i < ARRAY_LEN(sanity_checks); i++) {
        if (!conftest_sanity_check(op, builddir,
                                   sanity_checks[i].sanity_check_name,
                                   sanity_checks[i].conftest_name)) {
            return FALSE;
        }
    }

    /* 清理构建目录，确保从干净状态开始编译 */
    ui_log(op, "Cleaning kernel module build directory.");
    run_make(op, p, builddir, "clean", NULL, 0);

    /* 估算输出行数并开始编译，显示进度条 */
    match = count_lines(op, p, builddir, NULL);
    ret = run_make(op, p, builddir, "", "Building kernel modules", match);
    nvfree(match);

    /* 逐一检查所有内核模块的 .ko 文件是否成功生成 */
    for (i = 0; i < p->num_kernel_modules; i++) {
        if (!check_file(op, p, builddir, p->kernel_modules[i].module_name)) {
            handle_optional_module_failure(op, p->kernel_modules[i], "build");
            goto done;
        }
    }

    /*
     * 检查整体编译结果：即使所有 .ko 文件都生成了，
     * make 命令本身可能仍然返回了非零状态码。
     */
    if (!ret) {
        goto done;
    }

    ui_log(op, "Kernel module compilation complete.");

    /*
     * 如果不需要打包接口（fileInfos == NULL），直接返回编译好的模块数量。
     */
    if (fileInfos == NULL) {
        files_packaged = p->num_kernel_modules;
        goto done;
    }

    /* 分配 PrecompiledFileInfo 数组（+1 用于末尾哨兵，尽管不确定是否需要） */
    *fileInfos = nvalloc(sizeof(PrecompiledFileInfo) *
        (p->num_kernel_modules + 1));

    /* 逐一打包每个模块的预编译文件 */
    for (files_packaged = 0;
         files_packaged < p->num_kernel_modules;
         files_packaged++) {
        PrecompiledFileInfo *fileInfo = *fileInfos + files_packaged;
        KernelModuleInfo *module = p->kernel_modules + files_packaged;

        if (module->has_separate_interface_file) {
            /*
             * 模块有独立的接口文件：先编译接口文件（make <interface_filename>），
             * 然后打包接口文件和关联信息。
             */
            if (!(run_make(op, p, tmpdir, module->interface_filename,
                           NULL, NULL) &&
                pack_kernel_interface(op, p, tmpdir, fileInfo,
                                      module->interface_filename,
                                      module->module_filename,
                                      module->core_object_name))) {
                goto done;
            }
        } else if (!pack_kernel_module(op, tmpdir, fileInfo,
                                       module->module_filename)) {
            /* 模块没有独立接口文件：直接打包整个模块文件 */
                goto done;
        }
    }

done:

    /* 如果打包失败，释放已分配的数组 */
    if (files_packaged == 0 && fileInfos) {
        nvfree(*fileInfos);
        *fileInfos = NULL;
    }

    /* 清理临时目录 */
    if (tmpdir) {
        remove_directory(op, tmpdir);
        nvfree(tmpdir);
    }

    return files_packaged;
}




/*
 * check_for_warning_messages() - 检查内核模块是否通过 /proc 接口注册了警告消息。
 *
 * NVIDIA 内核模块加载后，如果检测到目标系统存在问题（如硬件兼容性问题），
 * 会在 /proc/driver/nvidia/warnings/ 目录下创建文件来记录警告信息。
 * 此函数扫描该目录并向用户显示所有警告消息。
 *
 * 注意：跳过名为 "README" 的文件（它是说明文件，不是实际警告）。
 *
 * 参数：
 *   op - 全局选项结构体
 */

void check_for_warning_messages(Options *op)
{
    char *paths[2] = { "/proc/driver/nvidia/warnings", NULL };
    FTS *fts;
    FTSENT *ent;
    char *buf = NULL;

    /* 使用 fts 遍历 /proc/driver/nvidia/warnings 目录 */
    fts = fts_open(paths, FTS_LOGICAL, NULL);
    if (!fts) return;

    while ((ent = fts_read(fts)) != NULL) {
        switch (ent->fts_info) {
            case FTS_F:
                /* 跳过 README 文件 */
                if ((strlen(ent->fts_name) == 6) &&
                    !strncmp("README", ent->fts_name, 6))
                    break;
                /* 读取警告文件内容并显示给用户 */
                if (read_text_file(ent->fts_path, &buf)) {
                    ui_warn(op, "%s", buf);
                    nvfree(buf);
                }
                break;
            default:
                /* 忽略非普通文件（如目录、符号链接等） */
                break;
        }
    }

    fts_close(fts);

} /* check_for_warning_messages() */




/*
 * printk 日志级别常量。
 * KERN_ALERT(1) 是较高的优先级，只显示紧急消息。
 * 在加载/卸载模块时设置此级别，可以防止大量内核日志消息干扰用户界面
 * （特别是 curses/ncurses 文本界面）。
 */
#define PRINTK_LOGLEVEL_KERN_ALERT 1

/*
 * set_loglevel() - 通过 /proc/sys 接口设置 printk 日志级别。
 *
 * 读取 /proc/sys/kernel/printk 获取当前日志级别，然后写入新的级别值。
 * 将旧的日志级别通过 old_level 返回给调用者，以便后续恢复。
 *
 * 参数：
 *   level     - 要设置的新日志级别
 *   old_level - 输出参数，存储设置前的日志级别；可以为 NULL（不保存）
 *
 * 返回值：
 *   TRUE  - 成功设置日志级别
 *   FALSE - 设置失败（如 /proc 不可用、权限不足等）
 */
static int set_loglevel(int level, int *old_level)
{
    FILE *fp;
    int loglevel_set = FALSE;

    fp = fopen("/proc/sys/kernel/printk", "r+");
    if (fp) {
        /* 如果需要保存旧级别，先读取当前值 */
        if (!old_level || fscanf(fp, "%d ", old_level) == 1) {
            /*
             * 使用动态缓冲区：内核不对日志级别做范围检查，
             * 所以 procfs 报告的值可能有任意位数。
             */
            char *strlevel = nvasprintf("%d", level);

            fseek(fp, 0, SEEK_SET);
            if (fwrite(strlevel, strlen(strlevel), 1, fp) == 1) {
                loglevel_set = TRUE;
            }

            nvfree(strlevel);
        }
        fclose(fp);
    }

    return loglevel_set;
}


/*
 * do_insmod() - 通过 mmap(2) 和 init_module(2) 系统调用直接加载内核模块。
 *
 * 与 modprobe 不同，此函数直接使用 SYS_init_module 系统调用加载模块，
 * 不经过模块依赖解析。主要用于测试加载（test_kernel_modules），
 * 以验证编译出的模块是否能被内核接受。
 *
 * 加载步骤：
 * 1. 降低 printk 日志级别（防止内核消息干扰 curses 界面）
 * 2. 打开模块文件并 mmap 到内存
 * 3. 调用 SYS_init_module 加载模块
 * 4. 清理并恢复日志级别
 *
 * 参数：
 *   op          - 全局选项结构体（此处实际未直接使用，但保留以保持接口一致性）
 *   module      - 模块文件的完整路径
 *   module_opts - 传递给模块的参数字符串（如 "NVreg_DeviceFileMode=0"）
 *
 * 返回值：
 *   0      - 加载成功
 *   ENOENT - 文件不存在或打开失败
 *   EEXIST - 模块已加载
 *   其他   - init_module 返回的 errno 值
 */
static int do_insmod(Options *op, const char *module, const char *module_opts)
{
    int ret = ENOENT, loglevel_set, old_loglevel, fd = -1;
    void *buf = MAP_FAILED;
    struct stat st;

    /*
     * 临时降低控制台日志级别到 KERN_ALERT，
     * 防止模块加载时的内核消息破坏 curses 文本界面的显示。
     * 保存原始级别以便后续恢复。
     */
    loglevel_set = set_loglevel(PRINTK_LOGLEVEL_KERN_ALERT, &old_loglevel);

    /* 以只读方式打开模块文件 */
    fd = open(module, O_RDONLY);
    if (fd < 0) {
        goto done;
    }

    /* 获取文件大小 */
    if (fstat(fd, &st) != 0) {
        goto done;
    }

    /* 将模块文件映射到内存（init_module 需要内存中的模块映像） */
    buf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED) {
        goto done;
    }

    /*
     * 调用 init_module 系统调用加载模块。
     * 参数：模块映像地址、映像大小、模块参数字符串。
     */
    ret = syscall(SYS_init_module, buf, st.st_size, module_opts);
    if (ret != 0) {
        ret = errno;
    }

done:

    if (buf != MAP_FAILED) {
        /* 此时 st 一定已被 fstat 填充（mmap 成功说明 fstat 也成功了） */
        munmap(buf, st.st_size);
    }

    if (fd >= 0) {
        close(fd);
    }

    /* 恢复原始的 printk 日志级别 */
    if (loglevel_set) {
        set_loglevel(old_loglevel, NULL);
    }

    return ret;
}


/*
 * ignore_load_error() - 判断模块加载错误是否可以被忽略，并显示详细的错误信息。
 *
 * 当 do_insmod() 返回非零值时调用此函数。根据错误类型和系统配置
 * （Secure Boot 状态、CONFIG_MODULE_SIG_FORCE 等），决定是否允许
 * 用户忽略加载错误并继续安装。
 *
 * 主要处理的场景：
 * - ENOKEY：内核找不到验证模块签名的密钥（Secure Boot 典型错误）
 * - CONFIG_MODULE_SIG_FORCE：内核强制要求模块签名
 * - Secure Boot 启用：UEFI 安全启动要求模块签名
 *
 * 如果模块已签名但仍然加载失败，通常是因为签名密钥不被内核信任，
 * 用户可以选择仍然安装已签名的模块（重启后导入密钥即可加载）。
 *
 * 参数：
 *   op              - 全局选项结构体
 *   p               - 安装包结构体
 *   module_filename - 加载失败的模块文件名
 *   insmod_status   - do_insmod() 返回的 errno 值
 *
 * 返回值：
 *   TRUE  - 用户选择忽略加载错误
 *   FALSE - 用户选择中止安装，或不具备忽略错误的条件
 */

static int ignore_load_error(Options *op, Package *p,
                             const char *module_filename, int insmod_status)
{
    int ignore_error = FALSE, secureboot, module_sig_force, enokey;
    const char *probable_reason, *signature_related;
    char *error = strerror(insmod_status);

    /* 检测各种与模块签名相关的条件 */
    enokey = (insmod_status == ENOKEY);
    secureboot = (secure_boot_enabled() == 1);
    module_sig_force =
        (test_kernel_config_option(op, p, "CONFIG_MODULE_SIG_FORCE") ==
         KERNEL_CONFIG_OPTION_DEFINED);

    /* 根据不同情况构造错误消息中的"可能原因"描述 */
    if (enokey) {
        probable_reason = ",";
        signature_related = "";
    } else if (module_sig_force) {
        probable_reason = ". CONFIG_MODULE_SIG_FORCE is set on the target "
                          "kernel, so this is likely";
    } else if (secureboot) {
        probable_reason = ". Secure boot is enabled on this system, so "
                          "this is likely";
    } else {
        probable_reason = ", possibly";
    }

    if (!enokey) {
        signature_related = "if this module loading failure is due to the "
                            "lack of a trusted signature, ";
    }

    /*
     * 只有在签名相关的错误条件下，或在专家模式下，才允许用户选择忽略错误。
     */
    if (enokey || secureboot || module_sig_force || op->expert) {
        if (op->kernel_module_signed) {
            /*
             * 模块已签名但仍加载失败：密钥不被信任。
             * 允许用户选择安装已签名模块（之后可通过 MOK 等方式导入密钥）。
             */
            const char *choices[2] = {
                "Install signed kernel module",
                "Abort installation"
            };

            ignore_error = (ui_multiple_choice(op, choices, 2, 0,
                                               "The signed kernel module failed "
                                               "to load%s because the kernel "
                                               "does not trust any key which is "
                                               "capable of verifying the module "
                                               "signature. Would you like to "
                                               "install the signed kernel module "
                                               "anyway?\n\nNote that %syou "
                                               "will not be able to load the "
                                               "installed module until after a "
                                               "key that can verify the module "
                                               "signature is added to a key "
                                               "database that is trusted by the "
                                               "kernel. This will likely "
                                               "require rebooting your computer.",
                                               probable_reason,
                                               signature_related) == 0);
        } else {
            /*
             * 模块未签名：不允许忽略错误，只显示错误信息和修复建议。
             */
            const char *secureboot_message;

            secureboot_message = secureboot == 1 ?
                                     "and sign the kernel module when "
                                     "prompted to do so." :
                                     "and set the --module-signing-secret-"
                                     "key and --module-signing-public-key "
                                     "options on the command line, or run "
                                     "the installer in expert mode to "
                                     "enable the interactive module "
                                     "signing prompts.";

            ui_error(op, "The kernel module failed to load%s because it "
                     "was not signed by a key that is trusted by the "
                     "kernel. Please try installing the driver again, %s",
                     probable_reason, secureboot_message);
        }
    }

    if (ignore_error) {
        /* 用户选择忽略错误，记录日志但继续安装 */
        ui_log(op, "An error was encountered when loading the kernel "
               "module, but that error was ignored, and the kernel module "
               "will be installed, anyway. The error was: %s", error);
    } else {
        /* 加载失败，显示详细的诊断信息和日志文件位置 */
        ui_error(op, "Unable to load the kernel module '%s'.  This "
                 "happens most frequently when this kernel module was "
                 "built against the wrong or improperly configured "
                 "kernel sources, with a version of gcc that differs "
                 "from the one used to build the target kernel, or "
                 "if another driver, such as nouveau, is "
                 "present and prevents the NVIDIA kernel module from "
                 "obtaining ownership of the NVIDIA device(s), "
                 "or no NVIDIA device installed in this system is supported "
                 "by this NVIDIA Linux graphics driver release.\n\n"
                 "Please see the log entries 'Kernel module load "
                 "error' and 'Kernel messages' at the end of the file "
                 "'%s' for more information.",
                 module_filename, op->log_file_name);

        /*
         * 在专家模式下，run_command() 已经将输出写入日志文件；
         * 非专家模式下需要手动记录错误信息。
         */
        if (!op->expert) ui_log(op, "Kernel module load error: %s", error);
    }

    return ignore_error;
}


/*
 * unload_kernel_modules() - 尝试卸载包中所有的内核模块。
 *
 * 按照模块列表的逆序卸载，因为模块之间可能存在依赖关系
 * （后加载的模块通常依赖先加载的模块）。
 *
 * 即使卸载失败也不中止操作——内核可能未配置模块卸载支持
 * （CONFIG_MODULE_UNLOAD 未启用，在 Linux 2.6 中较常见）。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 */

static void unload_kernel_modules(Options *op, Package *p) {
    int i;

    for (i = p->num_kernel_modules - 1; i >= 0; i--) {
        rmmod_kernel_module(op, p->kernel_modules[i].module_name);
    }
}

/*
 * toggle_udev_event_queue() - 启用或禁用 udev 事件队列。
 *
 * 在测试加载内核模块时，需要临时暂停 udev 事件队列，以防止
 * udev 规则自动加载其他内核模块（这会干扰测试过程）。
 *
 * 使用 `udevadm control --stop-exec-queue` 暂停队列，
 * 使用 `udevadm control --start-exec-queue` 恢复队列。
 *
 * 参数：
 *   op     - 全局选项结构体
 *   enable - TRUE=启用（恢复）队列，FALSE=禁用（暂停）队列
 */

static void toggle_udev_event_queue(Options *op, int enable)
{
    const char *verb  = enable ? "start" : "stop";
    char *udevadm = find_system_util("udevadm");
    static int already_warned = FALSE;

    if (udevadm) {
        /*
         * 禁用队列时需要等待 udevadm 完成操作。
         * 重新启用队列时，由于需要处理积压的事件可能耗时数秒，
         * 所以设置 timeout=0 让 udevadm 立即返回。
         */
        const char *timeout = enable ? " --timeout=0" : NULL;
        char *data;
        int cmd_ret;

        cmd_ret = run_command(op, &data, FALSE, NULL, TRUE,
                              udevadm, " control --", verb, "-exec-queue", timeout,
                              NULL);
        nvfree(udevadm);

        if (cmd_ret != 0) {
            ui_warn(op, "Failed to %s the udev event queue:\n\n%s",
                    verb, data);
        }
        nvfree(data);
    } else if (!already_warned) {
        ui_warn(op, "Failed to find udevadm(8); nvidia-installer will not "
                "be able to %s the udev event queue.", verb);
        already_warned = TRUE;
    }
}

/*
 * log_dmesg() - 记录内核环形缓冲区的最近几行日志。
 *
 * 用于模块加载失败后提供诊断信息，或者捕获 NVRM（NVIDIA 内核模块）
 * 可能输出的警告消息。
 *
 * 执行 `dmesg | tail -n 25` 获取最近 25 行内核消息。
 *
 * 参数：
 *   op - 全局选项结构体
 */
static void log_dmesg(Options *op)
{
    char *data = NULL;

    if (!run_command(op, &data, FALSE, NULL, TRUE,
                     op->utils[DMESG], " | ", op->utils[TAIL], " -n 25", NULL)) {
        ui_log(op, "Kernel messages:\n%s", data);
    }

    nvfree(data);
}

/*
 * test_kernel_modules_helper() - 测试加载内核模块的内部实现。
 *
 * 此函数执行内核模块的完整测试加载流程：
 * 1. 可选：暂停 udev 事件队列（防止模块被自动加载）
 * 2. 加载 NVIDIA 模块的依赖项（DRM、i2c、vfio 等）
 * 3. 卸载可能已存在的 NVIDIA 模块
 * 4. 按顺序逐一加载每个 NVIDIA 内核模块
 * 5. 检查内核模块的警告消息
 * 6. 可选：恢复 udev 事件队列
 * 7. 卸载所有测试加载的模块
 * 8. 记录 dmesg 日志
 * 9. 卸载之前加载的依赖模块
 *
 * 参数：
 *   op         - 全局选项结构体
 *   p          - 安装包结构体
 *   pause_udev - 是否暂停 udev 事件队列
 *
 * 返回值：
 *   do_insmod() 的返回值：
 *   0      - 所有模块加载成功
 *   EEXIST - 某个模块已经加载（需要暂停 udev 后重试）
 *   其他   - 加载失败的 errno 值
 */
static int test_kernel_modules_helper(Options *op, Package *p, int pause_udev)
{
    int insmod_ret = -1, i;

    /*
     * NVIDIA 内核模块可能依赖的内核模块列表。
     * 在测试加载前需要先确保这些依赖模块已加载。
     * 包括 DRM 子系统、i2c 总线、vfio 虚拟化框架等。
     */
    const char *depmods[] = {
        "i2c-core",         /* I2C 总线核心 */
        "drm",              /* Direct Rendering Manager */
        "drm-kms-helper",   /* DRM 内核模式设置辅助 */
        "drm-ttm-helper",   /* DRM TTM（Translation Table Maps）辅助 */
        "drm_client_lib",   /* DRM 客户端库 */
        "vfio_mdev",        /* vfio 中介设备 */
        "vfio",             /* 虚拟功能IO框架 */
        "mdev",             /* 中介设备框架 */
        "video",            /* ACPI 视频总线 */
        "backlight",        /* 背光控制 */
        "vfio_pci_core",    /* vfio PCI 核心 */
        "ecc",              /* ECC（错误纠正码）支持 */
        "nvgrace-egm",      /* NVIDIA Grace EGM（扩展GPU内存）支持 */
        // Tegra 平台相关的模块
        "tsecriscv",        /* Tegra 安全引擎 RISC-V */
        "tegra_dce",        /* Tegra 显示控制器引擎 */
        "host1x_nvhost",    /* Tegra Host1x 子系统 */
        "mc_utils",         /* 内存控制器工具 */
    };

    if (pause_udev) {
        /* 暂停 udev 事件队列，防止 udev 规则自动加载模块 */
        toggle_udev_event_queue(op, FALSE);
    }

    /*
     * 尝试加载 NVIDIA 模块的依赖项。
     * 对于已加载的模块，将其在数组中标记为 NULL，
     * 以避免后续误卸载用户原本就在使用的模块。
     */
    for (i = 0; i < ARRAY_LEN(depmods); i++) {
        if (check_for_loaded_kernel_module(op, depmods[i])) {
            /* 模块已加载：标记为 NULL，后续不卸载 */
            depmods[i] = NULL;
        } else {
            /*
             * 静默加载，忽略失败。
             * 即使依赖模块加载失败，NVIDIA 模块也可能正常加载
             * （例如相关代码已编译进内核而非模块形式）。
             */
            load_kernel_module_quiet(op, depmods[i]);
        }
    }

    /*
     * 在初次卸载尝试和此刻之间，可能有其他 NVIDIA 模块被加载
     * （例如被 udev 规则自动加载）。再次尝试卸载以确保干净状态。
     */
    unload_kernel_modules(op, p);

    /*
     * 按包清单中的顺序逐一加载每个内核模块。
     * 包清单的顺序已设定为满足模块间的依赖关系。
     */
    for (i = 0; i < p->num_kernel_modules; i++) {
        const char *module_opts = "";
        char *module_path;

        /*
         * 跳过 nvidia-peermem：它依赖树外内核模块（如 Mellanox OFED），
         * 在测试环境中无法可靠加载。
         */
        if (strcmp(p->kernel_modules[i].module_name, "nvidia-peermem") == 0) {
            continue;
        }

        module_path = nvstrcat(p->kernel_module_build_directory, "/",
                               p->kernel_modules[i].module_filename,
                               NULL);

        /*
         * 对于主 nvidia 模块，使用特殊参数禁用设备文件管理，
         * 避免测试加载时创建 /dev/nvidia* 设备节点。
         */
        if (strcmp(p->kernel_modules[i].module_name, "nvidia") == 0) {
            module_opts = "NVreg_DeviceFileUID=0 NVreg_DeviceFileGID=0 "
                          "NVreg_DeviceFileMode=0 NVreg_ModifyDeviceFiles=0";
        }

        insmod_ret = do_insmod(op, module_path, module_opts);
        nvfree(module_path);

        if (insmod_ret == EEXIST) {
            /* 模块已加载：将此错误传播回 test_kernel_modules() 处理 */
            break;
        } else if (insmod_ret != 0) {
            /* 加载失败：询问用户是否忽略错误 */
            const char *name = p->kernel_modules[i].module_filename;
            int ignore = ignore_load_error(op, p, name, insmod_ret);

            if (ignore) {
                op->skip_module_load = TRUE;
                ui_log(op, "Ignoring failure to load %s.", name);
                insmod_ret = 0;
            } else {
                handle_optional_module_failure(op, p->kernel_modules[i],
                                               "load");
            }

            break;
        }
    }

    /* 检查内核模块是否通过 /proc 接口注册了警告消息 */
    check_for_warning_messages(op);

    if (pause_udev) {
        /*
         * 恢复 udev 事件队列。在卸载模块之前恢复，
         * 这样 udev 只需处理加载模块时积压的事件，
         * 而不需要额外处理卸载模块触发的事件。
         */
        toggle_udev_event_queue(op, TRUE);
    }

    /* 卸载测试加载的 NVIDIA 模块 */
    unload_kernel_modules(op, p);

    /* 记录 dmesg 日志以供诊断 */
    log_dmesg(op);

    /* 卸载之前加载的依赖模块（只卸载本函数加载的，跳过原本就在的） */
    for (i = 0; i < ARRAY_LEN(depmods); i++) {
        if (depmods[i]) {
            modprobe_remove_kernel_module_quiet(op, depmods[i]);
        }
    }

    return insmod_ret;
}



/*
 * test_kernel_modules() - 测试内核模块是否可以正常加载。
 *
 * 通过 insmod 加载所有模块，然后 rmmod 卸载它们。这是安装过程中的
 * 重要验证步骤，确保编译出的模块能被当前内核接受。
 *
 * 如果首次尝试时发现模块已加载（EEXIST），说明可能是 udev 规则
 * 在测试期间自动加载了模块。此时会暂停 udev 事件队列后重试。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 返回值：
 *   TRUE  - 所有模块加载测试成功（或跳过了模块加载测试）
 *   FALSE - 模块加载测试失败
 */

int test_kernel_modules(Options *op, Package *p)
{
    int ret;

    /* 如果设置了跳过模块加载（例如为非运行中内核编译），直接返回成功 */
    if (op->skip_module_load) {
        return TRUE;
    }

    /* 首先不暂停 udev 进行测试 */
    ret = test_kernel_modules_helper(op, p, FALSE);

    if (ret == EEXIST) {
        /*
         * 模块已被加载（可能是 udev 规则自动加载的）。
         * 暂停 udev 事件队列后重新尝试。
         */
        ui_log(op, "One or more kernel modules were already loaded before "
               "the module test load; trying again with the udev event "
               "queue paused.");

        ret = test_kernel_modules_helper(op, p, TRUE);
    }

    return ret == 0;
}



/*
 * modprobe_helper() - modprobe 命令的内部封装。
 *
 * 统一封装了 modprobe 的加载和卸载操作，被多个公共函数调用。
 *
 * 注意：与 rmmod 不同，`modprobe -r` 会自动处理模块依赖关系，
 * 先卸载依赖的模块再卸载目标模块。
 *
 * 参数：
 *   op          - 全局选项结构体
 *   module_name - 要操作的内核模块名称
 *   quiet       - 是否静默模式（TRUE=不显示错误信息）
 *   unload      - 操作类型（TRUE=卸载模块 modprobe -r，FALSE=加载模块）
 *
 * 返回值：
 *   TRUE  - modprobe 命令执行成功
 *   FALSE - modprobe 命令执行失败
 */

static int modprobe_helper(Options *op, const char *module_name,
                           int quiet, int unload)
{
    int ret = 0, old_loglevel, loglevel_set;
    char *data;

    /* 如果设置了跳过模块加载，直接返回成功 */
    if (op->skip_module_load) {
        return TRUE;
    }

    /* 临时降低 printk 日志级别以避免干扰用户界面 */
    loglevel_set = set_loglevel(PRINTK_LOGLEVEL_KERN_ALERT, &old_loglevel);

    /* 执行 modprobe 命令 */
    ret = run_command(op, &data, FALSE, NULL, TRUE,
                      op->utils[MODPROBE],
                      quiet ? " -q" : "",       /* -q 静默模式 */
                      unload ? " -r" : "",      /* -r 卸载模式 */
                      " ", module_name,
                      NULL);

    /* 恢复原始日志级别 */
    if (loglevel_set) {
        set_loglevel(old_loglevel, NULL);
    }

    /* 非静默模式下，如果命令失败则显示错误信息 */
    if (!quiet && ret != 0) {
        char *expert_detail = nvstrcat(": '", data, "'", NULL);
        ui_error(op, "Unable to %s the '%s' kernel module%s",
                 unload ? "unload" : "load", module_name,
                 op->expert? expert_detail : ".");
        nvfree(expert_detail);

	log_dmesg(op);
    }

    nvfree(data);

    return ret == 0;
}

/*
 * load_kernel_module() - 使用 modprobe 加载指定的内核模块。
 *
 * 这是安装完成后加载 NVIDIA 内核模块的公共接口。
 * 加载失败时会显示错误信息。
 *
 * 参数：
 *   op          - 全局选项结构体
 *   module_name - 要加载的模块名称
 *
 * 返回值：
 *   TRUE=加载成功，FALSE=加载失败
 */
int load_kernel_module(Options *op, const char *module_name)
{
    return modprobe_helper(op, module_name, FALSE, FALSE);
}

/*
 * load_kernel_module_quiet() - 静默加载内核模块。
 *
 * 用于加载 NVIDIA 模块的依赖项（如 drm、i2c-core），
 * 加载失败时不显示错误信息（因为依赖项可能编译进内核而非模块形式）。
 */
static void load_kernel_module_quiet(Options *op, const char *module_name)
{
    modprobe_helper(op, module_name, TRUE, FALSE);
}

/*
 * modprobe_remove_kernel_module_quiet() - 静默使用 modprobe -r 卸载模块。
 *
 * 用于清理测试加载过程中加载的依赖模块。
 * modprobe -r 会自动处理依赖关系。
 */
static void modprobe_remove_kernel_module_quiet(Options *op, const char *name)
{
    modprobe_helper(op, name, TRUE, TRUE);
}



/*
 * check_for_unloaded_kernel_module() - 检查是否有冲突的内核模块已加载，
 * 如果有则尝试卸载。
 *
 * 安装新的 NVIDIA 驱动前，需要确保旧的 NVIDIA 内核模块（以及其他冲突模块
 * 如 nouveau）已被卸载。冲突模块列表定义在 conflicting-kernel-modules.h 中。
 *
 * 处理流程：
 * 1. 检查是否可以跳过此检测（非运行中内核、不安装内核模块等）
 * 2. 通过 lsmod 检查冲突模块列表中是否有已加载的模块
 * 3. 对已加载的冲突模块尝试 rmmod 卸载
 * 4. 如果无法卸载（模块正在使用），提示用户选择继续或中止
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   TRUE  - 没有冲突模块，或冲突模块已成功卸载，或用户选择继续
 *   FALSE - 用户选择中止安装
 */

int check_for_unloaded_kernel_module(Options *op)
{
    int n;
    int loaded = FALSE;
    unsigned long long int bits = 0;   /* 位图：记录哪些冲突模块已加载 */

    /*
     * 如果只安装非运行中内核的模块，可以跳过此检查
     * （运行中内核的模块不受影响）。
     */
    if (op->kernel_modules_only && op->kernel_name) {
        ui_log(op, "Only installing kernel modules for a non-running "
               "kernel; skipping the \"is an NVIDIA kernel module loaded?\" "
               "test.");
        return TRUE;
    }

    /* 如果不安装任何内核模块，也可以跳过 */
    if (op->no_kernel_modules) {
        ui_log(op, "Not installing any kernel modules; skipping the \"is an "
               "NVIDIA kernel module loaded?\" test.");
        return TRUE;
    }

    /* 检查所有冲突模块是否已加载，用位图记录结果 */
    for (n = 0; n < num_conflicting_kernel_modules; n++) {
        if (check_for_loaded_kernel_module(op, conflicting_kernel_modules[n])) {
            loaded = TRUE;
            bits |= (1 << n);
        }
    }

    if (!loaded) return TRUE;

    /* 有冲突模块已加载，逐一尝试卸载 */
    for (n = 0; n < num_conflicting_kernel_modules; n++) {
        if (!(bits & (1 << n))) {
            continue;   /* 该模块未加载，跳过 */
        }

        /* 尝试 rmmod 卸载 */
        rmmod_kernel_module(op, conflicting_kernel_modules[n]);

        /* 卸载后再次检查模块是否仍然加载 */
        if (check_for_loaded_kernel_module(op, conflicting_kernel_modules[n])) {
            int choice;

            op->loaded_kernel_module_detected = TRUE;

            /* 模块仍在运行（可能被 X Server、CUDA 程序等占用），警告用户 */
            ui_warn(op, "An NVIDIA kernel module '%s' appears to be already "
                "loaded in your kernel.  This may be because it is in use (for "
                "example, by an X server, a CUDA program, or the NVIDIA "
                "Persistence Daemon), but this may also happen if your kernel "
                "was configured without support for module unloading.  Some of "
                "the sanity checks that nvidia-installer performs to detect "
                "potential installation problems are not possible while an "
                "NVIDIA kernel module is running.",
                conflicting_kernel_modules[n]);

            /* 让用户选择继续安装还是中止 */
            choice = ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                NUM_CONTINUE_ABORT_CHOICES,
                op->allow_installation_with_running_driver ?
                CONTINUE_CHOICE : ABORT_CHOICE,
                "Would you like to continue installation and skip the sanity "
                "checks? If not, please abort the installation, then close "
                "any programs which may be using the NVIDIA GPU(s), and "
                "attempt installation again.");

            if (choice == CONTINUE_CHOICE) {
                /* 用户选择继续，跳过后续的模块加载测试 */
                ui_warn(op, "Continuing installation despite the presence of a "
                    "loaded NVIDIA kernel module.  Some sanity checks will not "
                    "be performed.  It is strongly recommended that you reboot "
                    "your computer after installation is complete.  If the "
                    "installation is not successful after rebooting the "
                    "computer, you can run `nvidia-uninstall` to attempt to "
                    "remove the NVIDIA driver.");

                op->skip_module_load = TRUE;
                ui_log(op, "Kernel module load tests will be skipped.");
            }

            return choice == CONTINUE_CHOICE;
        }
    }

    return TRUE;

}


/*
 * precompiled_kernel_interface_path() - 获取预编译内核接口文件的搜索路径。
 *
 * 返回安装包中预编译接口文件的目录路径：
 *   <kernel_module_build_directory>/precompiled/
 *
 * 参数：
 *   p - 安装包结构体
 *
 * 返回值：
 *   动态分配的路径字符串（调用者负责释放）
 */
char *precompiled_kernel_interface_path(const Package *p)
{
    return nvdircat(p->kernel_module_build_directory, "precompiled", NULL);
}


/*
 * find_precompiled_kernel_interface() - 查找与当前运行内核匹配的预编译内核接口。
 *
 * 预编译内核接口可以避免在目标系统上编译内核模块，大大加快安装速度。
 * 匹配基于 /proc/version 字符串（包含内核版本、编译器版本、编译日期等），
 * 确保预编译接口与目标内核完全兼容。
 *
 * 搜索顺序（优先级从高到低）：
 * 1. --precompiled-kernel-interfaces-path 命令行参数指定的目录
 * 2. 发行版提供的预编译接口目录（/lib/modules/precompiled/<uname -r>/nvidia/gfx/）
 * 3. 安装包中自带的预编译接口目录（<build_dir>/precompiled/）
 *
 * TODO（来自原始注释）：扩展此功能以支持为非运行中内核安装预编译接口。
 *
 * 参数：
 *   op - 全局选项结构体
 *   p  - 安装包结构体
 *
 * 返回值：
 *   成功：返回匹配的 PrecompiledInfo 结构体指针（调用者负责释放）
 *   失败：返回 NULL（未找到匹配的预编译接口）
 */

PrecompiledInfo *find_precompiled_kernel_interface(Options *op, Package *p)
{
    char *proc_version_string, *tmp;
    PrecompiledInfo *info = NULL;
    char **search_filelist = NULL;
    int i;

    /* 如果用户通过 --no-precompiled-interface 明确禁用了预编译接口，直接返回 */
    if (op->no_precompiled_interface) {
        ui_log(op, "Not probing for precompiled kernel interfaces.");
        return NULL;
    }

    /* 读取 /proc/version 获取当前运行内核的版本字符串（用于匹配预编译接口） */
    proc_version_string = read_proc_version(op, op->proc_mount_point);

    if (!proc_version_string) goto done;

    /* 确保模块构建目录存在 */
    if (!mkdir_recursive(op, p->kernel_module_build_directory, 0755, FALSE))
        goto done;

    /*
     * 构建搜索文件列表：对每个内核模块，确定需要查找的预编译文件名。
     * 如果模块有独立的接口文件，搜索接口文件名；否则搜索模块文件名。
     */
    search_filelist = nvalloc((p->num_kernel_modules + 1) * sizeof(char*));

    for (i = 0; i < p->num_kernel_modules; i++) {
        if (p->kernel_modules[i].has_separate_interface_file) {
            search_filelist[i] = p->kernel_modules[i].interface_filename;
        } else {
            search_filelist[i] = p->kernel_modules[i].module_filename;
        }
    }

    /* 搜索优先级1：用户通过命令行指定的预编译接口路径 */
    if (op->precompiled_kernel_interfaces_path) {
        info = scan_dir(op, p, op->precompiled_kernel_interfaces_path,
                        proc_version_string, search_filelist);
    }

    /* 搜索优先级2：发行版提供的预编译接口目录 */
    if (!info) {
        tmp = build_distro_precompiled_kernel_interface_dir(op);
        if (tmp) {
            info = scan_dir(op, p, tmp, proc_version_string, search_filelist);
            nvfree(tmp);
        }
    }

    /* 搜索优先级3：安装包中自带的预编译接口目录 */
    if (!info) {
        char *dir = precompiled_kernel_interface_path(p);
        info = scan_dir(op, p, dir, proc_version_string, search_filelist);
        nvfree(dir);
    }

    /* 专家模式下，即使找到预编译接口也让用户确认是否使用 */
    if (info && op->expert) {
        const char *choices[2] = {
            "Use the precompiled interface",
            "Compile the interface"
        };

        if (ui_multiple_choice(op, choices, 2, 0, "A precompiled kernel "
                               "interface for the kernel '%s' has been found.  "
                               "Would you like use the precompiled interface, "
                               "or would you like to compile the interface "
                               "instead?", info->description) == 1) {
            free_precompiled(info);
            info = NULL;
        }
    }

 done:

    nvfree(search_filelist);

    nvfree(proc_version_string);

    /* 未找到预编译接口时，在专家模式下通知用户需要编译 */
    if (!info && op->expert) {
        ui_message(op, "No precompiled kernel interface was found to match "
                   "your kernel; this means that the installer will need to "
                   "compile a new kernel interface.");
    }

    return info;
}



/*
 * get_kernel_name() - 获取目标内核的版本名称。
 *
 * 返回的版本名称用于构建内核模块路径等。
 *
 * 优先级：
 * 1. 如果用户通过 --kernel-name 指定了内核名称，使用该值
 *    （同时标记 skip_module_load，因为不能在非运行中的内核上测试加载）
 * 2. 否则通过 uname -r 获取当前运行内核的版本
 *
 * 注意：使用静态缓冲区缓存 uname 结果，避免重复调用系统调用。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   成功：返回内核版本名称字符串（如 "5.15.0-52-generic"）
 *   失败：返回 NULL（uname 调用失败且未指定 --kernel-name）
 */

char *get_kernel_name(Options *op)
{
    static char kernel_name[256];    /* 静态缓冲区，缓存 uname 结果 */
    struct utsname uname_buf;

    /* 首次调用时通过 uname 获取运行中内核的版本 */
    if (!kernel_name[0]) {
        if (uname(&uname_buf) == -1) {
            static int uname_failed;

            /* 只警告一次，避免重复输出 */
            if (!uname_failed) {
                ui_warn(op, "Unable to determine the version of the running "
                            "kernel (%s).", strerror(errno));
                uname_failed = TRUE;
            }
        } else {
            strncpy(kernel_name, uname_buf.release, sizeof(kernel_name) - 1);
        }
    }

    /* 如果用户通过命令行指定了目标内核名称 */
    if (op->kernel_name) {
        if (strcmp(op->kernel_name, kernel_name) != 0) {
            /* 目标内核与运行中内核不同，跳过模块加载测试 */
            op->skip_module_load = TRUE;
        }
        return op->kernel_name;
    }

    /* 返回 uname 获取的运行中内核版本 */
    if (kernel_name[0]) {
        return kernel_name;
    }

    return NULL;
} /* get_kernel_name() */



/*
 * test_kernel_config_option() - 检测目标内核配置中是否定义了指定选项。
 *
 * 通过 conftest.sh 的 test_configuration_option 功能检查内核的 .config
 * 文件中是否设置了指定的配置选项（如 CONFIG_MODULE_SIG_FORCE）。
 *
 * 参数：
 *   op     - 全局选项结构体
 *   p      - 安装包结构体
 *   option - 要检查的配置选项名称（如 "CONFIG_MODULE_SIG_FORCE"）
 *
 * 返回值：
 *   KERNEL_CONFIG_OPTION_DEFINED     - 选项已定义（=y 或 =m）
 *   KERNEL_CONFIG_OPTION_NOT_DEFINED - 选项未定义
 *   KERNEL_CONFIG_OPTION_UNKNOWN     - 无法确定（内核源码/输出路径未设置）
 */

KernelConfigOptionStatus test_kernel_config_option(Options* op, Package *p,
                                                   const char *option)
{
    if (op->kernel_source_path && op->kernel_output_path) {
        int ret;
        char *conftest_cmd;

        conftest_cmd = nvstrcat("test_configuration_option ", option, NULL);
        ret = run_conftest(op, p->kernel_module_build_directory, conftest_cmd,
                           NULL);
        nvfree(conftest_cmd);

        return ret ? KERNEL_CONFIG_OPTION_DEFINED :
                     KERNEL_CONFIG_OPTION_NOT_DEFINED;
    }

    return KERNEL_CONFIG_OPTION_UNKNOWN;
}



/*
 * guess_module_signing_hash() - 猜测内核模块签名使用的哈希算法。
 *
 * 通过 conftest.sh 的 guess_module_signing_hash 功能，检查内核配置
 * 中的 CONFIG_MODULE_SIG_HASH 选项来确定签名哈希算法。
 * 常见的返回值包括 "sha256"、"sha512" 等。
 *
 * 参数：
 *   op              - 全局选项结构体
 *   build_directory - 内核模块构建目录（conftest.sh 所在位置）
 *
 * 返回值：
 *   成功：返回哈希算法名称字符串（如 "sha256"，调用者负责释放）
 *   失败：返回 NULL（无法确定哈希算法）
 */

char *guess_module_signing_hash(Options *op, const char *build_directory)
{
    char *ret;

    if (run_conftest(op, build_directory,
                     "guess_module_signing_hash", &ret)) {
        return ret;
    }

    return NULL;
}

/*
 ***************************************************************************
 * 以下为本文件内部使用的静态辅助函数
 ***************************************************************************
 */



/*
 * default_kernel_module_installation_path() - 确定默认的内核模块安装路径。
 *
 * 逻辑等价于以下 Makefile 规则：
 *
 *   SYSSRC = /lib/modules/$(shell uname -r)
 *   ifeq ($(shell if test -d $(SYSSRC)/kernel; then echo yes; fi),yes)
 *     INSTALLDIR = $(SYSSRC)/kernel/drivers/video
 *   else
 *     INSTALLDIR = $(SYSSRC)/video
 *   endif
 *
 * 即：如果存在 /lib/modules/<ver>/kernel/ 目录（标准内核布局），
 * 则安装到 /lib/modules/<ver>/kernel/drivers/video/；
 * 否则安装到 /lib/modules/<ver>/video/（旧式布局）。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   动态分配的路径字符串，或 NULL（获取内核名称失败）
 */

static char *default_kernel_module_installation_path(Options *op)
{
    char *str, *tmp;

    tmp = get_kernel_name(op);
    if (!tmp) return NULL;

    /* 检查标准内核目录布局 */
    str = nvstrcat("/lib/modules/", tmp, "/kernel", NULL);

    if (directory_exists(str)) {
        free(str);
        /* 标准布局：安装到 kernel/drivers/video 子目录 */
        str = nvstrcat("/lib/modules/", tmp, "/kernel/drivers/video", NULL);
        return str;
    }

    free(str);

    /* 旧式布局：安装到 video 子目录 */
    str = nvstrcat("/lib/modules/", tmp, "/video", NULL);

    return str;

} /* default_kernel_module_installation_path() */



/*
 * default_kernel_source_path() - 确定默认的内核源码路径。
 *
 * 如果无法找到默认路径则返回 NULL。
 *
 * 搜索逻辑（优先级从高到低）：
 *
 * 1. --kernel-source-path 命令行参数（不检查目录是否存在——
 *    用户明确指定的路径，如果不存在应在 determine_kernel_source_path() 中报错）
 *
 * 2. --kernel-include-path 命令行参数（已弃用，自动转换为源码路径）
 *
 * 3. SYSSRC 环境变量（同样不检查存在性）
 *
 * 4. /lib/modules/`uname -r`/source（如果存在）
 *
 * 5. /lib/modules/`uname -r`/build/source（如果存在）
 *
 * 6. /lib/modules/`uname -r`/build（如果存在）
 *    — 这是最常见的路径，大多数发行版使用此符号链接
 *
 * 7. /usr/src/linux-`uname -r`（如果存在）
 *    — 某些发行版使用此命名规则
 *
 * 8. /usr/src/linux（如果存在）
 *    — 传统 Linux 源码路径
 *
 * 注意：对于用户明确指定的路径（1-3），不检查目录是否存在后就返回，
 * 让 determine_kernel_source_path() 给出更合适的错误信息。
 * 对于自动探测的路径（4-8），只在目录存在时才返回。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   找到的源码路径字符串，或 NULL
 */

static char *default_kernel_source_path(Options *op)
{
    char *str, *tmp;

    str = tmp = NULL;

    /* 优先级1：--kernel-source-path */
    if (op->kernel_source_path) {
        ui_log(op, "Using the kernel source path '%s' as specified by the "
               "'--kernel-source-path' commandline option.",
               op->kernel_source_path);
        return op->kernel_source_path;
    }

    /* 优先级2：--kernel-include-path（已弃用） */
    if (op->kernel_include_path) {
        ui_warn(op, "The \"--kernel-include-path\" option is deprecated "
                "(as part of reorganization to support Linux 2.6); please use "
                "\"--kernel-source-path\" instead.");
        /* 将 include 路径转换为源码路径（去掉末尾的 include 部分） */
        str = convert_include_path_to_source_path(op->kernel_include_path);
        ui_log(op, "Using the kernel source path '%s' (inferred from the "
               "'--kernel-include-path' commandline option '%s').",
               str, op->kernel_include_path);
        return str;
    }

    /* 优先级3：SYSSRC 环境变量 */
    str = getenv("SYSSRC");
    if (str) {
        ui_log(op, "Using the kernel source path '%s', as specified by the "
               "SYSSRC environment variable.", str);
        return str;
    }

    /* 优先级4-7：基于 `uname -r` 探测常见路径 */
    tmp = get_kernel_name(op);

    if (tmp) {
        /* 优先级4：/lib/modules/<ver>/source */
        str = nvstrcat("/lib/modules/", tmp, "/source", NULL);

        if (directory_exists(str)) {
            return str;
        }

        nvfree(str);

        /* 优先级5：/lib/modules/<ver>/build/source */
        str = nvstrcat("/lib/modules/", tmp, "/build/source", NULL);

        if (directory_exists(str)) {
            return str;
        }

        nvfree(str);

        /* 优先级6：/lib/modules/<ver>/build（最常见的路径） */
        str = nvstrcat("/lib/modules/", tmp, "/build", NULL);

        if (directory_exists(str)) {
            return str;
        }

        nvfree(str);

        /*
         * 优先级7：/usr/src/linux-<ver>
         * 此路径探测由 Peter Berg Larsen <pebl@math.ku.dk> 建议添加。
         */
        str = nvstrcat("/usr/src/linux-", tmp, NULL);
        if (directory_exists(str)) {
            return str;
        }

        free(str);
    }

    /* 优先级8：/usr/src/linux（传统路径） */
    if (directory_exists("/usr/src/linux")) {
        return "/usr/src/linux";
    }

    return NULL;

} /* default_kernel_source_path() */


/*
 * find_module_substring() - 在字符串中查找模块名子串，忽略连字符和下划线的差异。
 *
 * Linux 内核中，模块名称中的连字符（-）和下划线（_）是等价的。
 * 例如 "nvidia-drm" 和 "nvidia_drm" 指的是同一个模块。
 * 此函数在 lsmod 输出中搜索模块名时考虑了这种等价关系。
 *
 * 参数：
 *   string    - 要搜索的主字符串（如 lsmod 的输出）
 *   substring - 要查找的子串（模块名称）
 *
 * 返回值：
 *   找到：返回指向子串在主字符串中起始位置的指针
 *   未找到或参数无效：返回 NULL
 */

static char *find_module_substring(char *string, const char *substring)
{
    int string_len, substring_len, len;
    char *tstr;
    const char *tsubstr;

    if ((string == NULL) || (substring == NULL))
        return NULL;

    string_len = strlen(string);
    substring_len = strlen(substring);

    /* 在主字符串中逐位置尝试匹配 */
    for (len = 0; len <= string_len - substring_len; len++, string++) {
        /* 快速跳过首字符不匹配的位置 */
        if (*string != *substring) {
            continue;
        }

        /* 逐字符比较，将连字符和下划线视为等价 */
        for (tstr = string, tsubstr = substring;
             *tsubstr != '\0';
             tstr++, tsubstr++) {
            if (*tstr != *tsubstr) {
                /* 连字符和下划线视为相同 */
                if (((*tstr == '-') || (*tstr == '_')) &&
                    ((*tsubstr == '-') || (*tsubstr == '_')))
                    continue;
                break;
            }
        }

        /* 如果子串已完全匹配，返回匹配位置 */
        if (*tsubstr == '\0')
            return string;
    }

    return NULL;
} /* find_module_substring */


/*
 * substring_is_isolated() - 检查子串是否是一个独立的单词（即两侧被空白符或
 * 字符串边界包围）。
 *
 * 用于 check_for_loaded_kernel_module() 中避免误匹配：例如在搜索 "nvidia"
 * 时不应匹配到 "nvidia_drm" 中的 "nvidia" 部分。
 *
 * 参数：
 *   substring - 指向主字符串中某个位置的指针（匹配到的子串起始位置）
 *   string    - 主字符串的起始指针（用于判断子串是否在字符串开头）
 *   len       - 子串的长度
 *
 * 返回值：
 *   TRUE  - 子串是独立的（两侧为空白或字符串边界）
 *   FALSE - 子串不是独立的（两侧有非空白字符）
 */

static int substring_is_isolated(const char *substring, const char *string,
                                 int len)
{
    /* 检查左侧：如果不在字符串开头，左侧字符必须是空白 */
    if (substring != string) {
        if (!isspace(substring[-1])) {
            return FALSE;
        }
    }

    /* 检查右侧：如果不在字符串末尾，右侧字符必须是空白 */
    if (substring[len] && !isspace(substring[len])) {
        return FALSE;
    }

    return TRUE;
}


/*
 * check_for_loaded_kernel_module() - 通过 lsmod 检查指定模块是否已加载。
 *
 * 在 lsmod 输出中搜索模块名，同时确保匹配到的是独立的模块名
 * （而非其他模块名的子串）。例如搜索 "nvidia" 时不应匹配到 "nvidia_drm"。
 *
 * 搜索时会忽略连字符和下划线的差异（通过 find_module_substring()）。
 *
 * 参数：
 *   op          - 全局选项结构体
 *   module_name - 要检查的模块名称
 *
 * 返回值：
 *   TRUE  - 模块已加载
 *   FALSE - 模块未加载（或 lsmod 执行失败）
 */

static int check_for_loaded_kernel_module(Options *op, const char *module_name)
{
    char *result = NULL;
    int ret, found = FALSE;

    /* 执行 lsmod 获取已加载模块列表 */
    ret = run_command(op, &result, FALSE, NULL, TRUE, op->utils[LSMOD], NULL);

    if ((ret == 0) && (result) && (result[0] != '\0')) {
        char *ptr;
        int len = strlen(module_name);

        /* 在 lsmod 输出中查找模块名，确保是独立单词而非子串 */
        for (ptr = result;
             (ptr = find_module_substring(ptr, module_name));
             ptr += len) {
            if (substring_is_isolated(ptr, result, len)) {
                found = TRUE;
                break;
            }
        }
    }

    if (result) free(result);

    return found;

} /* check_for_loaded_kernel_module() */


/*
 * rmmod_kernel_module() - 使用 rmmod 命令卸载指定的内核模块。
 *
 * 注意：与 modprobe -r 不同，rmmod 不会自动处理模块依赖关系。
 * 如果模块仍被其他模块依赖或正在使用，卸载会失败。
 *
 * 卸载前临时降低 printk 日志级别以避免干扰用户界面。
 *
 * 参数：
 *   op          - 全局选项结构体
 *   module_name - 要卸载的模块名称
 *
 * 返回值：
 *   TRUE  - 卸载成功
 *   FALSE - 卸载失败
 */

int rmmod_kernel_module(Options *op, const char *module_name)
{
    int ret, old_loglevel, loglevel_set;

    loglevel_set = set_loglevel(PRINTK_LOGLEVEL_KERN_ALERT, &old_loglevel);

    ret = run_command(op, NULL, FALSE, NULL, TRUE,
                      op->utils[RMMOD], " ", module_name, NULL);

    if (loglevel_set) {
        set_loglevel(old_loglevel, NULL);
    }

    return ret ? FALSE : TRUE;

} /* rmmod_kernel_module() */



/*
 * conftest_sanity_check() - 运行指定的 conftest 健全性检查。
 *
 * 在编译内核模块之前运行一系列预检查，确保编译环境满足要求。
 * 如果检查失败，显示 conftest.sh 输出的错误信息。
 *
 * "just_msg" 参数告诉 conftest.sh 只输出消息而不执行其他操作。
 *
 * 参数：
 *   op                 - 全局选项结构体
 *   dir                - conftest.sh 所在的目录
 *   sanity_check_name  - 检查的显示名称（如 "Compiler"），用于日志
 *   conftest_name      - conftest.sh 中的检测函数名（如 "cc_sanity_check"）
 *
 * 返回值：
 *   TRUE  - 检查通过
 *   FALSE - 检查失败
 */

int conftest_sanity_check(Options *op, const char *dir,
                          const char *sanity_check_name,
                          const char *conftest_name)
{
    char *result, *conftest_args;
    int ret;

    ui_log(op, "Performing %s check.", sanity_check_name);

    /* 添加 "just_msg" 参数，让 conftest 只输出诊断信息 */
    conftest_args = nvstrcat(conftest_name, " just_msg", NULL);
    ret = run_conftest(op, dir, conftest_args, &result);
    nvfree(conftest_args);

    if (!ret && result) {
        ui_error(op, "The %s sanity check failed:\n\n%s",
                 sanity_check_name, result);
    }

    nvfree(result);
    return ret;
}



/*
 * scan_dir() - 扫描指定目录，查找与当前内核匹配的预编译内核接口文件。
 *
 * 遍历目录中的所有文件（跳过 "." 和 ".."），对每个文件调用
 * get_precompiled_info() 检查是否与当前内核的 /proc/version 字符串匹配。
 * 找到第一个匹配项后立即返回。
 *
 * 参数：
 *   op                  - 全局选项结构体
 *   p                   - 安装包结构体
 *   directory_name      - 要扫描的目录路径
 *   proc_version_string - 当前内核的 /proc/version 字符串（用于匹配）
 *   search_filelist     - 要搜索的文件名列表（接口文件名或模块文件名）
 *
 * 返回值：
 *   找到匹配：返回 PrecompiledInfo 结构体指针（调用者负责释放）
 *   未找到或目录不存在：返回 NULL
 */

static PrecompiledInfo *scan_dir(Options *op, Package *p,
                                 const char *directory_name,
                                 const char *proc_version_string,
                                 char *const *search_filelist)
{
    DIR *dir;
    struct dirent *ent;
    PrecompiledInfo *info = NULL;
    char *filename;

    if (!directory_name) return NULL;

    dir = opendir(directory_name);
    if (!dir) return NULL;

    /* 遍历目录中的所有条目，查找匹配的预编译内核接口 */
    while ((ent = readdir(dir)) != NULL) {

        /* 跳过当前目录和父目录 */
        if (((strcmp(ent->d_name, ".")) == 0) ||
            ((strcmp(ent->d_name, "..")) == 0)) continue;

        filename = nvstrcat(directory_name, "/", ent->d_name, NULL);

        /* 检查该文件是否是与当前内核匹配的预编译接口 */
        info = get_precompiled_info(op, filename, proc_version_string,
                                    p->version, search_filelist);

        free(filename);

        /* 找到第一个匹配项后停止搜索 */
        if (info) break;
    }

    if (closedir(dir) != 0) {
        ui_error(op, "Failure while closing directory '%s' (%s).",
                 directory_name,
                 strerror(errno));
    }

    return info;

} /* scan_dir() */



/*
 * build_distro_precompiled_kernel_interface_dir() - 构建发行版预编译接口目录路径。
 *
 * 某些 Linux 发行版会在标准路径下提供预编译的 NVIDIA 内核接口。
 * 此函数构建该路径：
 *   /lib/modules/precompiled/<uname -r>/nvidia/gfx/
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   动态分配的路径字符串（调用者负责释放），或 NULL（uname 失败时）
 */

static char *build_distro_precompiled_kernel_interface_dir(Options *op)
{
    struct utsname uname_buf;
    char *str;

    if (uname(&uname_buf) == -1) {
        ui_error(op, "Unable to determine kernel version (%s)",
                 strerror(errno));
        return NULL;
    }

    str = nvstrcat("/lib/modules/precompiled/", uname_buf.release,
                   "/nvidia/gfx/", NULL);

    return str;

} /* build_distro_precompiled_kernel_interface_dir() */



/*
 * convert_include_path_to_source_path() - 将 --kernel-include-path 转换为
 * 内核源码路径。
 *
 * 转换方法：去掉路径末尾的最后一个目录组件。
 * 例如：/usr/src/linux/include -> /usr/src/linux
 *
 * 参数：
 *   inc - 输入的 include 路径
 *
 * 返回值：
 *   动态分配的源码路径字符串（调用者负责释放）
 */

static char *convert_include_path_to_source_path(const char *inc)
{
    char *c, *str;

    str = nvstrdup(inc);

    /* 定位到字符串末尾 */
    for (c = str; *c; c++);

    /* 移动到最后一个可打印字符 */
    c--;

    /* 如果字符串以 '/' 结尾，再往前退一位 */
    if (*c == '/') c--;

    /* 继续往前找到下一个 '/' */
    while ((c >= str) && (*c != '/')) c--;

    /* 在 '/' 处截断字符串，去掉最后一个路径组件 */
    if (*c == '/') *c = '\0';

    return str;

} /* convert_include_path_to_source_path() */


/*
 * get_machine_arch() - 获取当前机器的 CPU 架构。
 *
 * 通过 uname 系统调用获取机器架构（如 "x86_64"、"aarch64"、"ppc64le"）。
 * 结果缓存在静态变量中，避免重复系统调用。
 *
 * 返回值用于 conftest.sh 脚本参数和其他架构相关判断。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   成功：返回架构名称字符串（静态存储，不需要释放）
 *   失败：返回 NULL
 */

const char *get_machine_arch(Options *op)
{
    static struct utsname uname_buf;

    /* 如果已有缓存结果，直接返回 */
    if (uname_buf.machine[0]) {
        return uname_buf.machine;
    }

    if (uname(&uname_buf) == -1) {
        ui_warn(op, "Unable to determine machine architecture (%s).",
                strerror(errno));
        uname_buf.machine[0] = '\0';
        return NULL;
    } else {
        return uname_buf.machine;
    }
} /* get_machine_arch() */

/*
 * run_make() - 执行 make 命令编译内核模块。
 *
 * 构建完整的 make 命令行，包括：
 * - 切换到构建目录
 * - 使用 -k 选项（错误时继续编译其他目标）
 * - 使用 -j<N> 进行并行编译
 * - 传递内核源码路径（SYSSRC）和输出路径（SYSOUT）
 * - 传递排除模块列表（NV_EXCLUDE_KERNEL_MODULES）
 * - 传递调用者指定的额外选项
 *
 * 如果提供了 status 字符串，会显示进度条；
 * 进度条基于 match 参数中预期的输出行数来计算百分比。
 *
 * 所有 make 输出都会追加到 p->kernel_make_logs 中以供诊断。
 *
 * 参数：
 *   op          - 全局选项结构体
 *   p           - 安装包结构体
 *   dir         - 构建目录
 *   cli_options - 传递给 make 的额外选项（如 "clean"、模块名等）
 *   status      - 进度条显示的消息；NULL 则不显示进度条
 *   match       - 输出行匹配规则数组，用于进度条计算；status 为 NULL 时忽略
 *
 * 返回值：
 *   TRUE  - make 命令执行成功（返回码为 0）
 *   FALSE - make 命令执行失败
 */
static int run_make(Options *op, Package *p, const char *dir,
                    const char *cli_options, const char *status,
                    const RunCommandOutputMatch *match) {
    char *cmd, *concurrency, *data = NULL;
    int ret;

    /* 构建并行编译参数 -j<N> */
    concurrency = nvasprintf(" -j%d ", op->concurrency_level);

    /* 拼接完整的 make 命令 */
    cmd = nvstrcat("cd ", dir, "; ",
                   op->utils[MAKE], " -k", concurrency,
                   " NV_EXCLUDE_KERNEL_MODULES=\"",
                   p->excluded_kernel_modules, "\"",
                   " SYSSRC=\"", op->kernel_source_path, "\"",
                   " SYSOUT=\"", op->kernel_output_path, "\" ",
                   cli_options,
                   NULL);
    nvfree(concurrency);

    /* 如果需要显示进度条，开始进度条 */
    if (status) {
        ui_status_begin(op, status, "");
    }

    /* 执行 make 命令；如果有进度条，传入匹配规则用于计算进度 */
    ret = (run_command(op, &data, TRUE, status ? match : NULL, TRUE, cmd, NULL) == 0);

    if (status) {
        if (ret) {
            ui_status_end(op, "done.");
        } else {
            ui_status_end(op, "Error.");
        }
    }

    /* 编译失败时记录详细的错误信息和命令输出 */
    if (!ret) {
        char *status_extra;

        if (status) {
            status_extra = nvasprintf(" while performing the step: \"%s\"",
                                       status);
        } else {
            status_extra = nvstrdup("");
        }

        ui_error(op, "An error occurred%s. See %s for details.",
                 status_extra, op->log_file_name);
        ui_log(op, "The command `%s` failed with the following output:\n\n%s",
               cmd, data);
        nvfree(status_extra);
    }

    /* 将 make 输出追加到累计的编译日志中（用于后续诊断） */
    if (p->kernel_make_logs) {
        char *old_logs = p->kernel_make_logs;
        p->kernel_make_logs = nvstrcat(old_logs, data, NULL);
        nvfree(old_logs);
        nvfree(data);
    } else {
        p->kernel_make_logs = data;
    }

    nvfree(cmd);

    return ret;
}


/*
 * remove_kernel_module_from_package() - 从安装包的内核模块列表中移除指定模块。
 *
 * 从 p->kernel_modules 数组中删除所有名称匹配的模块条目，
 * 并将其添加到排除列表（p->excluded_kernel_modules）中，
 * 使后续的 make 命令通过 NV_EXCLUDE_KERNEL_MODULES 变量跳过该模块。
 *
 * 使用"移位覆盖"算法：遇到要删除的条目时，后续条目向前移动覆盖它。
 *
 * 参数：
 *   p      - 安装包结构体
 *   module - 要移除的模块名称
 *
 * 返回值：
 *   移除的模块条目数量（通常为 0 或 1）
 */
int remove_kernel_module_from_package(Package *p, const char *module)
{
    int i, found = 0;

    for (i = 0; i < p->num_kernel_modules; i++) {
        /* 将后续条目前移以覆盖已删除的条目 */
        if (found) {
            p->kernel_modules[i - found] = p->kernel_modules[i];
        }

        if (strcmp(p->kernel_modules[i].module_name, module) == 0) {
            /* 释放该模块信息的动态内存 */
            free_kernel_module_info(p->kernel_modules[i]);
            found++;
        }
    }

    if (found) {
        /* 更新模块数量并将模块名添加到排除列表 */
        char *old_exclude_list;

        p->num_kernel_modules -= found;
        old_exclude_list = p->excluded_kernel_modules;
        p->excluded_kernel_modules = nvstrcat(module, " ", old_exclude_list,
                                              NULL);
        nvfree(old_exclude_list);
    }

    return found;
}


/*
 * free_kernel_module_info() - 释放 KernelModuleInfo 结构体中的动态分配内存。
 *
 * 注意：此函数接收结构体值（非指针），不释放结构体本身。
 *
 * 参数：
 *   info - 要释放的内核模块信息结构体
 */
void free_kernel_module_info(KernelModuleInfo info)
{
    nvfree(info.module_name);
    nvfree(info.module_filename);
    if (info.has_separate_interface_file) {
        nvfree(info.interface_filename);
        nvfree(info.core_object_name);
    }
}


/*
 * package_includes_kernel_module() - 检查安装包中是否包含指定名称的内核模块。
 *
 * 参数：
 *   p      - 安装包结构体
 *   module - 要查找的模块名称
 *
 * 返回值：
 *   TRUE  - 包中包含该模块
 *   FALSE - 包中不包含该模块
 */
int package_includes_kernel_module(const Package *p, const char *module)
{
    int i;

    for (i = 0; i < p->num_kernel_modules; i++) {
        if (strcmp(p->kernel_modules[i].module_name, module) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * 以下代码仅在 ppc64le（POWER 架构小端模式）上编译。
 * POWER9 及更新的处理器上，如果内核配置了 CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE
 * （新内存块自动上线），可能导致某些 NVIDIA GPU（Volta 及更新架构）出现问题。
 */
#if defined(NV_PPC64LE)
/*
 * auto_online_blocks() - 检查当前系统的新内存块上线策略。
 *
 * 读取 /sys/devices/system/memory/auto_online_blocks 文件，
 * 判断系统是否配置为自动上线新的热插拔内存块。
 *
 * 返回值：
 *   NV_OPTIONAL_BOOL_TRUE    - 策略为 "online"（自动上线）
 *   NV_OPTIONAL_BOOL_FALSE   - 策略为 "offline"（保持离线）
 *   NV_OPTIONAL_BOOL_DEFAULT - 无法确定策略（文件不存在或读取失败）
 */
static NVOptionalBool auto_online_blocks(void)
{
    static const char *file = "/sys/devices/system/memory/auto_online_blocks";
    int fd;
    NVOptionalBool ret = NV_OPTIONAL_BOOL_DEFAULT;

    fd = open(file, O_RDONLY);
    if (fd >= 0) {
        char auto_online_status[9]; /* strlen("offline\n") + 1 */
        ssize_t bytes_read = read(fd, auto_online_status,
                                  sizeof(auto_online_status) - 1);

        if (bytes_read >= 0) {
            int i = bytes_read;

            /* 添加 NUL 终止符并去除尾部空白 */
            do {
                auto_online_status[i] = '\0';
                i--;
            } while (i >= 0 && isspace(auto_online_status[i]));

            if (strcmp(auto_online_status, "online") == 0) {
                ret = NV_OPTIONAL_BOOL_TRUE;
            } else if (strcmp(auto_online_status, "offline") == 0) {
                ret = NV_OPTIONAL_BOOL_FALSE;
            }
        }

        close(fd);
    }

    return ret;
}

/*
 * get_cpu_type() - 从 /proc/cpuinfo 获取 CPU 类型。
 *
 * 仅在 ppc64le 上使用，因为 /proc/cpuinfo 的格式因 CPU 架构不同而差异很大。
 * 用于判断是否为 POWER8（POWER8 上不存在自动上线内存的兼容性问题）。
 *
 * 参数：
 *   op - 全局选项结构体
 *
 * 返回值：
 *   CPU 类型字符串（如 "POWER9"，调用者负责释放），或 NULL
 */
static char *get_cpu_type(const Options *op)
{
    char *proc_cpuinfo = nvstrcat(op->proc_mount_point, "/", "cpuinfo", NULL);
    FILE *fp = fopen(proc_cpuinfo, "r");

    nvfree(proc_cpuinfo);
    if (fp) {
        char *line, *ret = NULL;
        int eof;

        /* 逐行搜索 "cpu : <type>" 格式的行 */
        while ((line = fget_next_line(fp, &eof))) {
            ret = nvrealloc(ret, strlen(line) + 1);

            if (sscanf(line, "cpu : %s", ret) == 1) {
                return ret;
            }

            if (eof) {
                break;
            }
        }

        nvfree(ret);
    }

    return NULL;
}
#endif

/*
 * kernel_configuration_conflict() - 检查内核配置是否存在与 NVIDIA 驱动的冲突。
 *
 * 目前唯一检测的冲突是 ppc64le 架构上的内存自动上线问题：
 * 在 POWER9 及更新处理器上，如果 CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE 已启用，
 * 某些 NVIDIA GPU（Volta 及更新架构）可能无法正常工作。
 *
 * 参数：
 *   op                  - 全局选项结构体
 *   p                   - 安装包结构体
 *   target_system_checks - 是否执行目标系统特定的检查
 *                          （TRUE=安装模块时，FALSE=仅打包接口时）
 *
 * 返回值：
 *   TRUE  - 检测到冲突且用户选择中止
 *   FALSE - 无冲突，或用户选择忽略冲突
 */
static int kernel_configuration_conflict(Options *op, Package *p,
                                         int target_system_checks)
{
/* 内存自动上线在 POWER9 + Volta 或更新 GPU 上存在问题 */
#if defined(NV_PPC64LE)
    if (test_kernel_config_option(op, p, "CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE")
        == KERNEL_CONFIG_OPTION_DEFINED) {
        NVOptionalBool auto_online = NV_OPTIONAL_BOOL_DEFAULT;

        if (target_system_checks) {
            /* 在目标系统上检查 CPU 类型 */
            char *cpu_type = get_cpu_type(op);
            int cpu_is_power8 = strncmp(cpu_type, "POWER8", strlen("POWER8")) == 0;

            nvfree(cpu_type);
            if (cpu_is_power8) {
                /* POWER8 及更早的处理器不受此问题影响 */
                return FALSE;
            }

            /* 检查实际的内存块上线策略 */
            auto_online = auto_online_blocks();
        }

        if (auto_online == NV_OPTIONAL_BOOL_FALSE) {
            /* 虽然内核配置了自动上线，但当前策略已被设为 offline，无冲突 */
            ui_log(op, "CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE is enabled, but "
                   "current policy is offline new memory blocks; continuing.");
        } else {
            int choice;
            const char *msg = "CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE is enabled "
                              "on the target kernel. Some NVIDIA GPUs on some "
                              "system configurations will not work correctly "
                              "with auto-onlined memory; if you are not sure "
                              "whether your system will work, configure your "
                              "bootloader to set memhp_default_state=offline "
                              "on the kernel command line, or build a kernel "
                              "with CONFIG_MEMORY_HOTPLUG_DEFAULT_ONLINE "
                              "disabled in the kernel configuration. If you "
                              "choose to make these changes, you may abort "
                              "the installation now in order to make them.";

            choice = ui_multiple_choice(op, CONTINUE_ABORT_CHOICES,
                                        NUM_CONTINUE_ABORT_CHOICES,
                                        ABORT_CHOICE, "%s", msg);

            return choice == ABORT_CHOICE;
        }
    }
#endif

    return FALSE;
}

/*
 * 内核模块类型定义表。
 * NVIDIA 驱动提供两种内核模块：
 * - 闭源/专有模块（PROPRIETARY）：位于 "kernel" 目录，使用 NVIDIA 专有许可
 * - 开源模块（OPEN）：位于 "kernel-open" 目录，使用 MIT/GPL 双重许可
 *
 * 并非所有 GPU 都支持开源模块，valid_kernel_module_types() 会根据
 * 检测到的 GPU 类型过滤可用选项。
 */
static struct {
    char type;           /* 模块类型标识符（'p' 或 'o'） */
    char * const dir;    /* 模块源码目录名 */
    char * const license; /* 许可证名称 */
} kernel_module_types[NUM_KERNEL_MODULE_TYPES] = {
    { .type = PROPRIETARY, .dir = "kernel", .license = "NVIDIA Proprietary" },
    { .type = OPEN, .dir = "kernel-open", .license = "MIT/GPL" },
};

/*
 * valid_kernel_module_types() - 确定系统可用的内核模块类型。
 *
 * 根据系统中检测到的 GPU 类型，过滤可用的模块类型：
 * - 如果某个 GPU 要求使用开源模块（required），则排除闭源模块
 * - 如果 GPU 不支持开源模块，则排除开源模块
 * - 同时检查模块源码目录是否存在
 *
 * 当多种类型可用时，默认选择开源模块（OPEN）。
 *
 * 参数：
 *   op                      - 全局选项结构体
 *   info                    - 输出参数，存储可用类型信息
 *   allow_missing_directory - 是否允许模块目录不存在
 *                            （TRUE=用于检查兼容性，FALSE=用于实际编译）
 *
 * 返回值：
 *   可用的模块类型数量（0、1 或 2）
 */
int valid_kernel_module_types(Options *op, struct module_type_info *info, int allow_missing_directory)
{
    int num_valid_types = 0, i;

    memset(info, 0, sizeof(*info));

    for (i = 0; i < NUM_KERNEL_MODULE_TYPES; i++) {
        if (op->open_modules.required) {
            /*
             * 至少有一个 GPU 要求使用开源模块，不允许闭源模块。
             * 这通常是因为 GPU 架构只支持开源内核模块。
             */
            if (kernel_module_types[i].type == PROPRIETARY) {
                continue;
            }

            /* 如果系统中还有不支持开源模块的 GPU，发出警告 */
            if (op->open_modules.unsupported_gpu_present) {
                ui_warn(op, "This system requires the open GPU kernel "
                        "modules, but contains GPUs which are not "
                        "supported by the open GPU kernel modules. "
                        "The unsupported GPUs will be ignored.");
            }
        } else if (!op->open_modules.supported_gpu_present ||
                   op->open_modules.unsupported_gpu_present) {
            /*
             * GPU 不支持开源模块（或同时存在不支持的 GPU），
             * 将开源模块标记为无效。用户仍可通过命令行强制安装。
             */
            if (kernel_module_types[i].type == OPEN) {
                continue;
            }
        }

        /* 检查模块源码目录是否存在（或允许不存在） */
        if (directory_exists(kernel_module_types[i].dir) || allow_missing_directory) {
            info->types[num_valid_types] = kernel_module_types[i].type;
            info->dirs[num_valid_types] = kernel_module_types[i].dir;
            info->licenses[num_valid_types] = kernel_module_types[i].license;

            num_valid_types++;
        }
    }

    /* 如果有多种类型可用，确定默认选择（优先选择开源模块） */
    if (num_valid_types > 1) {
        char default_type = OPEN;

        for (i = 0; i < num_valid_types; i++) {
            if (info->types[i] == default_type) {
                info->default_entry = i;
                break;
            }
        }

        if (i == num_valid_types) {
            /* 未找到默认类型，使用数组第一项（default_entry 已被 memset 为 0） */
            ui_warn(op, "An error occurred while selecting the default kernel "
                    "module type; using \"%s\" as the default.",
                    info->licenses[info->default_entry]);
        }
    }

    /* 如果没有可用类型，或默认类型的目录不存在，报告错误/警告 */
    if (num_valid_types == 0 ||
        (!directory_exists(info->dirs[info->default_entry]) && allow_missing_directory)) {
        const char *msg = "This system requires a kernel module type which is "
                          "not present in this installer package.";
        if (num_valid_types == 0) {
            ui_error(op, "%s", msg);
        } else {
            ui_warn(op, "%s", msg);
        }
    }

    return num_valid_types;
}

/*
 * override_kernel_module_build_directory() - 覆盖内核模块构建目录。
 *
 * 当用户通过 --kernel-module-type 或其他选项指定特定的模块类型时调用。
 * 验证指定的目录是否有效、是否存在，以及是否与之前的设置冲突。
 *
 * 即使指定的目录不在 valid_kernel_module_types() 返回的有效列表中，
 * 如果目录存在且是已知的模块类型，仍允许强制安装（附带警告）。
 *
 * 参数：
 *   op        - 全局选项结构体
 *   directory - 要使用的模块构建目录名（如 "kernel" 或 "kernel-open"）
 *
 * 返回值：
 *   TRUE  - 覆盖成功
 *   FALSE - 目录无效或存在冲突
 */
int override_kernel_module_build_directory(Options *op, const char *directory)
{
    int i, num_types, ret = FALSE;
    struct module_type_info types;

    if (!directory_exists(directory)) {
        ui_error(op, "The kernel module build directory '%s' is not present in "
                 "this installer package.", directory);
        return FALSE;
    }

    /* 检查指定目录是否在当前系统的有效类型列表中 */
    num_types = valid_kernel_module_types(op, &types, FALSE);

    for (i = 0; i < num_types; i++) {
        if (strcmp(directory, types.dirs[i]) == 0) {
            ret = TRUE;
            break;
        }
    }

    if (ret) {
        /* 检查是否与之前设置的覆盖值冲突 */
        if (op->kernel_module_build_directory_override) {
            if (strcmp(op->kernel_module_build_directory_override, directory)) {
                ui_error(op, "Conflicting options set the kernel module build "
                         "directory to both '%s' and '%s'. Please use only one.",
                         op->kernel_module_build_directory_override, directory);
                ret = FALSE;
            }
        }
    } else {
        /*
         * 指定的目录不在有效列表中。检查它是否是已知的模块类型
         * （可能因为 GPU 不兼容而被过滤掉）。如果是，允许强制安装。
         */
        for (i = 0; i < NUM_KERNEL_MODULE_TYPES; i++) {
            if (strcmp(directory, kernel_module_types[i].dir) == 0) {
                if (directory_exists(directory)) {
                    if (kernel_module_types[i].type == OPEN &&
                        op->open_modules.supported_gpu_present &&
                        op->open_modules.unsupported_gpu_present) {
                        /* 部分 GPU 支持开源模块，部分不支持 */
                        ui_warn(op, "The open GPU kernel modules are supported "
                                "on some GPUs in this system, and unsupported "
                                "on others. The unsupported GPUs will be "
                                "ignored.");
                    } else {
                        /* 模块与检测到的 GPU 不兼容，但仍允许安装 */
                        ui_warn(op, "The '%s' kernel modules are incompatible "
                                "with the GPU(s) detected on this system. They "
                                "will be installed anyway, but are not "
                                "expected to work.",
                                kernel_module_types[i].license);
                    }
                    ret = TRUE;
                } else {
                    ui_error(op, "The '%s' kernel modules are not "
                             "present in this installer package.",
                             kernel_module_types[i].license);
                }
                break;
            }
        }

        if (i >= NUM_KERNEL_MODULE_TYPES) {
            ui_error(op, "'%s' is not a valid kernel module directory.",
                     directory);
        }
    }

    /* 保存覆盖设置 */
    if (ret) {
        op->kernel_module_build_directory_override = nvstrdup(directory);
    }

    return ret;
}

/*
 * override_kernel_module_type() - 通过类型名称覆盖内核模块类型。
 *
 * 将类型名称（如 "open"、"proprietary"）的首字母与已知类型匹配，
 * 然后委托给 override_kernel_module_build_directory() 处理。
 *
 * 参数：
 *   op   - 全局选项结构体
 *   type - 类型名称字符串（首字母匹配：'o'=开源，'p'=闭源）
 *
 * 返回值：
 *   TRUE  - 覆盖成功
 *   FALSE - 类型无效或目录不存在
 */
int override_kernel_module_type(Options *op, const char *type)
{
    const char *directory = NULL;
    int i;

    /* 通过首字母匹配模块类型 */
    for (i = 0; i < NUM_KERNEL_MODULE_TYPES; i++) {
        if (tolower(type[0]) == kernel_module_types[i].type) {
            directory = kernel_module_types[i].dir;
            break;
        }
    }

    if (!directory) {
        ui_error(op, "'%s' is not a valid kernel module type.", type);
        return FALSE;
    }

    return override_kernel_module_build_directory(op, directory);
}
