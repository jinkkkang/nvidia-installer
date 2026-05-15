/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
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
 * 文件说明: gen-manpage-opts-helper.c
 *
 * 本文件实现了 man 手册页选项部分的自动生成功能。
 * 它读取 NVGetoptOption 选项表，将每个选项转换为 *roff 格式的手册页文本，
 * 并输出到 stdout。
 *
 * 主要功能：
 *
 * 1. print_option() - 将单个选项转换为 *roff 格式输出：
 *    - 生成 .TP（缩进段落）和 .BI（粗体+斜体）格式的选项名
 *    - 处理短选项、长选项、参数名、布尔取反形式
 *    - 将描述文本转换为 *roff 格式，处理特殊字符：
 *      '&' 切换斜体，'^' 切换粗体，'-' 需要转义为 "\-"
 *
 * 2. gen_manpage_opts_helper() - 生成完整的选项部分：
 *    - 先输出 "OPTIONS" 章节（带有 NVGETOPT_HELP_ALWAYS 标志的选项）
 *    - 再输出 "ADVANCED OPTIONS" 章节（其余选项）
 *
 * 输出格式为 *roff (troff/groff) 宏语言，用于生成 Unix man 手册页。
 *
 * *roff 格式说明：
 *   .SH  : 章节标题（Section Header）
 *   .TP  : 带缩进的段落（Tagged Paragraph），选项名在标签位置
 *   .BI  : 交替使用粗体和斜体（Bold Italic）
 *   .I   : 斜体文本
 *   .B   : 粗体文本
 *   \-   : 连字符的转义形式（避免被解释为减号）
 */

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "nvgetopt.h"
#include "gen-manpage-opts-helper.h"
#include "common-utils.h"

/*
 * print_option() - 将单个 NVGetoptOption 输出为 *roff 格式的手册页文本。
 *
 * 功能：生成一个选项的完整 *roff 格式表示，包括选项名和描述文本。
 *
 * 输出格式示例：
 *   .TP
 *   .BI "\-c " "ARG" ", \-\-config=ARG, \-\-no\-config"
 *   Description text here.
 *
 * 处理流程：
 *   1. 准备参数名（使用 arg_name 或将选项名转为大写）
 *   2. 输出 .TP 和 .BI 宏
 *   3. 输出短选项格式（如果有，如 "\-c"）
 *   4. 输出长选项格式（如 "\-\-config"）
 *   5. 输出参数（如果有，如 "=ARG"）
 *   6. 输出布尔取反形式（如果适用，如 "\-\-no\-config"）
 *   7. 逐字符输出描述文本，处理特殊格式字符
 *
 * 描述文本中的特殊字符处理：
 *   '&' : 切换斜体模式（开启时输出 ".I "，关闭时输出换行）
 *   '^' : 切换粗体模式（开启时输出 ".B "，关闭时输出换行）
 *   '-' : 转义为 "\-"（*roff 中连字符需要转义）
 *   ' ' : 在斜体/粗体模式结束后的空格被省略
 *   '\n': 输出换行并重置 firstchar 标志
 *   '.' : 不能出现在行首（*roff 会将行首的 '.' 解释为宏命令）
 *   '\'': 不能出现在行首（*roff 会将行首的 '\'' 解释为注释）
 *
 * 参数：
 *   o - 指向 NVGetoptOption 结构体的指针
 */
static void print_option(const NVGetoptOption *o)
{
    char scratch[64];
    const char *s;
    int j, len;

    int italics, bold, omitWhiteSpace, firstchar;

    /* 如果选项需要参数，准备参数名字符串 */
    if (o->flags & NVGETOPT_HAS_ARGUMENT) {
        if (o->arg_name) {
            /* 使用选项指定的参数名 */
            strcpy(scratch, o->arg_name);
        } else {
            /* 将选项名转为大写作为参数名（如 "output" -> "OUTPUT"） */
            len = strlen(o->name);
            for (j = 0; j < len; j++) scratch[j] = toupper(o->name[j]);
            scratch[len] = '\0';
        }
    }

    /* 输出 *roff 的 .TP（缩进段落）和 .BI（粗体斜体交替）宏 */
    printf(".TP\n.BI \"");
    /* 输出选项名称 */
    /* 注意：此处应该对 o->name 中的 '-' 字符进行反斜杠转义（未实现） */

    if (isalpha(o->val)) {
        /* 输出短选项格式: '\-c' */
        printf("\\-%c", o->val);

        if (o->flags & NVGETOPT_HAS_ARGUMENT) {
            /* 输出短选项的参数格式: ' " "ARG" "' */
            printf(" \" \"%s\" \"", scratch);
        }
        /* 输出分隔符: ', ' */
        printf(", ");
    }

    /* 输出长选项格式: '\-\-name' */
    printf("\\-\\-%s", o->name);

    /* 输出长选项的参数格式: '=" "ARG' */
    if (o->flags & NVGETOPT_HAS_ARGUMENT) {
        printf("=\" \"%s", scratch);

        /* 如果是布尔或可禁用选项，输出额外的分隔空间 */
        if ((o->flags & NVGETOPT_IS_BOOLEAN) ||
            (o->flags & NVGETOPT_ALLOW_DISABLE)) {
            printf("\" \"");
        }
    }

    /* 输出布尔取反形式: ', \-\-no\-name' */
    if (((o->flags & NVGETOPT_IS_BOOLEAN) &&
         !(o->flags & NVGETOPT_HAS_ARGUMENT)) ||
        (o->flags & NVGETOPT_ALLOW_DISABLE)) {
        printf(", \\-\\-no\\-%s", o->name);
    }

    /* 结束 .BI 宏 */
    printf("\"\n");

    /*
     * 输出选项描述文本。
     *
     * 逐字符处理描述字符串（虽然效率不高），以便对特殊字符进行处理：
     *
     * '&' : 切换斜体开/关
     * '^' : 切换粗体开/关
     * '-' : 需要反斜杠转义为 "\-"
     * '\n': 重置行首标志
     * '.' : 不能出现在行首（*roff 将行首 '.' 解释为宏命令）
     * '\'': 不能出现在行首
     *
     * 当斜体或粗体开启时，结尾的空格会被省略
     */

    italics = FALSE;       /* 斜体模式是否开启 */
    bold = FALSE;          /* 粗体模式是否开启 */
    omitWhiteSpace = FALSE; /* 是否跳过空格（格式切换后） */
    firstchar = TRUE;      /* 是否在行首 */

    for (s = o->description; s && *s; s++) {

        switch (*s) {
          case '&':
              /* 切换斜体模式 */
              if (italics) {
                  printf("\n");         /* 关闭斜体 */
              } else {
                  printf("\n.I ");      /* 开启斜体（.I 宏） */
              }
              omitWhiteSpace = italics; /* 关闭时跳过后续空格 */
              firstchar = italics;      /* 关闭时标记为行首 */
              italics = !italics;
              break;
          case '^':
              /* 切换粗体模式 */
              if (bold) {
                  printf("\n");         /* 关闭粗体 */
              } else {
                  printf("\n.B ");      /* 开启粗体（.B 宏） */
              }
              omitWhiteSpace = bold;    /* 关闭时跳过后续空格 */
              firstchar = bold;         /* 关闭时标记为行首 */
              bold = !bold;
              break;
          case '-':
              /* 连字符需要转义为 "\-" 以避免 *roff 将其解释为减号 */
              printf("\\-");
              omitWhiteSpace = FALSE;
              firstchar = FALSE;
              break;
          case ' ':
              /* 在格式切换后的空格可能被省略 */
              if (!omitWhiteSpace) {
                  printf(" ");
                  firstchar = FALSE;
              }
              break;
          case '\n':
              /* 换行符重置行首标志 */
              printf("\n");
              omitWhiteSpace = FALSE;
              firstchar = TRUE;
              break;
          case '.':
              /*
               * '.' 不能出现在行首，因为 *roff 会将其解释为宏命令。
               * 如果出现在行首，说明描述文本中的 '&' 或 '^' 格式标记
               * 导致了不合法的 *roff 输出，报错退出。
               */
              if (firstchar) {
                  fprintf(stderr, "Error: *roff can't start a line with '.' "
                          "If you used '&' or '^' to format text in the "
                          "description of the '%s' option, please add some "
                          "text before the end of the sentence, so that a "
                          "valid manpage can be generated.\n", o->name);
                  exit(1);
              }
              /* 如果不在行首，正常输出（落入 default 分支） */
              /* fall through */
          case '\'':
              /*
               * '\'' 也不能出现在行首，原因同上。
               */
              if (firstchar) {
                  fprintf(stderr, "Error: *roff can't start a line with '\''. "
                          "If you started a line with '\'' in the description "
                          "of the '%s' option, please add some text at the "
                          "beginning of the sentence, so that a valid manpage "
                          "can be generated.\n", o->name);
                  exit(1);
              }
              /* fall through */
          default:
              /* 普通字符直接输出 */
              printf("%c", *s);
              omitWhiteSpace = FALSE;
              firstchar = FALSE;
              break;
        }
    }

    /* 输出选项描述的结束换行 */
    printf("\n");
}

/*
 * gen_manpage_opts_helper() - 生成完整的 man 手册页选项部分。
 *
 * 功能：遍历 NVGetoptOption 选项表，生成 *roff 格式的 "OPTIONS" 和
 *       "ADVANCED OPTIONS" 章节。
 *
 * 选项分类：
 *   - "基本选项"（OPTIONS）：带有 NVGETOPT_HELP_ALWAYS 标志的选项，
 *     这些是用户通过 --help 就能看到的常用选项
 *   - "高级选项"（ADVANCED OPTIONS）：没有 NVGETOPT_HELP_ALWAYS 标志的选项，
 *     这些通常需要通过 --advanced-help 才能看到
 *
 * 处理流程：
 *   1. 输出 .SH OPTIONS 章节标题
 *   2. 遍历选项表，输出所有 NVGETOPT_HELP_ALWAYS 选项
 *   3. 如果存在高级选项，输出 .SH "ADVANCED OPTIONS" 章节标题
 *   4. 遍历选项表，输出所有非 NVGETOPT_HELP_ALWAYS 的选项
 *
 * 参数：
 *   options - NVGetoptOption 选项数组（以 name==NULL 结尾）
 *
 * 注意：没有 description 的选项会被跳过（不会出现在手册页中）
 */
void gen_manpage_opts_helper(const NVGetoptOption *options)
{
    int i;
    int has_advanced_options = 0;

    /* 输出基本选项章节：即通过 --help 可以看到的选项 */
    printf(".SH OPTIONS\n");
    for (i = 0; options[i].name; i++) {
        const NVGetoptOption *o = &options[i];

        /* 跳过没有描述的选项 */
        if (!o->description) {
            continue;
        }

        /* 跳过非 HELP_ALWAYS 的选项（这些属于高级选项） */
        if (!(o->flags & NVGETOPT_HELP_ALWAYS)) {
            has_advanced_options = 1;
            continue;
        }

        print_option(o);
    }

    if (has_advanced_options) {
        /*
         * 如果存在高级选项，输出高级选项章节：
         * 即通过 --advanced-help 可以看到的选项
         */
        printf(".SH \"ADVANCED OPTIONS\"\n");
        for (i = 0; options[i].name; i++) {
            const NVGetoptOption *o = &options[i];

            /* 跳过没有描述的选项 */
            if (!o->description) {
                continue;
            }

            /* 跳过 HELP_ALWAYS 的选项（已在上面输出） */
            if (o->flags & NVGETOPT_HELP_ALWAYS) {
                continue;
            }

            print_option(o);
        }
    }
}
