/*
 * Prints the option help in a form that is suitable to include in the manpage.
 *
 * 【文件说明】自动生成 man 页面选项章节的工具（独立可执行程序）。
 *
 * 本程序从 option_table.h 中定义的选项表（__options 数组）读取所有命令行选项的
 * 名称、参数和描述信息，然后以 *roff（man 页面标记语言）格式输出到标准输出。
 * 输出结果通常被重定向到文件，作为 nvidia-installer man 页面的一部分。
 *
 * 工作原理：
 *   1. option_table.h 定义了 __options[] 数组（NVGetoptOption 类型），
 *      包含所有 nvidia-installer 支持的命令行选项
 *   2. gen_manpage_opts_helper() 函数（定义在 common-utils/gen-manpage-opts-helper.c）
 *      遍历选项数组，将每个选项转换为 .TP/.BI 格式的 *roff 输出
 *   3. 选项被分为两组输出：
 *      - .SH OPTIONS：基本选项（带 NVGETOPT_HELP_ALWAYS 标志的选项）
 *      - .SH "ADVANCED OPTIONS"：高级选项（不带该标志的选项）
 *
 * 构建流程：此程序在构建时编译运行，其输出被嵌入到最终的 man 页面文件中。
 * 这种方式确保 man 页面的选项文档始终与实际代码中的选项定义保持同步。
 */
#include <stdio.h>      /* 标准输入输出 */
#include <ctype.h>       /* 字符分类函数 */
#include <string.h>      /* 字符串操作 */

#include "option_table.h"           /* 定义 __options[] 选项表（nvidia-installer 的所有命令行选项） */
#include "gen-manpage-opts-helper.h" /* gen_manpage_opts_helper() 函数声明 */

/*
 * main() - 程序入口点。
 *
 * 调用 gen_manpage_opts_helper() 将 __options 数组中的所有选项
 * 以 *roff 格式打印到标准输出。输出可被重定向到 .1 man 页面文件中。
 *
 * 返回值：始终返回 0（成功）。
 */
int main(void)
{
    gen_manpage_opts_helper(__options);
    return 0;
}
