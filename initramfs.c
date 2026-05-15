/*
 * Copyright (C) 2023 NVIDIA Corporation
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
 * 【文件说明】initramfs 重建功能实现。
 *
 * 本文件负责在 NVIDIA 驱动安装完成后，检测并重建 initramfs（初始内存文件系统）。
 * Linux 系统启动时会先加载 initramfs，如果其中包含旧的 NVIDIA 内核模块或
 * Nouveau 开源驱动模块，可能导致新安装的驱动无法正常加载。因此安装器需要：
 *   1. 扫描现有 initramfs 内容，检测是否包含冲突模块（Nouveau 或旧 NVIDIA 模块）
 *   2. 如果检测到冲突，提示用户使用系统工具重建 initramfs
 *
 * 支持的 initramfs 工具：
 *   - 列出工具：lsinitramfs（Debian/Ubuntu）、lsinitrd（Fedora/RHEL/dracut）、
 *               lsinitcpio（Arch Linux）
 *   - 重建工具：dracut（Fedora/RHEL）、update-initramfs（Debian/Ubuntu）、
 *               mkinitcpio（Arch Linux）
 *
 * 扫描过程在后台线程中异步执行，以避免阻塞安装流程。
 */

#include <string.h>      /* 字符串操作：strdup, strchr, strstr 等 */
#include <unistd.h>      /* POSIX 标准：access() 文件存在性检查 */
#include <pthread.h>     /* POSIX 线程：用于后台异步扫描 initramfs */

#include "nvidia-installer.h"           /* Options 结构体定义及全局类型 */
#include "user-interface.h"             /* UI 交互函数：ui_log, ui_warn, ui_multiple_choice 等 */
#include "kernel.h"                     /* 内核相关工具函数：get_kernel_name 等 */
#include "initramfs.h"                  /* 本模块头文件：begin_initramfs_scan, update_initramfs */
#include "misc.h"                       /* 杂项工具函数：find_system_util, run_command 等 */
#include "conflicting-kernel-modules.h" /* 冲突内核模块列表：nouveau 等 */

/*
 * find_initramfs_images() - 在 /boot 目录下查找符合已知命名模式的 initramfs 镜像文件。
 *
 * 参数：
 *   op          - 安装器选项结构体，包含内核版本等信息
 *   found_paths - 输出参数（可为 NULL）。如果非 NULL，将通过此指针返回一个
 *                 堆分配的、以 NULL 结尾的字符串数组，其中包含所有找到的镜像路径。
 *
 * 返回值：找到的 initramfs 镜像文件数量。
 *
 * 查找的文件名模式包括：
 *   - /boot/initramfs-<内核版本>.img   （Fedora/RHEL/Arch 等）
 *   - /boot/initramfs-linux.img        （Arch Linux 默认内核）
 *   - /boot/initramfs-linux-lts.img    （Arch Linux LTS 内核）
 *   - /boot/initrd-<内核版本>          （openSUSE 等）
 *   - /boot/initrd.img-<内核版本>      （Debian/Ubuntu 等）
 *   - /boot/initramfs-linux-<主版本>.<次版本>-<架构>.img （通用模式）
 */
static int find_initramfs_images(Options *op, char ***found_paths)
{
    /* 获取当前目标内核的名称/版本字符串 */
    char *kernel_name  = get_kernel_name(op);
    int num_found_paths = 0;

    if (found_paths) {
        *found_paths = NULL;
    }

    if (kernel_name) {
        /* 数组大小必须至少比下面 __TEST_INITRAMFS_FILE 宏的调用次数多 2：
         * 一个用于 NULL 终止符，另一个用于调用方可能添加的"以上都不选"选项。 */
        const int found_paths_size = 8;
        char *tmp;

        if (found_paths) {
            *found_paths = nvalloc(found_paths_size * sizeof(char*));
        }

        /*
         * __TEST_INITRAMFS_FILE 宏：根据给定的格式和字符串构建 /boot/ 下的路径，
         * 检查文件是否存在（access(F_OK)），如果存在则加入结果数组。
         */
        #define __TEST_INITRAMFS_FILE(format, str) { \
            char *path = nvasprintf("/boot/" format, str); \
            if (access(path, F_OK) == 0) { \
                if (found_paths && num_found_paths < found_paths_size - 2) { \
                    (*found_paths)[num_found_paths] = path; \
                } \
                num_found_paths++; \
            } else { \
                nvfree(path); \
            } \
        }

        /* 添加新模板时，如有必要请增大 found_paths_size */
        __TEST_INITRAMFS_FILE("initramfs-%s.img", kernel_name);      /* 例：initramfs-5.15.0-generic.img */
        __TEST_INITRAMFS_FILE("initramfs-%s.img", "linux");           /* Arch: initramfs-linux.img */
        __TEST_INITRAMFS_FILE("initramfs-%s.img", "linux-lts");       /* Arch LTS: initramfs-linux-lts.img */
        __TEST_INITRAMFS_FILE("initrd-%s", kernel_name);              /* openSUSE 风格 */
        __TEST_INITRAMFS_FILE("initrd.img-%s", kernel_name);          /* Debian/Ubuntu 风格 */

        /*
         * 尝试从内核版本号中提取主版本和次版本号（例如从 "6.1.15-arch1-1"
         * 提取 "6.1"），构造 "linux-<主版本>.<次版本>-<架构>" 格式的名称，
         * 并检查对应的 initramfs 镜像是否存在。
         */
        tmp = strchr(kernel_name, '.');
        if (tmp) {
            char *kernel_name_copy = strdup(kernel_name);

            if (kernel_name_copy) {
                /* 找到第一个 '.'，然后找到第二个 '.' */
                tmp = strchr(kernel_name_copy, '.');
                if (tmp) {
                    tmp = strchr(tmp + 1, '.');
                }
                if (tmp) {
                    char *linux_ver_arch;

                    /* 在第二个 '.' 处截断，得到 "主版本.次版本" */
                    tmp[0] = '\0';
                    /* 构造例如 "linux-6.1-x86_64" */
                    linux_ver_arch = nvstrcat("linux-", kernel_name_copy, "-",
                                              get_machine_arch(op), NULL);
                    __TEST_INITRAMFS_FILE("initramfs-%s.img", linux_ver_arch);
                    nvfree(linux_ver_arch);
                }
            }

            nvfree(kernel_name_copy);
        }
    }

    return num_found_paths;
}

/*
 * get_initramfs_path() - 获取 initramfs 镜像文件路径。
 *
 * 在已知位置查找候选的 initramfs 文件：
 *   - 如果只找到一个，直接返回该路径
 *   - 如果找到多个且处于交互模式，提示用户选择其中一个
 *   - 如果没找到，或找到多个但用户拒绝选择（或非交互模式），返回 NULL
 *
 * 参数：
 *   op          - 安装器选项结构体
 *   interactive - 是否允许与用户交互（提示选择）
 *
 * 返回值：选中的 initramfs 文件路径，或 NULL。
 *
 * 注意：此函数使用 static 变量缓存结果，多次调用时会返回缓存值。
 * 但如果首次在非交互模式下未找到路径，允许后续在交互模式下重试。
 */

static char *get_initramfs_path(Options *op, int interactive)
{
    static char *path_ret = NULL;   /* 缓存的返回路径（跨调用保持） */
    static int attempted = FALSE;   /* 标记是否已经尝试过查找 */
    int num_found_paths;
    char **found_paths;
    int i;

    if (attempted) {
        /* 此函数已经被调用过：返回缓存的路径值（可能为 NULL） */
        return path_ret;
    }

    /* 查找所有匹配的 initramfs 镜像文件 */
    num_found_paths = find_initramfs_images(op, &found_paths);

    if (num_found_paths == 1) {
        /* 只找到一个镜像，直接使用 */
        path_ret = found_paths[0];
    } else if (num_found_paths > 1 && interactive) {
        int answer;

        /* find_initramfs_images() 已确保数组有足够空间添加额外选项 */
        found_paths[num_found_paths] = "Use none of these";

        /* 提示用户从多个候选中选择一个 */
        answer = ui_multiple_choice(op, (const char * const*)found_paths,
                                    num_found_paths + 1, num_found_paths,
                                    "More than one initramfs file found. "
                                    "Which file would you like to use?");
        if (answer < num_found_paths) {
            /* answer == num_found_paths 表示用户选择了"以上都不选" */
            path_ret = found_paths[answer];
        }
    }

    /* 清理未被选中的路径（释放内存） */
    for (i = 0; i < num_found_paths; i++) {
        if (found_paths[i] != path_ret) {
            nvfree(found_paths[i]);
        }
    }
    nvfree(found_paths);

    /* 如果在非交互模式下未找到路径，允许后续在交互模式下重试 */
    attempted = path_ret || interactive;

    return path_ret;
}

/*
 * InitramfsToolType - initramfs 工具类型枚举。
 * 区分"列出内容"和"重建"两类工具，因为扫描和重建是不同的操作步骤。
 */
typedef enum {
    INITRAMFS_LIST_TOOL,       /* 列出 initramfs 内容的工具（用于扫描） */
    INITRAMFS_REBUILD_TOOL,    /* 重建 initramfs 的工具 */
} InitramfsToolType;

/* 工具类型对应的用途描述字符串（用于日志和用户提示） */
char *initramfs_tool_purpose[] = {
    [INITRAMFS_LIST_TOOL] = "listing initramfs contents",
    [INITRAMFS_REBUILD_TOOL] = "rebuilding initramfs",
};

/*
 * initramfs_tools[] - 已知 initramfs 工具及其命令行参数的配置表。
 *
 * 每个条目描述一个工具的类型、名称以及各种情况下的命令行参数。
 * 安装器会在系统 PATH 中查找这些工具，自动选择可用的工具来使用。
 *
 * 字段说明：
 *   type                - 工具类型（列出内容 / 重建）
 *   name                - 工具可执行文件的名称
 *   common_args         - 始终添加的参数
 *   kernel_specific_args - 指定内核版本时的参数前缀（NULL 表示不支持指定内核）
 *   requires_kernel     - 是否必须指定内核版本
 *   path_specific_args  - 指定 initramfs 文件路径时的参数前缀（NULL 表示不支持指定路径）
 *   requires_path       - 是否必须指定 initramfs 文件路径
 */
static struct initramfs_tool {
        InitramfsToolType type;
        const char *name;
        const char *common_args;
        const char *kernel_specific_args;
        int requires_kernel;
        const char *path_specific_args;
        int requires_path;
    } initramfs_tools[] = {
    {
        /* lsinitramfs：Debian/Ubuntu 系统的 initramfs 内容列出工具。
         * 用法示例：lsinitramfs -l /boot/initrd.img-5.15.0
         * 必须指定 initramfs 文件路径，不支持直接指定内核版本。 */
        .type = INITRAMFS_LIST_TOOL,
        .name = "lsinitramfs",
        .common_args = "",
        .kernel_specific_args = NULL,
        .requires_kernel = FALSE,
        .path_specific_args = "-l",
        .requires_path = TRUE,
    },
    {
        /* lsinitrd：dracut 配套的列出工具（Fedora/RHEL 等）。
         * 用法示例：lsinitrd 或 lsinitrd -k 5.15.0 /boot/initramfs-5.15.0.img
         * 可选指定内核版本（-k）和文件路径。 */
        .type = INITRAMFS_LIST_TOOL,
        .name = "lsinitrd",
        .common_args = "",
        .kernel_specific_args = "-k",
        .requires_kernel = FALSE,
        .path_specific_args = "",
        .requires_path = FALSE,
    },
    {
        /* lsinitcpio：Arch Linux 的 initramfs 内容列出工具。
         * 用法示例：lsinitcpio /boot/initramfs-linux.img
         * 必须指定 initramfs 文件路径。 */
        .type = INITRAMFS_LIST_TOOL,
        .name = "lsinitcpio",
        .common_args = "",
        .kernel_specific_args = NULL,
        .requires_kernel = FALSE,
        .path_specific_args = "",
        .requires_path = TRUE,
    },
    {
        /* dracut：Fedora/RHEL/openSUSE 等系统的 initramfs 重建工具。
         * 用法示例：dracut --force 或 dracut --force --kver 5.15.0
         * --force 参数表示强制覆盖已有的 initramfs 镜像。 */
        .type = INITRAMFS_REBUILD_TOOL,
        .name = "dracut",
        .common_args = "--force",
        .kernel_specific_args = "--kver",
        .requires_kernel = FALSE,
        .path_specific_args = "",
        .requires_path = FALSE,
    },
    {
        /* update-initramfs：Debian/Ubuntu 系统的 initramfs 更新工具。
         * 用法示例：update-initramfs -u 或 update-initramfs -u -k 5.15.0
         * -u 参数表示更新现有的 initramfs。不支持指定输出路径。 */
        .type = INITRAMFS_REBUILD_TOOL,
        .name = "update-initramfs",
        .common_args = "-u",
        .kernel_specific_args = "-k",
        .requires_kernel = FALSE,
        .path_specific_args = NULL,
        .requires_path = FALSE,
    },
    {
        /* mkinitcpio：Arch Linux 的 initramfs 生成工具。
         * 用法示例：mkinitcpio -P
         * -P 参数表示根据所有预设（preset）重建所有 initramfs。 */
        .type = INITRAMFS_REBUILD_TOOL,
        .name = "mkinitcpio",
        .common_args = "-P",
        .kernel_specific_args = "",
        .requires_kernel = FALSE,
        .path_specific_args = "",
        .requires_path = FALSE,
    },
};

typedef struct initramfs_tool InitramfsTool;

/*
 * get_tool_index() - 验证工具的前置条件并返回其在 initramfs_tools 数组中的索引。
 *
 * 参数：
 *   op   - 安装器选项
 *   tool - 指向 initramfs_tools 数组中某个元素的指针
 *
 * 返回值：
 *   工具在数组中的索引（>= 0 表示可用），
 *   或 -1 表示前置条件不满足（缺少所需的内核名称或 initramfs 文件）。
 */
static int get_tool_index(Options *op, const InitramfsTool *tool)
{
    /* 如果工具要求指定内核版本，但无法获取内核名称，则不可用 */
    if (tool->requires_kernel && get_kernel_name(op) == NULL) {
        return -1;
    }

    /* 如果工具要求指定 initramfs 路径，但找不到任何 initramfs 镜像，则不可用 */
    if (tool->requires_path && find_initramfs_images(op, NULL) == 0) {
        return -1;
    }

    /* 通过指针算术计算数组索引 */
    return tool - initramfs_tools;
}

/* 交互模式标志：NON_INTERACTIVE 不与用户交互，INTERACTIVE 允许用户交互 */
enum {
    NON_INTERACTIVE,
    INTERACTIVE,
};

/*
 * find_initramfs_tool() - 在系统中查找指定类型的 initramfs 工具。
 *
 * 遍历 initramfs_tools 配置表，使用 find_system_util() 检查每个同类型
 * 工具是否存在于系统 PATH 中。
 *
 * 处理逻辑：
 *   - 找到 0 个：记录日志并返回 -1
 *   - 找到 1 个：直接返回该工具的索引
 *   - 找到多个且为交互模式：提示用户选择
 *   - 找到多个且为非交互模式：返回 -1（无法自动选择）
 *
 * 参数：
 *   op          - 安装器选项结构体
 *   type        - 要查找的工具类型（列出 / 重建）
 *   interactive - 交互模式标志
 *
 * 返回值：initramfs_tools[] 数组中的索引，或 -1 表示未找到/未选择。
 */
static int find_initramfs_tool(Options *op, InitramfsToolType type,
                               int interactive)
{
    InitramfsTool *found_tools[ARRAY_LEN(initramfs_tools)];
    const char *purpose = initramfs_tool_purpose[type];
    int i, num_found_tools = 0;

    /* 遍历所有已知工具，查找与指定类型匹配且在系统中存在的工具 */
    for (i = 0; i < ARRAY_LEN(initramfs_tools); i++) {
        if (initramfs_tools[i].type != type) {
            continue;
        }

        /* find_system_util() 在 PATH 中搜索可执行文件 */
        if (find_system_util(initramfs_tools[i].name)) {
            found_tools[num_found_tools++] = initramfs_tools + i;
        }
    }

    if (num_found_tools == 0) {
        ui_log(op, "Unable to locate any tools for %s.", purpose);
        return -1;
    }

    if (num_found_tools == 1) {
        /* 只找到一个匹配的工具，验证其前置条件并返回索引 */
        return get_tool_index(op, found_tools[0]);
    } else if (interactive == NON_INTERACTIVE)  {
        /* 非交互模式下找到多个工具，无法自动选择 */
        return -1;
    } else {
        /* 交互模式：让用户从多个工具中选择 */
        const char *found_tool_names[num_found_tools + 1];
        int chosen_tool;

        for (i = 0; i < num_found_tools; i++) {
            found_tool_names[i] = found_tools[i]->name;
        }

        /* 添加"以上都不选"选项 */
        found_tool_names[num_found_tools] = "None of these";

        chosen_tool = ui_multiple_choice(op, found_tool_names,
                                         num_found_tools + 1, num_found_tools,
                                         "More than one tool for %s detected. "
                                         "Which tool would you like to use?",
                                         purpose);

        if (chosen_tool == num_found_tools) {
            /* 用户选择了"以上都不选" */
            return -1;
        }

        return get_tool_index(op, found_tools[chosen_tool]);
    }
}

/*
 * initramfs_tool_helper() - 使用指定的工具、内核版本和 initramfs 路径参数执行命令。
 *
 * 此函数是执行 initramfs 工具的核心辅助函数。它负责：
 *   1. 根据工具配置构建完整的命令行
 *   2. 在交互模式下显示进度指示器
 *   3. 执行命令并捕获输出
 *   4. 处理错误情况
 *
 * 参数：
 *   op          - 安装器选项结构体
 *   tool        - initramfs_tools[] 数组中的工具索引
 *   kernel      - 目标内核版本字符串（可为 NULL 表示不指定）
 *   path        - initramfs 镜像文件路径（可为 NULL 表示不指定）
 *   data        - 输出参数，用于接收命令的标准输出（可为 NULL 表示不需要）
 *   interactive - 是否显示进度指示器等交互元素
 *
 * 返回值：0 表示成功，非 0 表示失败。
 */
static int initramfs_tool_helper(Options *op, int tool, const char *kernel,
                                 const char *path, char **data, int interactive)
{
    char *cmd, *tool_path, *kernel_args, *path_args, *buf, **cmd_data;
    /* standalone 标志：当前没有活跃的状态栏时为 TRUE，表示需要自行管理状态栏 */
    int standalone = !op->ui.status_active;
    int ret = 1;

    /* 如果调用方需要命令输出，使用调用方提供的指针；否则使用本地缓冲区 */
    if (data) {
        cmd_data = data;
    } else {
        cmd_data = &buf;
    }

    *cmd_data = NULL;

    /* 获取工具的完整路径 */
    tool_path = find_system_util(initramfs_tools[tool].name);
    kernel_args = path_args = NULL;

    /* 构建内核版本相关的命令行参数 */
    if (kernel) {
        if (!initramfs_tools[tool].kernel_specific_args) {
            ui_log(op, "%s does not take a kernel argument.", tool_path);
            goto done;
        }
        /* 拼接内核参数，例如 "--kver 5.15.0" */
        kernel_args = nvstrcat(initramfs_tools[tool].kernel_specific_args, " ",
                               kernel, NULL);
    } else {
        if (initramfs_tools[tool].requires_kernel) {
            ui_log(op, "%s requires a kernel argument, but none was given.",
                   tool_path);
            goto done;
        }
        kernel_args = nvstrdup("");
    }

    /* 构建 initramfs 文件路径相关的命令行参数 */
    if (path && initramfs_tools[tool].path_specific_args) {
        if (!initramfs_tools[tool].path_specific_args) {
            ui_log(op, "%s does not take a path argument.", tool_path);
            goto done;
        }
        /* 拼接路径参数，例如 "-l /boot/initramfs-5.15.0.img" */
        path_args = nvstrcat(initramfs_tools[tool].path_specific_args, " ",
                             path, NULL);
    } else {
        if (initramfs_tools[tool].requires_path || !path) {
            ui_log(op, "%s requires a file path argument, but none was given.",
                   tool_path);
            goto done;
        }
        path_args = nvstrdup("");
    }

    /* 将所有参数组合成完整的命令行字符串 */
    cmd = nvstrcat(tool_path, " ", initramfs_tools[tool].common_args, " ",
                   kernel_args, " ", path_args, NULL);

    /* 在交互模式下，显示进度指示器（initramfs 重建可能需要较长时间） */
    if (interactive) {
        const char *s = initramfs_tool_purpose[initramfs_tools[tool].type];

        if (standalone) {
            /* 如果没有已有的状态栏，则创建一个新的 */
            ui_status_begin(op, "Processing the initramfs:", "%s", s);
        }

        /* 启动不确定进度指示器（旋转动画等） */
        ui_indeterminate_begin(op, "%s (this may take a while)", s);
    }

    /* 记录并执行命令 */
    ui_log(op, "Executing: %s", cmd);
    ret = run_command(op, cmd_data, FALSE, NULL, TRUE, cmd, NULL);

    if (interactive) {
        /* 停止进度指示器 */
        ui_indeterminate_end(op);

        if (standalone) {
            /* 结束状态栏显示 */
            ui_status_end(op, ret == 0 ? "done" : "failed");
        }
    }

    if (ret != 0) {
        ui_log(op, "Failed to run `%s`:\n\n%s", cmd, *cmd_data);
    }
    nvfree(cmd);

done:
    /* 清理临时分配的内存 */
    nvfree(tool_path);
    nvfree(kernel_args);
    nvfree(path_args);

    if (!data) {
        /* 如果调用方不需要输出数据，释放本地缓冲区 */
        nvfree(buf);
    }

    return ret;
}


/*
 * run_initramfs_tool() - 使用渐进式参数策略运行指定的 initramfs 工具。
 *
 * 先尝试最少的参数，如果失败则逐步添加更多参数重试。
 * 重试策略（按优先级从高到低）：
 *   1. 不指定内核版本和路径（让工具自动检测）
 *   2. 仅指定 initramfs 文件路径
 *   3. 仅指定内核版本
 *   4. 同时指定内核版本和 initramfs 文件路径
 *
 * 参数：
 *   op          - 安装器选项结构体
 *   tool        - initramfs_tools[] 数组中的工具索引
 *   data        - 输出参数，用于接收命令输出（可为 NULL）
 *   interactive - 是否为交互模式
 *
 * 返回值：0 表示成功，非 0 表示所有尝试均失败。
 */
static int run_initramfs_tool(Options *op, int tool, char **data,
                              int interactive)
{
    int ret = -1;
    /*
     * 以下情况必须指定内核版本：
     *   - 工具本身要求指定内核版本
     *   - 用户在命令行明确指定了内核版本（通常意味着目标内核不是当前运行的内核）
     */
    int kernel_required = initramfs_tools[tool].requires_kernel ||
                          op->kernel_name != NULL;

    if (data) {
        *data = NULL;
    }

    /* 策略 1：不指定内核版本和路径（如果两者都不是必需的） */
    if (!kernel_required && !initramfs_tools[tool].requires_path) {
        ret = initramfs_tool_helper(op, tool, NULL, NULL, data, interactive);
    }

    /* 策略 2：仅指定 initramfs 路径（如果内核版本不是必需的） */
    if (ret != 0 && !kernel_required) {
        char *initramfs_path = get_initramfs_path(op, interactive);

        if (data) {
            nvfree(*data);
        }
        ret = initramfs_tool_helper(op, tool, NULL, initramfs_path,
                                    data, interactive);
    }

    /* 策略 3：仅指定内核版本（如果路径不是必需的） */
    if (ret != 0 && !initramfs_tools[tool].requires_path) {
        if (data) {
            nvfree(*data);
        }
        ret = initramfs_tool_helper(op, tool, get_kernel_name(op), NULL, data,
                                    interactive);
    }

    /* 策略 4：同时指定内核版本和 initramfs 路径（最完整的参数） */
    if (ret != 0) {
        char *initramfs_path = get_initramfs_path(op, interactive);

        if (data) {
            nvfree(*data);
        }
        ret = initramfs_tool_helper(op, tool, get_kernel_name(op),
                                    initramfs_path, data, interactive);
    }

    return ret;
}

/* 后台扫描线程句柄（用于异步扫描 initramfs 内容） */
static pthread_t scan_thread;

/*
 * ScanThreadData - 后台 initramfs 扫描线程的共享数据结构。
 *
 * 在安装过程中，initramfs 扫描在后台线程中异步执行，
 * 以便安装器可以继续进行其他操作。扫描完成后通过 pthread_join 获取结果。
 */
typedef struct {
    /* initramfs_tools[] 数组中扫描工具的索引。
     * 负值表示没有找到合适的工具。
     * 应使用 find_initramfs_tool() 的返回值或负值来初始化。 */
    int tool;

    /* 以下标志表示 initramfs 扫描的结果，首次扫描前应全部初始化为 0 */

    /* 在 initramfs 中检测到 Nouveau 驱动模块 */
    int nouveau_ko_detected;
    /* 在 initramfs 中检测到 NVIDIA 内核模块（旧版本） */
    int nvidia_ko_detected;
    /* 非交互式扫描失败，需要用户交互才能完成（例如需要用户
     * 在多个候选工具中选择）。此标志指示后续应在交互模式下重试。 */
    int try_scan_again;
    /* initramfs 已成功扫描完成，*_ko_detected 标志的值可信。
     * 如果为 FALSE，则检测结果不可靠（扫描可能失败或未完成）。 */
    int scan_complete;
} ScanThreadData;

/*
 * scan_initramfs() - 使用指定的工具扫描 initramfs 内容，检测冲突模块。
 *
 * 检测目标：
 *   1. Nouveau 开源驱动模块（/nouveau.ko）- 与 NVIDIA 专有驱动冲突
 *   2. 旧的 NVIDIA 内核模块 - 可能与新安装的版本冲突
 *
 * 扫描方式：运行列出工具（如 lsinitramfs），获取 initramfs 中的文件列表，
 * 然后在列表中搜索冲突模块的文件名。
 *
 * 参数：
 *   op          - 安装器选项结构体
 *   data        - 扫描线程数据结构（存储扫描结果）
 *   interactive - 交互模式标志
 */
static void scan_initramfs(Options *op, ScanThreadData *data, int interactive)
{
    if (data->tool >= 0) {
        char *listing;
        int ret;

        ui_log(op, "Scanning the initramfs with %s...",
               initramfs_tools[data->tool].name);

        /* 运行列出工具，将 initramfs 内容列表存储在 listing 中 */
        ret = run_initramfs_tool(op, data->tool, &listing, interactive);
        data->scan_complete = FALSE;

        if (ret == 0) {
            int i;

            /* 检查列表中是否包含 Nouveau 内核模块 */
            if (strstr(listing, "/nouveau.ko")) {
                ui_log(op, "Nouveau detected in initramfs");

                data->nouveau_ko_detected = TRUE;
            }

            /* 遍历所有已知的冲突 NVIDIA 内核模块名称，检查是否存在于 initramfs 中 */
            for (i = 0; i < num_conflicting_kernel_modules; i++) {
                /* 构造模块文件名模式，例如 "/nvidia.ko" */
                char *module = nvstrcat("/", conflicting_kernel_modules[i],
                                        ".ko", NULL);
                if (strstr(listing, module)) {
                    ui_log(op, "%s detected in initramfs", module + 1);

                    data->nvidia_ko_detected = TRUE;
                }

                nvfree(module);

                /* 一旦检测到任何 NVIDIA 模块，无需继续检查其余的 */
                if (data->nvidia_ko_detected) {
                    break;
                }
            }

            data->scan_complete = TRUE;
        }
        nvfree(listing);

        /* 如果非交互式扫描失败，标记后续在交互模式下重试 */
        data->try_scan_again = !interactive && !data->scan_complete;
        ui_log(op, "Initramfs scan %s.", ret == 0 ? "complete" : "failed");
    } else {
        ui_log(op, "Unable to scan initramfs: no tool found");
    }
}

/*
 * initramfs_scan_worker() - 后台扫描线程的入口函数。
 *
 * 在非交互模式下查找列出工具并执行 initramfs 扫描。
 * 使用 static 局部变量保存扫描结果，通过线程返回值传递给主线程。
 *
 * 参数：
 *   arg - 指向 Options 结构体的指针（通过 pthread_create 传入）
 *
 * 返回值：指向 static ScanThreadData 结构体的指针（包含扫描结果）。
 */
static void *initramfs_scan_worker(void *arg)
{
    static ScanThreadData data = {};
    Options *op = arg;

    /* 在非交互模式下查找可用的 initramfs 列出工具 */
    data.tool = find_initramfs_tool(op, INITRAMFS_LIST_TOOL, NON_INTERACTIVE);

    /* 执行扫描（非交互模式，如果需要用户选择则标记 try_scan_again） */
    scan_initramfs(op, &data, NON_INTERACTIVE);

    return &data;
}

/*
 * begin_initramfs_scan() - 启动异步 initramfs 扫描。
 *
 * 创建一个后台线程来扫描 initramfs 内容，检测是否包含冲突的内核模块。
 * 扫描结果将在后续调用 update_initramfs() 时通过 pthread_join 获取。
 *
 * 此函数只会启动一次扫描（通过 static 标志防止重复启动）。
 *
 * 参数：
 *   op - 安装器选项结构体
 *
 * 返回值：TRUE 表示扫描已启动（或之前已启动），FALSE 表示启动失败。
 */
int begin_initramfs_scan(Options *op)
{
    static int scan_started;
    int ret;

    /* 防止重复启动扫描线程 */
    if (scan_started) {
        return TRUE;
    }

    /* 创建后台扫描线程 */
    ret = pthread_create(&scan_thread, NULL, initramfs_scan_worker, op);

    if (ret == 0) {
        scan_started = TRUE;
        return TRUE;
    }

    return FALSE;
}

/*
 * update_initramfs() - 检测是否需要重建 initramfs，并引导用户完成重建。
 *
 * 这是 initramfs 管理的主入口函数，在驱动安装完成后调用。
 *
 * 处理流程：
 *   1. 查找可用的 initramfs 重建工具
 *   2. 如果用户通过命令行明确指定了重建行为（--rebuild-initramfs / --no-rebuild-initramfs），
 *      则直接执行用户的选择
 *   3. 否则，等待后台扫描线程完成，获取扫描结果
 *   4. 根据扫描结果（是否检测到 Nouveau 或旧 NVIDIA 模块），
 *      决定是否需要重建，并提示用户确认
 *   5. 如果用户同意重建，执行重建命令
 *
 * 参数：
 *   op - 安装器选项结构体
 *
 * 返回值：
 *   TRUE  - 成功（包括成功重建、用户选择不重建、无需重建等情况）
 *   FALSE - 尝试重建但失败
 */
int update_initramfs(Options *op)
{
    int rebuild_tool, ret = FALSE, pthread_join_ret;

    /* 无法确定 initramfs 内容时显示的提示信息 */
    const char *no_listing = "Unable to determine whether NVIDIA kernel "
                             "modules are present in the initramfs. Existing "
                             "NVIDIA kernel modules in the initramfs, if any, "
                             "may interfere with the newly installed driver.";

    /* 用户选择：不重建 / 重建 */
    const char * const choices[] = {
                                       "Do not rebuild initramfs",
                                       "Rebuild initramfs"
                                   };
    ScanThreadData data = {}, *data_pointer;
    char *reason;

    /* 在交互模式下查找可用的 initramfs 重建工具 */
    rebuild_tool = find_initramfs_tool(op, INITRAMFS_REBUILD_TOOL, INTERACTIVE);

    /* 处理用户通过命令行明确指定的 initramfs 重建行为 */
    if (op->rebuild_initramfs != NV_OPTIONAL_BOOL_DEFAULT) {
        if (op->rebuild_initramfs == NV_OPTIONAL_BOOL_TRUE) {
            /* 用户请求重建 initramfs */
            if (rebuild_tool >= 0) {
                ret = run_initramfs_tool(op, rebuild_tool, NULL, INTERACTIVE)
                      == 0;
            } else {
                ui_warn(op, "An initramfs rebuild was requested on the "
                            "installer command line, but a suitable tool was "
                            "not found.");
            }
        } else {
            /* 用户明确要求跳过重建 */
            ret = TRUE;
            ui_log(op, "Skipping initramfs rebuild.");
        }

        goto done;
    }

    /* 等待后台扫描线程完成，获取扫描结果 */
    pthread_join_ret = pthread_join(scan_thread, (void **) &data_pointer);

    if (pthread_join_ret != 0) {
        /* pthread_join 失败，使用本地初始化的空数据 */
        data_pointer = &data;
    }

    /* 如果之前的非交互式扫描标记了需要重试，则在交互模式下重新扫描 */
    if (data_pointer->try_scan_again) {
        data_pointer->tool = find_initramfs_tool(op, INITRAMFS_LIST_TOOL,
                                                 INTERACTIVE);

        scan_initramfs(op, data_pointer, INTERACTIVE);
    }

    /* 收集需要重建 initramfs 的原因列表 */
    reason = nvstrdup("");

    if (nouveau_is_present()) {
        /* 系统中存在 Nouveau 模块（安装器已尝试禁用） */
        add_bullet_list_item("nvidia-installer attempted to disable Nouveau.",
                             &reason);
    }

    if (data_pointer->nouveau_ko_detected) {
        /* initramfs 中包含 Nouveau 模块 */
        add_bullet_list_item("Nouveau is present in the initramfs.", &reason);
    }

    if (data_pointer->nvidia_ko_detected) {
        /* initramfs 中包含旧的 NVIDIA 内核模块 */
        add_bullet_list_item("An NVIDIA kernel module was found in the "
                             "initramfs.", &reason);
    }

    /* 根据检测结果和工具可用性，决定下一步操作 */
    if (rebuild_tool >= 0) {
        /* 有可用的重建工具 */
        int rebuild = FALSE;

        if (reason[0]) {
            /* 检测到需要重建的原因，建议用户重建（默认选择"重建"） */
            rebuild = ui_multiple_choice(op, choices, 2, 1,
                                         "The initramfs will likely need to be "
                                         "rebuilt due to the following "
                                         "condition(s):\n%s\n"
                                         "Would you like to rebuild the "
                                         "initramfs?", reason);
        } else if (data_pointer->scan_complete) {
            /* 扫描完成且未检测到冲突模块，无需重建 */
            ui_log(op, "No NVIDIA modules detected in the initramfs.");
            ret = TRUE;
        } else {
            /* 扫描未完成（无法确定内容），询问用户是否重建（默认选择"不重建"） */
            rebuild = ui_multiple_choice(op, choices, 2, 0,
                                         "%s Would you like to rebuild "
                                         "the initramfs?", no_listing);
        }

        if (rebuild) {
            /* 用户选择重建 */
            ret = run_initramfs_tool(op, rebuild_tool, NULL, INTERACTIVE) == 0;

            if (!ret) {
                ui_error(op, "Failed to rebuild the initramfs!");
            }
        } else {
            /* 用户选择不重建 */
            ui_log(op, "The initramfs will not be rebuild.");
            ret = TRUE;
        }
    } else if (reason[0]) {
        /* 没有找到重建工具，但检测到需要重建的原因——警告用户手动处理 */
        ui_warn(op, "nvidia-installer was unable to locate a tool for "
                    "rebuilding the initramfs, which is strongly recommended "
                    "due to the following condition(s):\n%s\n"
                    "Please consult your distribution's documentation for "
                    "instructions on how to rebuild the initramfs.", reason);
        ret = TRUE;
    } else if (!data_pointer->scan_complete) {
        /* 没有重建工具，也无法确定 initramfs 内容——通知用户 */
        ui_message(op, "%s", no_listing);
        ret = TRUE;
    }

    nvfree(reason);

done:
    return ret;
}
