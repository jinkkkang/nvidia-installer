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
 *
 *
 * nvgetopt.c - 可移植的 getopt_long() 替代实现；
 * 不需要使用烦人的 optstring 参数。
 */

/*
 * 文件说明: nvgetopt.c
 *
 * 本文件实现了一个功能增强的命令行参数解析库，类似于 getopt_long()，
 * 但提供了更丰富的功能。主要包含以下函数：
 *
 * 1. nvgetopt() - 核心参数解析函数：
 *    - 支持长选项（--option）和短选项（-o）
 *    - 支持布尔选项（--option / --no-option）
 *    - 支持字符串、整数、浮点数类型的参数
 *    - 支持可选参数
 *    - 支持 "--no-" 前缀来禁用选项
 *    - 支持多个短选项合并（如 -abc）
 *    - 支持短选项与整数参数紧贴（如 -j8）
 *    - 使用静态变量维护解析位置，多次调用可遍历所有参数
 *
 * 2. cook_description() - 描述文本预处理函数：
 *    - 移除描述字符串中的特殊格式字符（& 和 ^）
 *    - 这些字符是 manpage 生成器使用的格式标记
 *
 * 3. nvgetopt_print_help() - 帮助信息打印函数：
 *    - 遍历选项表，为每个选项生成格式化的帮助文本
 *    - 支持通过 include_mask 过滤要显示的选项
 *    - 使用回调函数让调用者控制实际的输出格式
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>

#include "nvgetopt.h"
#include "common-utils.h"


/*
 * nvgetopt() - 命令行参数解析主函数。
 *
 * 功能：类似于 glibc 的 getopt_long(3)，但不需要构造 optstring 参数。
 *       通过内部静态变量 argv_index 维护当前解析位置，
 *       每次调用解析一个选项并推进 argv_index。
 *
 * 支持的选项格式：
 *   - 长选项：  --option, --option=value, --no-option
 *   - 短选项：  -o, -o value
 *   - 合并短选项：-abc（等价于 -a -b -c）
 *   - 短选项+整数：-j8（等价于 -j 8）
 *   - "--" 单独出现时终止选项解析
 *
 * 参数：
 *   argc       - 命令行参数个数（与 main 的 argc 相同）
 *   argv       - 命令行参数数组（与 main 的 argv 相同）
 *   options    - NVGetoptOption 选项表（以 name==NULL 的条目结尾）
 *   strval     - 输出参数，用于接收字符串类型的选项值（可以为 NULL）
 *   boolval    - 输出参数，用于接收布尔类型的选项值（可以为 NULL）
 *   intval     - 输出参数，用于接收整数类型的选项值（可以为 NULL）
 *   doubleval  - 输出参数，用于接收浮点类型的选项值（可以为 NULL）
 *   disable_val - 输出参数，指示选项是否被 "--no-" 禁用（可以为 NULL）
 *
 * 返回值：
 *   成功时返回匹配的 NVGetoptOption.val 值
 *   没有更多选项时返回 -1
 *   出错时返回 0
 *
 * 注意：
 *   - argv_index 是静态变量，程序生命周期内持续递增
 *   - 如果 strval 被赋值为非 NULL，调用者负责释放该字符串
 */

int nvgetopt(int argc,
             char *argv[],
             const NVGetoptOption *options,
             char **strval,
             int *boolval,
             int *intval,
             double *doubleval,
             int *disable_val)
{
    char *c, *a, *arg, *name = NULL, *argument=NULL;
    int i, found = NVGETOPT_FALSE;
    int ret = 0;
    int negate = NVGETOPT_FALSE;      /* 是否以 "--no-" 前缀取反 */
    int disable = NVGETOPT_FALSE;     /* 是否禁用该选项 */
    int double_dash = NVGETOPT_FALSE; /* 是否使用了 "--" 前缀 */
    const NVGetoptOption *o = NULL;   /* 匹配的选项指针 */
    static int argv_index = 0;        /* 当前解析到的 argv 索引（静态，跨调用保持） */

    /* 初始化所有输出参数 */
    if (strval) *strval = NULL;
    if (boolval) *boolval = NVGETOPT_FALSE;
    if (intval) *intval = 0;
    if (doubleval) *doubleval = 0.0;
    if (disable_val) *disable_val = NVGETOPT_FALSE;

    argv_index++;

    /* 如果没有更多选项，返回 -1 */

    if (argv_index >= argc) return -1;

    /* 获取当前要处理的参数 */

    arg = strdup(argv[argv_index]);

    /* 查找 "--" 或 "-" 前缀，确定选项名起始位置 */

    if ((arg[0] == '-') && (arg[1] == '-')) {
        name = arg + 2;                /* 跳过 "--"，name 指向选项名 */
        double_dash = NVGETOPT_TRUE;
    } else if (arg[0] == '-') {
        name = arg + 1;                /* 跳过 "-"，name 指向选项名 */
    } else {
        fprintf(stderr, "%s: invalid option: \"%s\"\n", argv[0], arg);
        goto done;
    }

    /*
     * 如果选项名中包含 "="，则 "=" 后面的部分是参数值。
     * 将 "=" 替换为 '\0'，使 name 只包含选项名。
     */

    c = name;
    while (*c) {
        if (*c == '=') {
            argument = c + 1;    /* argument 指向 "=" 后面的参数值 */
            *c = '\0';           /* 截断选项名 */
            break;
        }
        c++;
    }

    /*
     * 处理特殊情况：
     * - 如果 "--" 后没有字符，且没有 "=" 参数，则终止选项处理
     * - 如果只有一个字符，作为短选项处理
     * - 如果有多个字符，作为长选项处理
     */

    if (name[0] == '\0') {
        if (double_dash && argument == NULL) { /* "--" 作为选项列表终止符 */
            ret = -1;
            goto done;
        }
    } else if (name[1] == '\0') { /* 短选项（单个字符） */
        /* 在选项表中查找匹配的短选项 */
        for (i = 0; options[i].name; i++) {
            if (options[i].val == name[0]) {
                o = &options[i];
                break;
            }
        }
    } else { /* 长选项（多个字符） */
        for (i = 0; options[i].name; i++) {
            const char *tmpname;
            int tmp_negate;

            /*
             * 如果此选项允许以 "--no-" 前缀取反
             * （IS_BOOLEAN 或 ALLOW_DISABLE 标志），
             * 则跳过参数中可能存在的 "no-" 前缀进行匹配
             */

            if ((options[i].flags & (NVGETOPT_IS_BOOLEAN |
                                     NVGETOPT_ALLOW_DISABLE)) &&
                (name[0] == 'n') &&
                (name[1] == 'o') &&
                (name[2] == '-')) {
                tmpname = name + 3;        /* 跳过 "no-" 前缀 */
                tmp_negate = NVGETOPT_TRUE;
            } else {
                tmpname = name;
                tmp_negate = NVGETOPT_FALSE;
            }

            if (strcmp(tmpname, options[i].name) == 0) {
                o = &options[i];
                negate = tmp_negate;
                break;
            }
        }
    }

    /*
     * 如果没有找到匹配的选项，尝试将其解释为多个短选项的合并。
     * 例如 "-abc" 可能等价于 "-a -b -c"，
     * 前提是每个字符都是一个有效的短选项。
     */

    if (!o) {
        /* 逐字符检查是否都是有效的短选项 */
        for (c = name; *c; c++) {
            found = NVGETOPT_FALSE;
            for (i = 0; options[i].name; i++) {
                if (options[i].val == *c) {
                    found = NVGETOPT_TRUE;
                    break;
                }
            }
            if (!found) break;
        }

        if (found) {

            /*
             * 所有字符都是有效的短选项，按此方式解释。
             * 只处理第一个短选项，然后修改 argv 中的当前条目，
             * 移除第一个字符，这样下次调用时会处理剩余的字符。
             */

            for (i = 0; options[i].name; i++) {
                if (options[i].val == name[0]) {

                    /*
                     * 不允许带参数的选项在合并模式下处理
                     */

                    if (options[i].flags & NVGETOPT_HAS_ARGUMENT) break;

                    /*
                     * 从 argv[argv_index] 中移除第一个短选项字符。
                     * 跳过 '-' 和可能的第二个 '-' 以及 '+'
                     */

                    a = argv[argv_index];
                    if (a[0] == '-') a++;
                    if (a[0] == '-') a++;
                    if (a[0] == '+') a++;

                    /* 将后续字符左移一位 */
                    while (a[0]) { a[0] = a[1]; a++; }

                    /*
                     * 回退 argv_index，这样下次调用会重新处理
                     * 这个（已缩短的）argv 条目
                     */

                    argv_index--;

                    o = &options[i];
                    break;
                }
            }
        }
    }

    /*
     * 如果仍然没有找到匹配的选项，尝试将其解释为
     * 短选项+紧跟的整数参数（如 "-j8"）。
     * 目前仅限于整数参数的短选项。
     */
    if (!o && intval) {

        /* 检查第一个字符之后的部分是否为整数 */
        int appendedInteger = NVGETOPT_FALSE;
        if ((name[0] != '\0') && (name[1] != '\0')) {
            char *endptr;
            strtol(name + 1, &endptr, 0);
            if (*endptr == '\0') {
                /*
                 * 第一个字符之后的所有字符都可以被 strtol(3) 解析，
                 * 说明这是一个紧跟的整数参数
                 */
                appendedInteger = NVGETOPT_TRUE;
            }
        }

        if (appendedInteger) {
            /* 在选项表中查找匹配的短选项（必须接受整数参数） */
            for (i = 0; options[i].name; i++) {
                if ((options[i].flags & NVGETOPT_INTEGER_ARGUMENT) == 0) {
                    continue;
                }
                if (options[i].val == name[0]) {
                    o = &options[i];
                    argument = name + 1;    /* 参数值紧跟在短选项字母后面 */
                    break;
                }
            }
        }
    }

    /* 如果最终没有找到匹配的选项，报错并返回 */

    if (!o) {
        fprintf(stderr, "%s: unrecognized option: \"%s\"\n", argv[0], arg);
        goto done;
    }


    /* 如果选项是布尔类型，将 !negate 作为布尔值记录 */

    if (o->flags & NVGETOPT_IS_BOOLEAN) {
        if (boolval) *boolval = !negate;
    }


    /*
     * 如果此选项标记为"可禁用"，并且使用了 "--no-" 前缀，
     * 则将其解释为禁用该选项
     */

    if ((o->flags & NVGETOPT_ALLOW_DISABLE) && (negate == NVGETOPT_TRUE)) {
        disable = NVGETOPT_TRUE;
    }


    /*
     * 如果选项需要参数（字符串、整数或浮点数），并且未被禁用，
     * 则需要解析参数值。参数值来源于：
     * 1. 选项名后面 "=" 后面的部分（已在前面提取到 argument 中）
     * 2. argv 中的下一个条目
     */

    if ((o->flags & NVGETOPT_HAS_ARGUMENT) && !disable) {
        if (argument) {
            /* 如果 "=" 后面是空字符串，报错 */
            if (!argument[0]) {
                fprintf(stderr, "%s: option \"%s\" requires an "
                        "argument.\n", argv[0], arg);
                goto done;
            }
        } else {

            /*
             * 如果参数是可选的，且满足以下条件之一：
             * - 已经到达 argv 列表末尾
             * - 下一个 argv 条目以 '-' 开头
             * 则认为没有提供参数
             */

            if ((o->flags & NVGETOPT_ARGUMENT_IS_OPTIONAL) &&
                ((argv_index == (argc - 1)) ||
                 (argv[argv_index + 1][0] == '-'))) {
                argument = NULL;
                goto argument_processing_done;
            } else {
                /* 从下一个 argv 条目获取参数值 */
                argv_index++;
                if (argv_index >= argc) {
                    fprintf(stderr, "%s: option \"%s\" requires an "
                            "argument.\n", argv[0], arg);
                    goto done;
                }
                argument = argv[argv_index];
            }
        }

        /* argument 现在是一个有效的字符串：解析它 */

        if ((o->flags & NVGETOPT_INTEGER_ARGUMENT) && (intval)) {

            /* 将参数解析为整数 */

            char *endptr;
            *intval = (int) strtol(argument, &endptr, 0);
            if (*endptr) {
                fprintf(stderr, "%s: \"%s\" is not a valid argument for "
                        "option \"%s\".\n", argv[0], argument, arg);
                goto done;
            }
        } else if ((o->flags & NVGETOPT_STRING_ARGUMENT) && (strval)) {

            /* 将参数作为字符串处理 */

            *strval = strdup(argument);
        } else if ((o->flags & NVGETOPT_DOUBLE_ARGUMENT) && (doubleval)) {

            /* 将参数解析为双精度浮点数 */

            char *endptr;
            *doubleval = (double) strtod(argument, &endptr);
            if (*endptr) {
                fprintf(stderr, "%s: \"%s\" is not a valid argument for "
                        "option \"%s\".\n", argv[0], argument, arg);
                goto done;
            }
        } else {
            fprintf(stderr, "%s: error while assigning argument for "
                    "option \"%s\".\n", argv[0], arg);
            goto done;
        }

    } else {

        /* 如果选项不需要参数但却提供了参数，报错 */

        if (argument) {
            fprintf(stderr, "%s: option \"%s\" does not take an argument, but "
                    "was given an argument of \"%s\".\n",
                    argv[0], arg, argument);
            goto done;
        }
    }

 argument_processing_done:

    ret = o->val;

    /* 落入 done 标签 */

 done:

    /* 设置 disable 输出参数 */
    if (disable_val) *disable_val = disable;

    free(arg);
    return ret;

} /* nvgetopt() */


/*
 * cook_description() - 预处理描述文本，移除 manpage 生成器使用的特殊格式字符。
 *
 * 功能：描述字符串中可能包含特殊标记字符（由 manpage 生成器解释）：
 *   - '&' : 用于切换斜体格式
 *   - '^' : 用于切换粗体格式
 *   在输出到终端帮助文本时，需要将这些字符移除。
 *
 * 处理流程：
 *   1. 复制描述字符串
 *   2. 逐字符遍历源字符串
 *   3. 跳过 '&' 和 '^' 字符，其他字符原样复制到目标
 *
 * 参数：
 *   description - 原始描述文本（可以为 NULL）
 *
 * 返回值：
 *   新分配的处理后的字符串，调用者负责释放；
 *   如果输入为 NULL，则返回 NULL
 */

static char *cook_description(const char *description)
{
    const char *src;
    char *s, *dst;

    if (!description) {
        return NULL;
    }

    s = strdup(description);

    if (!s) {
        return NULL;
    }

    /* 遍历源字符串，跳过 '&' 和 '^' 字符 */
    for (src = description, dst = s; *src; src++) {
        if ((*src == '&') || (*src == '^')) {
            continue;
        }
        *dst = *src;
        dst++;
    }

    *dst = '\0';

    return s;
}


/*
 * nvgetopt_print_help() - 为选项表中的每个选项打印帮助信息。
 *
 * 功能：遍历 NVGetoptOption 选项表，为每个选项生成格式化的帮助文本，
 *       然后通过回调函数输出。适用于实现 "--help" 功能。
 *
 * 处理流程：
 *   对于每个选项：
 *   1. 跳过没有描述文本的选项
 *   2. 使用 include_mask 过滤选项（选项的 flags 必须包含 mask 的所有位）
 *   3. 构建参数名：使用 arg_name（如果有）或将选项名转大写
 *   4. 构建选项名字符串：
 *      a. "--option" 或 "--option=ARG"（长选项格式）
 *      b. 如果有短选项，在前面加 "-o" 或 "-o ARG, "
 *      c. 如果是布尔/可禁用选项，在后面加 ", --no-option"
 *   5. 预处理描述文本（移除格式字符）
 *   6. 调用回调函数传递选项名和描述
 *
 * 参数：
 *   options      - NVGetoptOption 选项表（以 name==NULL 结尾）
 *   include_mask - 位掩码，只显示 flags 中包含所有这些位的选项
 *   callback     - 回调函数，接收格式化后的选项名和描述字符串
 *
 * 回调函数示例：
 *   name = "-v, --version"
 *   description = "Print usage information for the common commandline ..."
 */

void nvgetopt_print_help(const NVGetoptOption *options,
                         unsigned int include_mask,
                         nvgetopt_print_help_callback_ptr callback)
{
    const NVGetoptOption *o;
    int i;

    for (i = 0; options[i].name; i++) {

        char *msg = NULL, *arg = NULL, *description = NULL;

        o = &options[i];

        /* 跳过没有帮助文本的选项 */
        if (!o->description) {
            continue;
        }

        /* 跳过没有包含 include_mask 中所有位的选项 */
        if ((o->flags & include_mask) != include_mask) {
            continue;
        }

        /* 如果选项需要参数，准备参数名字符串 */
        arg = NULL;
        if (o->flags & NVGETOPT_HAS_ARGUMENT) {
            if (o->arg_name) {
                /* 使用选项定义中指定的参数名 */
                arg = strdup(o->arg_name);
            } else {
                /* 将选项名转为大写作为参数名（如 "output" -> "OUTPUT"） */
                char *tmp;
                arg = strdup(o->name);
                for (tmp = arg; tmp && *tmp; tmp++) {
                    *tmp = toupper(*tmp);
                }
            }
        }

        msg = NULL;

        /*
         * 构建长选项格式的字符串。
         * 例如 "--foo" 或 "--foo=BAR"
         */
        if (arg) {
            msg = nvstrcat("--", o->name, "=", arg, NULL);
        } else {
            msg = nvstrcat("--", o->name, NULL);
        }

        /*
         * 如果选项有对应的短选项（单字母），在前面添加短选项格式。
         * 例如 "-f BAR, --foo=BAR" 或 "-f, --foo"
         */
        if (o->val <= UCHAR_MAX && o->val >= 0 && isalpha(o->val)) {
            char scratch[16];
            char *tmp;
            snprintf(scratch, sizeof(scratch), "%c", o->val);
            if (arg) {
                tmp = nvstrcat("-", scratch, " ", arg, ", ", msg, NULL);
            } else {
                tmp = nvstrcat("-", scratch, ", ", msg, NULL);
            }
            free(msg);
            msg = tmp;
        }

        /*
         * 如果是布尔选项（无参数）或可禁用选项，
         * 在后面追加取反形式：", --no-foo"
         */
        if (((o->flags & NVGETOPT_IS_BOOLEAN) &&
             !(o->flags & NVGETOPT_HAS_ARGUMENT)) ||
            (o->flags & NVGETOPT_ALLOW_DISABLE)) {
            char *tmp = nvstrcat(msg, ", --no-", o->name, NULL);
            free(msg);
            msg = tmp;
        }

        /* 预处理描述文本（移除 & 和 ^ 等格式字符） */
        description = cook_description(o->description);

        /* 通过回调函数输出格式化后的选项名和描述 */
        callback(msg, description);

        free(msg);
        free(arg);
        free(description);
    }
}
