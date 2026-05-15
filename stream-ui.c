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
 *
 * stream_ui.c - implementation of the nvidia-installer ui using printf
 * and friends.  This user interface is compiled into nvidia-installer to
 * ensure that we always have a ui available.
 *
 * 【文件说明】基于 printf/scanf 的简单流式用户界面实现。
 *
 * 本文件实现了 InstallerUI 调度表（stream_ui_dispatch_table）中定义的
 * 全部接口函数，提供了一个基于标准输入输出的纯文本 UI。
 *
 * 此 UI 直接编译进 nvidia-installer 主程序（而非作为动态库），
 * 作为 ncurses UI 不可用时的回退方案，确保安装器始终有一个可用的 UI。
 *
 * 与 ncurses UI 的主要区别：
 *   - 不使用终端控制序列，仅通过 printf/fprintf 输出文本
 *   - 不提供图形化的窗口、按钮等元素
 *   - 进度条使用简单的 '#' 字符文本方式显示
 *   - 用户输入通过 stdin 读取一行文本
 *   - 不支持窗口大小变化的动态适应（但注册了 SIGWINCH 处理器
 *     来更新终端宽度信息供文本格式化使用）
 *
 * 典型使用场景：
 *   - 非交互式终端（如 SSH 管道、脚本调用）
 *   - 终端窗口太小无法使用 ncurses
 *   - 系统上没有安装 ncurses 库
 *   - 用户通过 --silent 选项要求静默安装
 */

/* ===== 标准库头文件 ===== */
#include <stdio.h>      /* printf, fprintf, fflush, stdout, stderr, stdin */
#include <stdlib.h>     /* atoi, malloc, free */
#include <string.h>     /* strlen, strcasecmp */
#include <ctype.h>      /* tolower, isdigit */
#include <signal.h>     /* signal, SIGWINCH */
#include <unistd.h>     /* usleep（不确定进度动画延迟） */

/* ===== 项目内部头文件 ===== */
#include "nvidia-installer.h"       /* 核心数据结构：Options, Package 等 */
#include "nvidia-installer-ui.h"    /* UI 调度表接口：InstallerUI 结构定义 */
#include "misc.h"                   /* 杂项工具函数，含 reset_current_terminal_width() */
#include "files.h"                  /* 文件操作，含 fget_next_line()（从流中读取一行） */
#include "common-utils.h"           /* 通用工具：nvalloc, nvfree, nvstrdup, nv_info_msg 等 */
#include "msg.h"                    /* 消息输出函数：nv_info_msg, nv_error_msg, nv_info_msg_to_file 等 */

/*
 * ===== 流式 UI 函数原型声明 =====
 *
 * 以下函数对应 InstallerUI 调度表中的各个函数指针，
 * 是流式 UI 对外暴露的全部接口。
 *
 * 注意：与 ncurses-ui.c 不同，这些函数未声明为 static，
 * 因为流式 UI 直接编译进安装器主程序（而非动态库），
 * 通过 stream_ui_dispatch_table 静态链接引用。
 */

int   stream_detect              (Options*);
int   stream_init                (Options*, FormatTextRows format_text_rows);
void  stream_set_title           (Options*, const char*);
char *stream_get_input           (Options*, const char*, const char*);
void  stream_message             (Options*, int level, const char*);
void  stream_command_output      (Options*, const char*);
int   stream_approve_command_list(Options*, CommandList*, const char*);
int   stream_yes_no              (Options*, const int, const char*);
int   stream_multiple_choice     (Options *op, const char *, const char * const *,
                                  int, int);
int   stream_paged_prompt        (Options *op, const char *, const char *,
                                  const char *, const char * const *, int, int);
void  stream_status_begin        (Options*, const char*, const char*);
void  stream_status_update       (Options*, const float, const char*);
void  stream_status_end          (Options*, const char*);
void  stream_update_indeterminate(Options*, const char*);
void  stream_close               (Options*);

/*
 * 流式 UI 调度表。
 *
 * 当 ncurses UI 不可用时（检测失败或用户选择），安装器使用此调度表。
 * 与 ncurses 版本不同，此调度表是直接编译进主程序的，
 * 而非通过 dlsym() 动态加载。
 */

InstallerUI stream_ui_dispatch_table = {
    stream_detect,              /* detect：始终返回 TRUE（流式 UI 总是可用） */
    stream_init,                /* init：初始化并打印欢迎消息 */
    stream_set_title,           /* set_title：空操作（流式 UI 无标题栏） */
    stream_get_input,           /* get_input：从 stdin 读取用户输入 */
    stream_message,             /* message：通过 printf 显示消息 */
    stream_command_output,      /* command_output：在专家模式下显示命令输出 */
    stream_approve_command_list,/* approve_command_list：列出命令并询问确认 */
    stream_yes_no,              /* yes_no：从 stdin 读取 y/n */
    stream_multiple_choice,     /* multiple_choice：显示编号列表供选择 */
    stream_paged_prompt,        /* paged_prompt：显示文本后提供选择 */
    stream_status_begin,        /* status_begin：打印标题并开始进度条 */
    stream_status_update,       /* status_update：更新进度条 */
    stream_status_end,          /* status_end：完成进度条（显示100%） */
    stream_update_indeterminate,/* update_indeterminate：滚动不确定进度指示 */
    stream_close                /* close：释放私有数据 */
};


/*
 * Data - 流式 UI 的私有数据结构。
 *
 * 存储到 Options->ui.priv 中，用于在 status_update 调用之间
 * 跟踪上一次的进度百分比，避免重复绘制相同百分比的进度条。
 */

typedef struct {
    float percent;  /* 上一次显示的进度百分比值 */
} Data;



/* 进度条状态常量，用于 print_status_bar() 的 status 参数 */
#define STATUS_BEGIN 0          /* 进度条开始：重置状态 */
#define STATUS_UPDATE 1         /* 进度条更新：回到行首重绘 */
#define STATUS_END 2            /* 进度条结束：显示100%并换行 */
#define STATUS_INDETERMINATE 3  /* 不确定进度：显示滚动动画 */

#define STATUS_BAR_WIDTH 30     /* 进度条的字符宽度 */

/*
 * print_status_bar() - 在终端上绘制文本进度条。
 *
 * 参数：
 *   d       - 流式 UI 私有数据（此处未直接使用 d 的字段）
 *   status  - 进度条状态（STATUS_BEGIN/UPDATE/END/INDETERMINATE）
 *   percent - 当前完成百分比（0.0 到 1.0）
 *
 * 进度条格式示例：
 *   确定进度：  [###############               ]  50%
 *   不确定进度：[        #                      ]
 *   完成状态：  [##############################] 100%\n
 *
 * 处理流程：
 *   1. BEGIN 状态：重置不确定进度指示器位置
 *      其他状态：输出 '\r' 回到行首（在同一行重绘）
 *   2. 打印进度条 "[###...   ]" 格式
 *   3. 打印百分比数字或清空（不确定进度时）
 *   4. END 状态：输出换行符
 *   5. 刷新 stdout 确保立即显示
 */

static void print_status_bar(Data *d, int status, float percent)
{
    int i;
    float val;

    static int indeterminate_position;  /* 不确定进度中 '#' 的当前位置（持久变量） */

    switch (status) {
    case STATUS_BEGIN:
        /* 重置不确定进度指示器的位置 */
        indeterminate_position = 0;
        break;
    case STATUS_UPDATE:
    case STATUS_END:
    case STATUS_INDETERMINATE:
    default:
        /* 回到行首，在同一行重绘进度条 */
        printf("\r");
        break;
    }

    printf("  [");

    /* 计算进度条中应填充的字符数 */
    val = ((float) STATUS_BAR_WIDTH * percent);

    /* 逐字符绘制进度条 */
    for (i = 0; i < STATUS_BAR_WIDTH; i++) {
        char c;

        if (status == STATUS_INDETERMINATE) {
            /* 不确定进度：一个 '#' 在进度条中来回滚动 */
            c = (i == indeterminate_position % STATUS_BAR_WIDTH) ? '#' : ' ';
        } else {
            /* 确定进度：已完成部分用 '#'，未完成部分用空格 */
            c = (float) i < val ? '#' : ' ';
        }

        printf("%c", c);
    }

    /* 不确定进度指示器位置递增（每次调用向右移动一格） */
    indeterminate_position++;

    printf("] ");
    if (status == STATUS_INDETERMINATE) {
        /* 清除可能遗留的百分比数字显示，然后退格回到 ']' 后方 */
        printf("    \b\b\b\b\b");
    } else {
        /* 显示百分比数字 */
        printf("%3d%%", (int) (percent * 100.0));
    }

    /* 进度完成时换行 */
    if (status == STATUS_END) printf("\n");

    fflush(stdout);  /* 强制刷新输出缓冲区，确保进度条立即显示 */

} /* print_status_bar() */



/*
 * sigwinch_handler() - SIGWINCH 信号处理器。
 *
 * 参数：
 *   n - 信号编号（此处为 SIGWINCH）
 *
 * 当终端窗口大小变化时，操作系统会发送 SIGWINCH 信号。
 * 此处理器调用 reset_current_terminal_width(0) 重新查询终端宽度，
 * 以便后续的文本格式化能适应新的窗口宽度。
 */

static void sigwinch_handler(int n)
{
    reset_current_terminal_width(0);
}


/*
 * stream_detect() - 检测流式 UI 是否可用。
 *
 * 参数：
 *   op - 安装器选项结构（此处未使用）
 *
 * 返回值：
 *   始终返回 TRUE。流式 UI 只依赖标准输入输出，不需要特殊的终端支持，
 *   因此始终可用。这确保了安装器在任何环境下都有一个可用的 UI。
 */

int stream_detect(Options *op)
{
    return TRUE;

} /* stream_detect() */



/*
 * stream_init() - 初始化流式 UI 并打印欢迎消息。
 *
 * 参数：
 *   op              - 安装器选项结构
 *   format_text_rows - 文本格式化函数指针（流式 UI 未使用此参数，
 *                      但接口定义要求传入以保持与 ncurses UI 的一致性）
 *
 * 返回值：
 *   始终返回 TRUE
 *
 * 处理流程：
 *   1. 分配并初始化私有数据结构 Data
 *   2. 如果不是静默模式，打印欢迎消息
 *   3. 注册 SIGWINCH 信号处理器以响应终端窗口大小变化
 */

int stream_init(Options *op, FormatTextRows format_text_rows)
{
    Data *d = nvalloc(sizeof(Data));  /* 分配并零初始化私有数据 */

    op->ui.priv = d;  /* 存入 Options 的 UI 私有数据指针 */

    if (!op->silent) {

        /* 打印欢迎消息 */
        nv_info_msg(NULL, "");
        nv_info_msg(NULL, "Welcome to the NVIDIA Software Installer for "
                          "Unix/Linux");
        nv_info_msg(NULL, "");

        /* 注册 SIGWINCH 信号处理器，以便在终端窗口大小变化时更新宽度 */

        signal(SIGWINCH, sigwinch_handler);
    }

    return TRUE;

} /* stream_init() */



/*
 * stream_set_title() - 设置 UI 标题。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   title - 标题字符串
 *
 * 在流式 UI 中此函数是空操作——纯文本输出没有标题栏或窗口标题
 * 的概念。其他 UI 后端（如 ncurses）会用它来更新窗口标题。
 */

void stream_set_title(Options *op, const char *title)
{
    return;

} /* stream_set_title() */



/*
 * stream_get_input() - 显示提示消息并获取用户文本输入。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   def - 默认值（用户直接按回车时返回此值）
 *   msg - 提示消息文本
 *
 * 返回值：
 *   用户输入的字符串。如果用户输入为空，返回默认值的副本。
 *   调用方负责释放返回的字符串。
 *
 * 处理流程：
 *   1. 打印提示消息和默认值
 *   2. 从 stdin 读取一行用户输入
 *   3. 如果输入为空或只有换行符，使用默认值
 */

char *stream_get_input(Options *op, const char *def, const char *msg)
{
    char *buf;

    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "%s", msg);
    fprintf(stdout, "  [default: '%s']: ", def);
    fflush(stdout);

    buf = fget_next_line(stdin, NULL);  /* 从 stdin 读取一行 */

    if (!buf || !buf[0]) {
        /* 用户未输入任何内容，使用默认值 */
        if (buf) free(buf);
        buf = nvstrdup(def);
    }

    return buf;

} /* stream_get_input() */




/*
 * stream_message() - 按指定级别打印消息。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   level - 消息级别（NV_MSG_LEVEL_LOG/MESSAGE/WARNING/ERROR）
 *   msg   - 消息文本
 *
 * 各级别的处理方式：
 *   LOG     - 输出到 stdout，无前缀，无额外换行（进度条显示时抑制）
 *   MESSAGE - 输出到 stdout，无前缀，前后各加一空行
 *   WARNING - 输出到 stderr，"WARNING: " 前缀，前后各加一空行
 *   ERROR   - 输出到 stderr，"ERROR: " 前缀，前后各加一空行
 *
 * 当进度条正在显示时（status_active=TRUE），LOG 级别消息会被抑制，
 * 避免干扰进度条的显示（进度条使用 '\r' 回到行首重绘）。
 */

void stream_message(Options *op, const int level, const char *msg)
{
    /* 消息级别属性表：定义每个级别的前缀、输出流和是否加空行 */
    typedef struct {
        char *prefix;       /* 消息前缀（如 "WARNING: "） */
        FILE *stream;       /* 输出目标流（stdout 或 stderr） */
        int newline;        /* 是否在消息前后各添加一个空行 */
    } MessageLevelAttributes;

    const MessageLevelAttributes msg_attrs[] = {
        { NULL,        stdout, FALSE }, /* NV_MSG_LEVEL_LOG：仅日志，不加空行 */
        { NULL,        stdout, TRUE  }, /* NV_MSG_LEVEL_MESSAGE：普通消息 */
        { "WARNING: ", stderr, TRUE  }, /* NV_MSG_LEVEL_WARNING：警告 */
        { "ERROR: ",   stderr, TRUE  }  /* NV_MSG_LEVEL_ERROR：错误 */
    };

    /* 进度条显示期间，不输出 LOG 级别消息，避免干扰进度条 */

    if ((level == NV_MSG_LEVEL_LOG) && (op->ui.status_active)) return;

    /* 如果需要，在消息前添加空行 */
    if (msg_attrs[level].newline) {
        nv_info_msg_to_file(msg_attrs[level].stream, NULL, "");
    }
    /* 输出消息本身 */
    nv_info_msg_to_file(msg_attrs[level].stream,
                        msg_attrs[level].prefix,
                        "%s", msg);
    /* 如果需要，在消息后添加空行 */
    if (msg_attrs[level].newline) {
        nv_info_msg_to_file(msg_attrs[level].stream, NULL, "");
    }

} /* stream_message() */



/*
 * stream_command_output() - 显示命令执行的输出。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   msg - 命令输出文本
 *
 * 仅在专家模式（--expert）下显示，且不在进度条显示期间输出
 * （避免干扰进度条）。输出带有缩进前缀 "   "。
 *
 * 非专家模式下此函数是空操作，命令输出仍会写入日志文件。
 */

void stream_command_output(Options *op, const char *msg)
{
    /* 非专家模式或进度条显示期间，不输出命令结果 */
    if ((!op->expert) || (op->ui.status_active)) return;

    nv_info_msg("   ", "%s", msg);  /* 带3空格缩进输出 */

} /* stream_command_output() */



/*
 * stream_approve_command_list() - 显示待执行的命令列表并请求用户批准。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   cl    - 命令列表（包含各操作的描述文本）
 *   descr - 安装目标描述
 *
 * 返回值：
 *   TRUE  - 用户批准（回答 yes）
 *   FALSE - 用户拒绝（回答 no 或输入错误），安装将中止
 *
 * 处理流程：
 *   1. 打印说明文本和所有命令描述（每条前加 " --> " 前缀）
 *   2. 调用 stream_yes_no() 询问用户是否接受
 *   3. 如果用户拒绝，输出错误消息并返回 FALSE
 */

int stream_approve_command_list(Options *op, CommandList *cl,
                                const char *descr)
{
    int i;
    const char *prefix = " --> ";  /* 每条命令描述的前缀 */

    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "The following operations will be performed to install the %s:",
                descr);
    nv_info_msg(NULL, "");

    /* 逐条显示命令描述 */
    for (i = 0; i < cl->num; i++) {
        nv_info_msg(prefix, "%s", cl->descriptions[i]);
    }

    fflush(stdout);

    /* 询问用户是否接受 */
    if (!stream_yes_no(op, TRUE, "\nIs this acceptable? (answering 'no' will "
                       "abort installation)")) {
        nv_error_msg("Command list not accepted; exiting installation.");
        return FALSE;
    }

    return TRUE;

} /* stream_approve_command_list() */



/*
 * stream_yes_no() - 显示是/否选择问题。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   def - 默认答案（TRUE=Yes, FALSE=No）
 *   msg - 问题文本
 *
 * 返回值：
 *   TRUE  - 用户选择 Yes
 *   FALSE - 用户选择 No，或输入读取失败
 *
 * 处理流程：
 *   1. 打印问题文本和默认值提示
 *   2. 从 stdin 读取用户输入
 *   3. 根据输入的首字母判断：'y'=Yes, 'n'=No
 *   4. 如果首字母不是 y 或 n，使用默认值
 */

int stream_yes_no(Options *op, const int def, const char *msg)
{
    char *buf;
    int eof, ret = def;  /* 默认返回值 */

    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "%s", msg);
    /* 显示默认选项提示 */
    if (def) fprintf(stdout, "  [default: (Y)es]: ");
    else fprintf(stdout, "  [default: (N)o]: ");
    fflush(stdout);

    buf = fget_next_line(stdin, &eof);

    if (!buf) return FALSE;  /* 读取失败（如 EOF）返回 FALSE */

    /* 根据首字母判断用户选择，不区分大小写 */

    if (tolower(buf[0]) == 'y') ret = TRUE;
    if (tolower(buf[0]) == 'n') ret = FALSE;

    free(buf);

    return ret;

} /* stream_yes_no() */


/*
 * select_option() - 将用户输入的文本匹配到可选项列表。
 *
 * 参数：
 *   options        - 可选项名称数组
 *   num_options    - 可选项数量
 *   default_option - 默认选项索引
 *   selection      - 用户输入的选择文本
 *
 * 返回值：
 *   >= 0  - 匹配到的选项索引
 *   -1    - 用户输入了文本但没有匹配任何选项名称
 *   -2    - 用户输入了数字但超出有效范围
 *
 * 选择方式（按优先级）：
 *   1. 如果输入为空字符串，返回默认选项
 *   2. 如果输入以数字开头，解析为编号（1-based），返回对应索引（0-based）
 *   3. 否则，进行大小写不敏感的名称匹配，返回首个匹配项的索引
 *   4. 以上都不匹配，返回负数表示无效输入
 */

static int select_option(const char * const *options, int num_options,
                         int default_option, const char *selection)
{
    int i;

    /* 空输入使用默认选项 */
    if (strlen(selection) == 0) {
        return default_option;
    }

    /* 数字开头：按编号选择（显示给用户的是1-based编号） */
    if (isdigit(selection[0])) {
        int index = atoi(selection);

        if (index < 1 || index > num_options) {
            return -2;  /* 编号超出范围 */
        }

        return index - 1;  /* 转换为0-based索引 */
    }

    /* 文本输入：按名称匹配（大小写不敏感） */
    for (i = 0; i < num_options; i++) {
        if (strcasecmp(selection, options[i]) == 0) {
            return i;
        }
    }

    return -1;  /* 无匹配 */
}



/*
 * stream_multiple_choice() - 显示带多个选项的选择题。
 *
 * 参数：
 *   op             - 安装器选项结构
 *   question       - 问题文本
 *   answers        - 选项名称数组
 *   num_answers    - 选项数量
 *   default_answer - 默认选项索引
 *
 * 返回值：
 *   用户选择的选项索引（0-based）
 *
 * 处理流程：
 *   1. 打印问题文本和所有可选答案（带编号，标注默认选项）
 *   2. 从 stdin 读取用户输入
 *   3. 通过 select_option() 匹配用户输入到选项
 *   4. 如果匹配失败，输出错误提示并重复询问
 *   5. 匹配成功后返回选项索引
 *
 * 用户可以通过编号（如 "1"）或选项名称（如 "Yes"）进行选择。
 * 直接按回车使用默认选项。
 */

int stream_multiple_choice(Options *op, const char *question,
                           const char * const *answers, int num_answers,
                           int default_answer)
{
    int ret = default_answer;

    do {
        char *str;
        int i;

        /* 如果上一次输入无效，显示错误提示 */
        if (ret < 0) {
            nv_error_msg("Invalid response!");
        }

        /* 打印问题和选项列表 */
        nv_info_msg(NULL, "");
        nv_info_msg(NULL, "%s", question);
        nv_info_msg(NULL, "Valid responses are: ");

        for (i = 0; i < num_answers; i++) {
            /* 格式：(编号) "选项名" [default] */
            nv_info_msg(NULL, " (%d)\t\"%s\"%s", i + 1, answers[i],
                        i == default_answer ? " [ default ]" : "");
        }

        /* 提示用户输入 */
        nv_info_msg(NULL, "Please select your response by number or name:");
        str = fget_next_line(stdin, NULL);

        /* 解析用户输入 */
        ret = select_option(answers, num_answers, default_answer, str);
        free(str);
    } while (ret < 0);  /* 输入无效时循环重试 */

    return ret;
}



/*
 * stream_paged_prompt() - 显示带文本内容的多选提示。
 *
 * 参数：
 *   op             - 安装器选项结构
 *   question       - 问题文本
 *   pager_title    - 文本区域标题
 *   pager_text     - 文本内容（不可滚动，一次性全部输出）
 *   answers        - 选项名称数组
 *   num_answers    - 选项数量
 *   default_answer - 默认选项索引
 *
 * 返回值：
 *   用户选择的选项索引
 *
 * 处理流程：
 *   1. 打印分隔线和文本标题
 *   2. 打印完整的文本内容
 *   3. 打印结束分隔线
 *   4. 委托给 stream_multiple_choice() 进行选项选择
 *
 * 与 ncurses 版本的区别：
 *   流式 UI 不支持文本滚动，而是一次性将全部文本输出到终端。
 *   对于较长的文本，用户需要依赖终端自身的滚动功能来查看。
 */

int stream_paged_prompt(Options *op, const char *question,
                        const char *pager_title, const char *pager_text,
                        const char * const *answers, int num_answers,
                        int default_answer)
{
    /* 打印文本内容，用下划线分隔线包围 */
    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "________");       /* 上分隔线 */
    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "%s", pager_title); /* 标题 */
    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "%s", pager_text);  /* 文本内容 */
    nv_info_msg(NULL, "");
    nv_info_msg(NULL, "________");       /* 下分隔线 */

    /* 委托给多选处理函数 */
    return stream_multiple_choice(op, question, answers, num_answers,
                                  default_answer);
}



/*
 * stream_status_begin() - 开始进度显示。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   title - 进度条标题（始终有值，如 "Installing"）
 *   msg   - 可选的描述消息（可以为 NULL）
 *
 * 处理流程：
 *   1. 重置进度百分比为 0
 *   2. 打印标题和描述消息
 *   3. 绘制初始的空进度条（0%）
 */

void stream_status_begin(Options *op, const char *title, const char *msg)
{
    Data *d = op->ui.priv;

    d->percent = 0;  /* 重置进度 */

    /* 打印标题行 */
    nv_info_msg(NULL, "%s: %s\n", title, msg ? msg : "");

    /* 绘制初始进度条 */
    print_status_bar(d, STATUS_BEGIN, 0.0);

} /* stream_status_begin() */



/*
 * stream_status_update() - 更新进度百分比。
 *
 * 参数：
 *   op      - 安装器选项结构
 *   percent - 当前完成百分比（0.0 到 1.0）
 *   msg     - 状态消息（流式 UI 未使用此参数，因为文本进度条
 *             没有额外的消息显示区域）
 *
 * 仅当百分比实际变化时才重绘进度条，避免不必要的终端输出。
 */

void stream_status_update(Options *op, const float percent, const char *msg)
{
    Data *d = op->ui.priv;

    /* 仅在百分比变化时更新显示 */
    if (d->percent != percent) {
        d->percent = percent;
        print_status_bar(op->ui.priv, STATUS_UPDATE, percent);
    }

} /* stream_status_update() */


/*
 * stream_update_indeterminate() - 更新不确定进度动画。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   msg - 状态消息（流式 UI 未使用）
 *
 * 每次调用绘制一帧不确定进度动画（'#' 字符在进度条中向右移动一格），
 * 然后等待 250ms。此函数由后台线程循环调用。
 */

void stream_update_indeterminate(Options *op, const char *msg)
{
    Data *d = op->ui.priv;

    print_status_bar(d, STATUS_INDETERMINATE, 0.0);
    usleep(250000);  /* 等待 250ms 控制动画速度 */
}


/*
 * stream_status_end() - 结束进度显示。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   msg - 最终状态消息（流式 UI 未使用）
 *
 * 将进度条绘制到 100% 完成状态并输出换行符。
 */

void stream_status_end(Options *op, const char *msg)
{
    print_status_bar(op->ui.priv, STATUS_END, 1.0);
} /* stream_status_end() */



/*
 * stream_close() - 关闭流式 UI 并释放资源。
 *
 * 参数：
 *   op - 安装器选项结构（可能为 NULL，如从信号处理器调用时）
 *
 * 处理流程：
 *   释放 Data 私有数据结构。流式 UI 不需要其他清理操作
 *   （不像 ncurses 需要 endwin() 恢复终端状态）。
 */

void stream_close(Options *op)
{
    if (op) {
        nvfree(op->ui.priv);  /* 释放私有数据 */
    }
}
