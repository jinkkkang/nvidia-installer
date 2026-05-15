/*
 * Copyright (C) 2004-2010 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * 文件说明: nvgetopt.h
 *
 * 本头文件定义了 NVIDIA 命令行参数解析库的接口，包括：
 *
 * 1. 布尔常量：
 *    - NVGETOPT_TRUE / NVGETOPT_FALSE
 *
 * 2. 选项标志位（flags）定义：
 *    - NVGETOPT_IS_BOOLEAN         : 布尔选项（支持 --no- 取反）
 *    - NVGETOPT_STRING_ARGUMENT    : 接受字符串参数
 *    - NVGETOPT_INTEGER_ARGUMENT   : 接受整数参数
 *    - NVGETOPT_DOUBLE_ARGUMENT    : 接受浮点数参数
 *    - NVGETOPT_HAS_ARGUMENT       : 上述三种参数类型的合并掩码
 *    - NVGETOPT_ALLOW_DISABLE      : 允许通过 --no- 前缀禁用
 *    - NVGETOPT_ARGUMENT_IS_OPTIONAL: 参数可选
 *    - NVGETOPT_HELP_ALWAYS        : 总是在帮助中显示
 *    - NVGETOPT_IS_DEPRECATED      : 已弃用选项
 *    - NVGETOPT_DISABLE_IS_INVALID : --no- 形式无效
 *    - NVGETOPT_ENABLE_IS_INVALID  : 启用形式无效
 *    - NVGETOPT_UNUSED_FLAG_RANGE  : 供用户自定义使用的高 16 位
 *
 * 3. NVGetoptOption 结构体：
 *    - 描述单个命令行选项的全部属性
 *
 * 4. nvgetopt() 函数：
 *    - 核心参数解析函数
 *
 * 5. nvgetopt_print_help() 函数和回调类型：
 *    - 生成帮助文本的辅助函数
 */

#ifndef __NVGETOPT_H__
#define __NVGETOPT_H__

/* 布尔常量 */
#define NVGETOPT_FALSE 0
#define NVGETOPT_TRUE  1


/*
 * NVGETOPT_UNUSED_FLAG_RANGE - 未被 nvgetopt 使用的 flags 位范围。
 *
 * NVGetoptOption::flags 的高 16 位（0xffff0000）未被 nvgetopt 库内部使用，
 * 可供 nvgetopt 的使用者自定义用途。例如用于标记选项的分组、
 * 可见性控制等。
 */

#define NVGETOPT_UNUSED_FLAG_RANGE 0xffff0000

/*
 * NVGETOPT_IS_BOOLEAN - 标记选项为布尔类型。
 *
 * 选项出现时解释为 TRUE；如果以 '--no-' 前缀出现，则解释为 FALSE。
 * 解析成功时，nvgetopt 通过 boolval 参数返回布尔值。
 */

#define NVGETOPT_IS_BOOLEAN             0x0001


/*
 * NVGETOPT_STRING_ARGUMENT - 标记选项接受字符串参数。
 *
 * 选项需要一个字符串类型的参数值。
 * 解析成功时，nvgetopt 通过 strval 参数返回字符串值。
 */

#define NVGETOPT_STRING_ARGUMENT        0x0002


/*
 * NVGETOPT_INTEGER_ARGUMENT - 标记选项接受整数参数。
 *
 * 选项需要一个整数类型的参数值。
 * 解析成功时，nvgetopt 通过 intval 参数返回整数值。
 */

#define NVGETOPT_INTEGER_ARGUMENT       0x0004


/*
 * NVGETOPT_DOUBLE_ARGUMENT - 标记选项接受双精度浮点数参数。
 *
 * 选项需要一个双精度浮点数类型的参数值。
 * 解析成功时，nvgetopt 通过 doubleval 参数返回浮点数值。
 */

#define NVGETOPT_DOUBLE_ARGUMENT        0x0008


/*
 * NVGETOPT_HAS_ARGUMENT - 辅助宏，表示选项需要某种参数。
 *
 * 由 STRING_ARGUMENT、INTEGER_ARGUMENT 和 DOUBLE_ARGUMENT 三个标志
 * 进行按位或运算而来。用于快速判断选项是否需要参数。
 */

#define NVGETOPT_HAS_ARGUMENT (NVGETOPT_STRING_ARGUMENT | \
                               NVGETOPT_INTEGER_ARGUMENT | \
                               NVGETOPT_DOUBLE_ARGUMENT)

/*
 * NVGETOPT_ALLOW_DISABLE - 标记选项可以被 "--no-" 前缀禁用。
 *
 * 当选项通常需要参数时，使用 "--no-" 前缀可以不提供参数来禁用该选项。
 * 如果选项被禁用，nvgetopt 通过 disable_val 返回 TRUE。
 *
 * 注意：NVGETOPT_ALLOW_DISABLE 只能与需要参数的选项一起使用。
 */

#define NVGETOPT_ALLOW_DISABLE          0x0010


/*
 * NVGETOPT_ARGUMENT_IS_OPTIONAL - 标记选项的参数为可选。
 *
 * 如果没有提供参数（已到达 argv 列表末尾，或下一个 argv 以 '-' 开头），
 * 则该选项会在没有参数的情况下正常返回。
 */

#define NVGETOPT_ARGUMENT_IS_OPTIONAL   0x0020


/*
 * NVGETOPT_HELP_ALWAYS - 标记选项始终在帮助信息中显示。
 *
 * 此标志不被 nvgetopt() 本身使用，而是供使用 NVGetoptOption 表的
 * 其他代码（如帮助信息打印）使用。通常用于区分"基本选项"（--help 显示）
 * 和"高级选项"（--advanced-help 显示）。
 */

#define NVGETOPT_HELP_ALWAYS            0x0040


/*
 * NVGETOPT_IS_DEPRECATED - 标记选项已弃用。
 *
 * NVGetoptOption 的 description 字段（如果设置）可以用于存储
 * 解释该选项为何不再使用的文本。
 */

#define NVGETOPT_IS_DEPRECATED          0x0080

/*
 * NVGETOPT_DISABLE_IS_INVALID / NVGETOPT_ENABLE_IS_INVALID
 *
 * 这些标志指示某个布尔值或可禁用值的特定状态不再被支持。
 * 可以与 NVGETOPT_IS_DEPRECATED 一起使用，以区分：
 *   - 被忽略的弃用布尔值或启用/禁用状态
 *   - 无效的不受支持的布尔值或启用/禁用状态
 *
 * DISABLE_IS_INVALID : --no-xxx 形式无效（禁用不被支持）
 * ENABLE_IS_INVALID  : --xxx 形式无效（启用不被支持）
 */

#define NVGETOPT_DISABLE_IS_INVALID     0x0100
#define NVGETOPT_ENABLE_IS_INVALID      0x0200

/*
 * NVGetoptOption 结构体 - 描述单个命令行选项。
 *
 * 成员：
 *   name        - 长选项名（如 "version"，使用时为 --version）
 *   val         - 选项的标识值；对于短选项，通常设为对应的字符（如 'v'）
 *   flags       - 选项标志位的按位或组合（上面定义的 NVGETOPT_* 常量）
 *   arg_name    - 参数名（用于帮助文本显示，如 "FILE"），不被 nvgetopt() 使用
 *   description - 选项描述文本（用于帮助信息），不被 nvgetopt() 使用
 *
 * 选项表的最后一个条目必须是 name == NULL 的哨兵条目。
 */
typedef struct {
    const char *name;
    int val;
    unsigned int flags;
    const char *arg_name;     /* nvgetopt() 不使用此字段 */
    const char *description;  /* nvgetopt() 不使用此字段 */
} NVGetoptOption;


/*
 * nvgetopt() - 命令行参数解析函数。
 *
 * 用法描述参见 glibc 的 getopt_long(3) 手册页。
 * 选项可以使用 "--"、"-" 或 "--no-" 前缀。
 *
 * 内部使用静态全局变量存储当前 argv 数组的索引位置，
 * 因此对 nvgetopt() 的后续调用会依次遍历 argv[]。
 *
 * 成功时返回匹配的 NVGetoptOption.val 值。
 *
 * 如果设置了 NVGETOPT_IS_BOOLEAN 标志，boolval 会被设为 TRUE
 * （或者如果选项字符串以 "--no-" 开头则设为 FALSE）。
 *
 * 如果选项字符串以 "--no-" 开头，disable_val 被设为 TRUE，
 * 否则设为 FALSE。
 *
 * 如果参数被成功解析，strval、intval 或 doubleval 中的一个会被赋值，
 * 取决于选项 flags 中设置了 NVGETOPT_STRING_ARGUMENT、
 * NVGETOPT_INTEGER_ARGUMENT 还是 NVGETOPT_DOUBLE_ARGUMENT。
 * 如果 strval 被赋值为非 NULL，则调用者负责在使用完毕后释放该字符串。
 *
 * 失败时打印错误到 stderr 并返回 0。
 *
 * 当没有更多选项需要解析时返回 -1。
 */

int nvgetopt(int argc,
             char *argv[],
             const NVGetoptOption *options,
             char **strval,
             int *boolval,
             int *intval,
             double *doubleval,
             int *disable_val);

/*
 * nvgetopt_print_help() - 为选项表中的每个选项打印帮助信息。
 *
 * 功能：遍历 NVGetoptOption 选项数组，为每个选项生成名称字符串和描述字符串，
 *       然后通过回调函数传递给调用者进行实际的格式化和打印。
 *       适用于实现工具的 "--help" 输出。
 *
 * 选项过滤：只有 flags 中包含 include_mask 中所有位的选项才会被显示。
 *
 * 回调函数会收到两个字符串参数：
 *   - name：选项名字符串，例如 "-v, --version"
 *   - description：选项描述字符串
 *
 * 参数：
 *   options      - NVGetoptOption 选项数组（以 name==NULL 结尾）
 *   include_mask - 位掩码，用于过滤要显示的选项
 *   callback     - 回调函数指针，负责实际的输出
 */

typedef void nvgetopt_print_help_callback_ptr(const char *name,
                                              const char *description);

void nvgetopt_print_help(const NVGetoptOption *options,
                         unsigned int include_mask,
                         nvgetopt_print_help_callback_ptr callback);

#endif /* __NVGETOPT_H__ */
