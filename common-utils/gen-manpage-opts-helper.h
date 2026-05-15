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
 * 文件说明: gen-manpage-opts-helper.h
 *
 * 本头文件声明了 man 手册页选项生成辅助函数的接口。
 *
 * gen_manpage_opts_helper() 函数读取 NVGetoptOption 选项表，
 * 将选项信息转换为 *roff 格式并输出到 stdout，
 * 用于自动生成 man 手册页的 "OPTIONS" 和 "ADVANCED OPTIONS" 章节。
 *
 * 依赖：
 *   - nvgetopt.h（NVGetoptOption 结构体定义）
 */

#if !defined(__GEN_MANPAGE_OPTS_HELPER_H__)
#define __GEN_MANPAGE_OPTS_HELPER_H__

#include "nvgetopt.h"

/*
 * gen_manpage_opts_helper() - 根据选项表生成 *roff 格式的手册页选项部分。
 *
 * 参数：
 *   options - NVGetoptOption 选项数组（以 name==NULL 的哨兵条目结尾）
 *
 * 输出：
 *   将 *roff 格式的 OPTIONS 和 ADVANCED OPTIONS 章节内容输出到 stdout
 */
void gen_manpage_opts_helper(const NVGetoptOption *options);

#endif /* __GEN_MANPAGE_OPTS_HELPER_H__ */
