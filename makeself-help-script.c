/*
 * Implements the '--help-args-only' and '--advanced-options-args-only'
 * nvidia-installer command line options for use by makeself when
 * generating the .run file during the 'dist' step.
 *
 * 【文件说明】生成 .run 自解压安装包的帮助脚本（独立可执行程序）。
 *
 * NVIDIA 驱动通过 makeself 工具打包为 .run 格式的自解压安装包。
 * .run 包本身支持 --help 和 --advanced-options 选项，用于在解压前
 * 显示安装器的可用命令行选项。这些帮助文本需要在打包（dist 步骤）时
 * 预先生成并嵌入到 .run 包中。
 *
 * 本程序的作用就是在构建时生成这些帮助文本：
 *   - --help-args-only：输出基本选项帮助（带 NVGETOPT_HELP_ALWAYS 标志的选项）
 *   - --advanced-options-args-only：输出所有选项帮助（包括高级选项）
 *
 * 输出格式为纯文本，宽度固定为 65 列（不依赖终端宽度），
 * 适合嵌入到 makeself.sh 脚本中。
 *
 * 与 gen-manpage-opts.c 的区别：
 *   - gen-manpage-opts.c 输出 *roff 格式（用于 man 页面）
 *   - 本程序输出纯文本格式（用于 .run 包的 --help 输出）
 */

#include <stdlib.h>              /* exit() */
#include <stdio.h>               /* fprintf, stderr */
#include <string.h>              /* strcmp() */

#include "nvidia-installer.h"    /* nvidia-installer 全局定义 */
#include "nvgetopt.h"            /* NVGetoptOption 类型和 nvgetopt_print_help() */
#include "option_table.h"        /* __options[] 选项表定义 */
#include "msg.h"                 /* nv_info_msg() 格式化输出，TAB/BIGTAB 缩进常量 */

/*
 * print_usage() - 打印程序用法说明到标准错误。
 *
 * 参数：
 *   argv - 命令行参数数组（用于显示程序名）
 */
static void print_usage(char **argv)
{
    fprintf(stderr, "usage: %s --help-args-only|"
            "--advanced-options-args-only\n", argv[0]);
}

/*
 * print_help_helper() - 格式化输出单个选项的帮助信息。
 *
 * 作为回调函数传递给 nvgetopt_print_help()，为每个选项生成两行输出：
 *   第 1 行：选项名称（前缀一个 TAB 缩进）
 *   第 2 行：选项描述（前缀一个 BIGTAB 双倍缩进）
 *   第 3 行：空行（用于分隔不同选项）
 *
 * 参数：
 *   name        - 选项名称字符串（例如 "--no-opengl-files"）
 *   description - 选项描述文本
 */
static void print_help_helper(const char *name, const char *description)
{
    nv_info_msg(TAB, name);
    nv_info_msg(BIGTAB, description);
    nv_info_msg(NULL, "");
}

/*
 * main() - 程序入口点。
 *
 * 根据命令行参数决定输出哪些选项的帮助文本：
 *   --help-args-only            : 仅输出基本选项（日常使用的选项）
 *   --advanced-options-args-only : 输出所有选项（包括高级/调试选项）
 *
 * 参数：
 *   argc - 命令行参数个数（必须为 2）
 *   argv - 命令行参数数组
 *
 * 返回值：0 表示成功，1 表示参数错误。
 */
int main(int argc, char **argv)
{
    unsigned int include_mask = 0;

    /* 本程序只接受一个参数 */
    if (argc != 2) {
        print_usage(argv);
        exit(1);
    }

    /*
     * 输出帮助文本是为了嵌入到 makeself.sh 脚本中，而非在终端显示。
     * 因此将输出宽度硬编码为 65 列，不依赖于当前终端的实际宽度。
     */
    reset_current_terminal_width(65);

    if (strcmp(argv[1], "--help-args-only") == 0) {
        /* 仅输出带有 NVGETOPT_HELP_ALWAYS 标志的基本选项 */
        include_mask = NVGETOPT_HELP_ALWAYS;
    } else if (strcmp(argv[1], "--advanced-options-args-only") == 0) {
        /* 输出所有选项（include_mask=0 表示不过滤，显示全部） */
        include_mask = 0;
    } else {
        print_usage(argv);
        exit(1);
    }

    /* 遍历 __options 数组并使用 print_help_helper 回调输出每个选项 */
    nvgetopt_print_help(__options, include_mask, print_help_helper);

    return 0;
}
