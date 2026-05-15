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
 * ncurses_ui.c - implementation of the nvidia-installer ui using ncurses.
 *
 * 【文件说明】基于 ncurses 库的图形化终端用户界面实现。
 *
 * 本文件实现了 InstallerUI 调度表（ui_dispatch_table）中定义的全部接口函数，
 * 提供了一个功能丰富的终端 UI，包括：
 *   - 页眉/页脚区域显示（标题栏和状态栏）
 *   - 消息对话框（带 OK 按钮确认）
 *   - 用户文本输入框（支持光标移动、删除、滚动）
 *   - 是/否选择对话框
 *   - 多选按钮对话框
 *   - 带可滚动文本区域的分页提示（类似 less 的翻页浏览）
 *   - 进度条显示（确定进度和不确定进度两种模式）
 *   - 终端窗口大小变化的自动处理
 *
 * 本文件不使用 ncurses 的 window 子系统（因为发现了足够多的 ncurses window
 * 相关 bug），而是自行实现了 RegionStruct 来管理屏幕区域。所有绘制操作
 * 都直接在 stdscr（通过 nv_stdscr 变量）上进行。
 *
 * 当 ncurses 不可用（如非交互式终端、窗口太小）时，安装器会回退到
 * stream-ui.c 中实现的基于 printf/scanf 的简单流式 UI。
 */

/* ===== 标准库和第三方库头文件 ===== */
#include <ncurses.h>    /* ncurses 终端控制库：窗口管理、颜色、键盘输入等 */
#include <string.h>     /* 字符串操作：strlen, strcpy, memset 等 */
#include <stdlib.h>     /* 通用工具：malloc, free, strdup 等 */
#include <unistd.h>     /* POSIX 接口：usleep（按钮动画延迟）等 */
#include <ctype.h>      /* 字符分类：isprint（判断是否为可打印字符）等 */

/* ===== 项目内部头文件 ===== */
#include "nvidia-installer.h"       /* 核心数据结构：Options, Package 等 */
#include "nvidia-installer-ui.h"    /* UI 调度表接口：InstallerUI 结构定义 */




/* ===== 数据结构定义 ===== */

/*
 * RegionStruct - 屏幕区域结构体。
 *
 * 类似于 ncurses 的 window 概念，但完全在本文件中自行实现。
 * 作者在 ncurses window 中发现了足够多的 bug，因此决定避免使用它们，
 * 改为直接在 stdscr 上按坐标操作。
 *
 * 每个 Region 描述了屏幕上的一个矩形区域，用于绘制页眉、页脚、
 * 消息框、进度条等 UI 元素。
 */

typedef struct {
    int x, y, w, h; /* 区域在 stdscr 上的位置 (x,y) 和尺寸 (w=宽, h=高) */
    int attr;       /* 显示属性（颜色对或反色等，传递给 wattrset()） */
    char *line;     /* 一个由空格填充的字符串，长度等于区域宽度 w。
                       用于快速清空区域——将整行空格输出即可覆盖原内容 */
} RegionStruct;




/*
 * PagerStruct - 文本分页浏览器结构体。
 *
 * 实现了类似 `less` 命令的文本分页浏览功能。
 * 用于显示较长的文本内容（如命令列表、许可证文本），
 * 支持上/下箭头逐行滚动和 PageUp/PageDown 翻页。
 */

typedef struct {
    TextRows *t;          /* 要显示的文本行数据（由 format_text_rows 生成） */
    RegionStruct *region; /* 文本显示所在的屏幕区域 */
    const char *label;    /* 页脚左侧的标签文本（如 "Proposed CommandList"） */
    int cur;              /* 当前滚动位置（第一行在 t->t[] 中的索引） */
    int page;             /* 每页的行数（用于 PageUp/PageDown 翻页计算，等于区域高度减 2） */
} PagerStruct;




/*
 * DataStruct - ncurses UI 的私有数据结构。
 *
 * 在 nv_ncurses_init() 中分配，并存储到 Options->ui.priv 指针中。
 * 所有 ncurses UI 函数通过 op->ui.priv 获取此结构来访问 UI 状态。
 */

typedef struct {

    RegionStruct *header;  /* 页眉区域：显示安装器标题 */
    RegionStruct *footer;  /* 页脚区域：显示左侧标签和右侧状态（如滚动百分比） */
    RegionStruct *message; /* 消息区域：显示对话框、进度条等动态内容 */

    FormatTextRows format_text_rows; /* 文本格式化函数指针，由安装器主程序传入。
                                        用于将长文本按指定宽度折行为 TextRows 结构 */

    bool use_color;        /* 是否启用颜色显示（受终端能力和 --no-ncurses-color 选项影响） */

    int width;             /* 终端窗口宽度的缓存值（列数） */
    int height;            /* 终端窗口高度的缓存值（行数） */

    char *title;           /* 页眉标题文本的缓存（如 "NVIDIA Software Installer for Unix/Linux"） */
    char *footer_left;     /* 页脚左侧文本的缓存 */
    char *footer_right;    /* 页脚右侧文本的缓存（如 "www.nvidia.com" 或翻页百分比） */

    char *progress_title;  /* 进度条标题文本的缓存。
                              在 status_begin 中设置，在 status_end 中释放 */

} DataStruct;




/* ===== 常量定义 ===== */

/* 页眉和页脚的默认显示文本 */

#define NV_NCURSES_DEFAULT_TITLE "NVIDIA Software Installer for Unix/Linux"
#define NV_NCURSES_DEFAULT_FOOTER_LEFT NV_NCURSES_DEFAULT_TITLE
#define NV_NCURSES_DEFAULT_FOOTER_RIGHT "www.nvidia.com"

/*
 * ncurses 颜色对索引。
 * ncurses 使用 init_pair(index, fg, bg) 定义颜色对，
 * 然后通过 COLOR_PAIR(index) 获取对应的属性值。
 */

#define NV_NCURSES_HEADER_COLOR_IDX      1  /* 页眉颜色对索引 */
#define NV_NCURSES_MESSAGE_COLOR_IDX     2  /* 消息区域颜色对索引 */
#define NV_NCURSES_BUTTON_COLOR_IDX      3  /* 按钮颜色对索引 */
#define NV_NCURSES_INPUT_COLOR_IDX       4  /* 输入框/进度条颜色对索引 */

/* 有颜色支持时使用的属性宏 */
#define NV_NCURSES_HEADER_COLOR (COLOR_PAIR(NV_NCURSES_HEADER_COLOR_IDX))   /* 页眉：黑字绿底 */
#define NV_NCURSES_FOOTER_COLOR A_REVERSE                                    /* 页脚：反色显示 */
#define NV_NCURSES_MESSAGE_COLOR (COLOR_PAIR(NV_NCURSES_MESSAGE_COLOR_IDX)) /* 消息：白字蓝底 */
#define NV_NCURSES_BUTTON_COLOR (COLOR_PAIR(NV_NCURSES_BUTTON_COLOR_IDX))   /* 按钮：白字红底 */
#define NV_NCURSES_INPUT_COLOR (COLOR_PAIR(NV_NCURSES_INPUT_COLOR_IDX))     /* 输入/进度条：绿字黑底 */

/* 无颜色支持时使用的回退属性 */
#define NV_NCURSES_HEADER_NO_COLOR A_REVERSE   /* 页眉：反色 */
#define NV_NCURSES_FOOTER_NO_COLOR A_REVERSE   /* 页脚：反色 */
#define NV_NCURSES_MESSAGE_NO_COLOR A_NORMAL   /* 消息：正常显示 */

/* 按钮按下动画的持续时间（微秒），125000us = 125ms */

#define NV_NCURSES_BUTTON_PRESS_TIME 125000

/* 键盘按键的 ASCII 值定义 */
#define NV_NCURSES_TAB 9           /* Tab 键 */
#define NV_NCURSES_ENTER 10        /* Enter/回车键 */
#define NV_NCURSES_BACKSPACE 8     /* Backspace 键 */

/* 将字符转换为 Ctrl+字符 的控制码（通过与 0x1f 进行位与运算） */
#define NV_NCURSES_CTRL(x) ((x) & 0x1f)


/*
 * 终端窗口最小尺寸要求。
 * 如果终端窗口小于此尺寸，ncurses UI 将拒绝初始化，
 * 安装器会回退到流式 UI（stream-ui.c）。
 * 这些值是比较保守的估计——确保有足够空间显示基本的对话框。
 */

#define NV_NCURSES_MIN_WIDTH 40    /* 最小宽度（列数） */
#define NV_NCURSES_MIN_HEIGHT 10   /* 最小高度（行数） */

/* 水平分隔线字符，用于进度条区域的标题与进度条之间的分隔 */
#define NV_NCURSES_HLINE    '_'



/*
 * ===== UI 入口函数原型声明 =====
 *
 * 以下函数对应 InstallerUI 调度表中的各个函数指针，
 * 是 ncurses UI 对外暴露的全部接口。
 * 均声明为 static，通过调度表 ui_dispatch_table 间接调用。
 */

static int   nv_ncurses_detect              (Options*);
static int   nv_ncurses_init                (Options*,
                                             FormatTextRows format_text_rows);
static void  nv_ncurses_set_title           (Options*, const char*);
static char *nv_ncurses_get_input           (Options*, const char*,
                                             const char*);
static void  nv_ncurses_message             (Options*, const int level,
                                             const char*);
static void  nv_ncurses_command_output      (Options*, const char*);
static int   nv_ncurses_approve_command_list(Options*, CommandList*,
                                             const char*);
static int   nv_ncurses_yes_no              (Options*, const int, const char*);
static int   nv_ncurses_multiple_choice     (Options *, const char*,
                                             const char * const *, int, int);
static void  nv_ncurses_status_begin        (Options*, const char*,
                                             const char*);
static void  nv_ncurses_status_update       (Options*, const float,
                                             const char*);

static void  nv_ncurses_update_indeterminate(Options*, const char*);
static void  nv_ncurses_status_end          (Options*, const char*);
static void  nv_ncurses_close               (Options*);



/* ===== 页眉/页脚操作辅助函数原型 ===== */

static void nv_ncurses_set_header(DataStruct *, const char *);
static void nv_ncurses_set_footer(DataStruct *, const char *, const char *);


/* ===== RegionStruct 操作辅助函数原型 ===== */

static RegionStruct *nv_ncurses_create_region(DataStruct *, int, int,
                                              int, int, int, int);
static void nv_ncurses_clear_region(RegionStruct *);
static void nv_ncurses_destroy_region(RegionStruct *);


/* ===== 按钮绘制辅助函数原型 ===== */

static void nv_ncurses_draw_button(DataStruct *, RegionStruct *, int, int,
                                   int, int, const char *, bool, bool);
static void nv_ncurses_erase_button(RegionStruct *, int, int, int, int);
static void draw_buttons(DataStruct *d, const char * const *buttons,
                         int num_buttons, int button, int button_w,
                         const int *buttons_x, int button_y);


/* ===== 区域绘制辅助函数原型 ===== */

static void nv_ncurses_do_message_region(DataStruct *, const char *,
                                         const char *, int, int);
static void nv_ncurses_do_progress_bar_region(DataStruct *);
static void nv_ncurses_do_progress_bar_message(DataStruct *, const char *,
                                               int, int);

/* ===== 分页浏览器函数原型 ===== */

static PagerStruct *nv_ncurses_create_pager(DataStruct *, int, int, int, int,
                                            TextRows *, const char *, int);
static void nv_ncurses_pager_update(DataStruct *, PagerStruct *);
static void nv_ncurses_pager_handle_events(DataStruct *, PagerStruct *, int);
static void nv_ncurses_destroy_pager(PagerStruct *);
static int nv_ncurses_paged_prompt(Options *, const char*, const char*,
                                   const char*, const char * const *, int, int);


/* ===== 进度条辅助函数原型 ===== */

static int choose_char(int i, int p[4], char v[4], char def);
static void init_percentage_string(char v[4], int n);
static void init_position(int p[4], int w);


/* ===== 其他辅助函数原型 ===== */

static char *nv_ncurses_create_command_list_text(DataStruct *, CommandList *);

static void nv_ncurses_free_text_rows(TextRows *);

static int nv_ncurses_format_print(DataStruct *, RegionStruct *,
                                   int, int, int, int, TextRows *t);

static int nv_ncurses_check_resize(DataStruct *, bool);







/*
 * UI 调度表（dispatch table）。
 *
 * 此全局变量会被 user-interface.c 中的 ui_init() 函数通过 dlsym() 动态查找。
 * 它将 InstallerUI 结构中的每个函数指针映射到本文件中对应的 ncurses 实现函数。
 * 安装器通过此调度表间接调用 UI 操作，从而实现 UI 后端的可替换。
 */

InstallerUI ui_dispatch_table = {
    nv_ncurses_detect,              /* detect：检测 ncurses UI 是否可用 */
    nv_ncurses_init,                /* init：初始化 ncurses 环境 */
    nv_ncurses_set_title,           /* set_title：更新页眉标题 */
    nv_ncurses_get_input,           /* get_input：获取用户文本输入 */
    nv_ncurses_message,             /* message：显示消息对话框 */
    nv_ncurses_command_output,      /* command_output：显示命令输出（当前未实现） */
    nv_ncurses_approve_command_list,/* approve_command_list：命令列表审批 */
    nv_ncurses_yes_no,              /* yes_no：是/否确认对话框 */
    nv_ncurses_multiple_choice,     /* multiple_choice：多选对话框 */
    nv_ncurses_paged_prompt,        /* paged_prompt：带分页文本的多选对话框 */
    nv_ncurses_status_begin,        /* status_begin：开始进度显示 */
    nv_ncurses_status_update,       /* status_update：更新进度百分比 */
    nv_ncurses_status_end,          /* status_end：结束进度显示 */
    nv_ncurses_update_indeterminate,/* update_indeterminate：更新不确定进度动画 */
    nv_ncurses_close                /* close：关闭并清理 ncurses 环境 */
};




/* ===== 内部变量 ===== */

/*
 * 保存 initscr() 的返回值，而不是直接使用全局变量 stdscr。
 * 这样做是为了避免某些 libncurses.so 版本不导出 stdscr 符号的问题，
 * 此问题在 SUSE 的 bug 报告 http://bugzilla.suse.com/show_bug.cgi?id=1132282
 * 中有记录。
 */
static WINDOW *nv_stdscr;




/*
 * nv_ncurses_detect() - 检测 ncurses UI 是否可用。
 *
 * 参数：
 *   op - 安装器选项结构（此处未直接使用，但接口要求传入）
 *
 * 返回值：
 *   TRUE  - ncurses 初始化成功且终端窗口足够大
 *   FALSE - 初始化失败或窗口太小
 *
 * 处理流程：
 *   1. 调用 initscr() 初始化 ncurses，保存返回的窗口指针
 *   2. 查询当前终端窗口尺寸
 *   3. 如果窗口宽度 < 40 或高度 < 10，结束 ncurses 模式并返回 FALSE
 *   4. 否则返回 TRUE，表示 ncurses UI 可以使用
 *
 * 注意：initscr() 成功后 ncurses 已处于活跃状态，若后续决定不使用
 *       ncurses UI，需要调用 endwin() 恢复终端状态。
 */

static int nv_ncurses_detect(Options *op)
{
    int x, y;

    if (!(nv_stdscr = initscr())) return FALSE;

    /*
     * 查询当前终端窗口尺寸，如果太小则不使用 ncurses UI
     */

    getmaxyx(nv_stdscr, y, x);

    if ((x < NV_NCURSES_MIN_WIDTH) || (y < NV_NCURSES_MIN_HEIGHT)) {
        endwin();
        return FALSE;
    }

    return TRUE;

} /* nv_ncurses_detect() */




/*
 * nv_ncurses_init() - 初始化 ncurses 用户界面。
 *
 * 参数：
 *   op              - 安装器选项结构（包含是否禁用颜色等配置）
 *   format_text_rows - 文本格式化函数指针，由安装器主程序提供，
 *                      用于将文本按指定宽度折行为 TextRows 结构
 *
 * 返回值：
 *   TRUE - 始终返回 TRUE（此函数在 detect 成功后调用，不会失败）
 *
 * 处理流程：
 *   1. 分配并零初始化 DataStruct 私有数据
 *   2. 初始化颜色支持（检查终端能力 + 用户选项）
 *   3. 配置 ncurses 终端模式（关闭回显、关闭行缓冲、隐藏光标等）
 *   4. 创建页眉和页脚区域
 *   5. 将 DataStruct 存入 op->ui.priv 供后续使用
 *   6. 设置默认的页眉标题和页脚文本
 *   7. 刷新屏幕
 */

static int nv_ncurses_init(Options *op, FormatTextRows format_text_rows)
{
    DataStruct *d;

    /* 分配并零初始化 DataStruct */
    d = (DataStruct *) malloc(sizeof(DataStruct));
    memset(d, 0, sizeof(DataStruct));

    /* 保存文本格式化函数指针 */
    d->format_text_rows = format_text_rows;

    /* 初始化颜色支持 */

    d->use_color = !op->no_ncurses_color;  /* 用户是否通过 --no-ncurses-color 禁用了颜色 */

    if (d->use_color) {
        if (!has_colors()) {               /* 检查终端是否支持颜色 */
            d->use_color = FALSE;
        }
    }

    if (d->use_color) {
        if (start_color() == ERR) {        /* 启动颜色子系统 */
            d->use_color = FALSE;
        } else {
            /* 定义颜色对：                           前景色        背景色 */
            init_pair(NV_NCURSES_HEADER_COLOR_IDX,  COLOR_BLACK,  COLOR_GREEN);  /* 页眉：黑字绿底 */
            init_pair(NV_NCURSES_MESSAGE_COLOR_IDX, COLOR_WHITE,  COLOR_BLUE);   /* 消息：白字蓝底 */
            init_pair(NV_NCURSES_BUTTON_COLOR_IDX,  COLOR_WHITE,  COLOR_RED);    /* 按钮：白字红底 */
            init_pair(NV_NCURSES_INPUT_COLOR_IDX,   COLOR_GREEN,  COLOR_BLACK);  /* 输入框：绿字黑底 */
        }
    }

    /* 配置 ncurses 终端行为 */
    wclear(nv_stdscr);       /* 清空屏幕 */
    noecho();                /* 禁止输入字符自动回显到屏幕 */
    cbreak();                /* 禁用行缓冲，使字符输入后立即可读（但保留 Ctrl+C 等信号） */
    curs_set(0);             /* 隐藏终端光标 */
    keypad(nv_stdscr, TRUE); /* 启用功能键、方向键等特殊键的识别 */

    getmaxyx(nv_stdscr, d->height, d->width); /* 获取并缓存当前终端尺寸 */

    /* 创建页眉区域：位于屏幕顶部第一行，留出左右各 1 列边距 */

    d->header = nv_ncurses_create_region(d, 1, 0, d->width - 2, 1,
                                         NV_NCURSES_HEADER_COLOR,
                                         NV_NCURSES_HEADER_NO_COLOR);

    /* 创建页脚区域：位于屏幕倒数第二行，留出左右各 1 列边距 */

    d->footer = nv_ncurses_create_region(d, 1, d->height - 2, d->width - 2, 1,
                                         NV_NCURSES_FOOTER_COLOR,
                                         NV_NCURSES_FOOTER_NO_COLOR);

    /* 将 DataStruct 存储到 Options 中的 UI 私有数据指针 */

    op->ui.priv = (void *) d;

    /* 设置页眉和页脚的初始文本 */

    nv_ncurses_set_header(d, NV_NCURSES_DEFAULT_TITLE);

    nv_ncurses_set_footer(d, NV_NCURSES_DEFAULT_FOOTER_LEFT,
                          NV_NCURSES_DEFAULT_FOOTER_RIGHT);

    wrefresh(nv_stdscr);  /* 将缓冲区的内容刷新到终端屏幕 */

    return TRUE;

} /* nv_ncurses_init() */




/*
 * nv_ncurses_set_title() - 更新页眉区域的标题文本。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   title - 要显示的标题字符串
 *
 * 处理流程：
 *   调用 nv_ncurses_set_header() 更新页眉内容，然后刷新屏幕。
 */

static void nv_ncurses_set_title(Options *op, const char *title)
{
    DataStruct *d = (DataStruct *) op->ui.priv;

    nv_ncurses_set_header(d, title);

    wrefresh(nv_stdscr);

} /* nv_ncurses_set_title() */




/*
 * nv_ncurses_get_input() - 显示提示信息并获取用户文本输入。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   def - 默认输入值（用户直接按回车时返回此值）
 *   msg - 提示信息文本
 *
 * 返回值：
 *   用户输入的字符串（调用方负责释放），若 msg 为 NULL 则返回 NULL
 *
 * 处理流程：
 *   1. 在消息提示后追加 ": " 后缀
 *   2. 将默认值复制到编辑缓冲区
 *   3. 计算输入框和消息区域的布局（尽量将提示和输入框放在一行）
 *   4. 创建消息区域并显示提示文本
 *   5. 进入输入循环：处理字符输入、光标移动、删除、退格等操作
 *   6. 当用户按下 Enter 时退出循环，返回输入的字符串
 *
 * 支持的编辑操作：
 *   - 左/右箭头：移动光标
 *   - Backspace：删除光标前字符
 *   - Delete：删除光标处字符
 *   - Ctrl+L：强制重绘屏幕
 *   - 当输入文本超出显示宽度时，会自动滚动并显示 < > 箭头提示
 */

#define MIN_INPUT_LEN 32     /* 输入框的最小显示长度（字符数） */
#define MAX_BUF_LEN 1024     /* 输入缓冲区的最大长度 */
#define BUF_CHAR(c) ((c) ? (c) : ' ')  /* 将 '\0' 转换为空格，用于填充输入框空白位置 */

static char *nv_ncurses_get_input(Options *op,
                                  const char *def, const char *msg)
{
    DataStruct *d = (DataStruct *) op->ui.priv;
    int msg_len, width, input_len, buf_len, def_len;
    int input_x, input_y, i, w, h, x, y, c, lines, ch, color, redraw;
    char *tmp, buf[MAX_BUF_LEN];
    TextRows *t;

    if (!msg) return NULL;

    /* 检查是否发生了窗口大小变化 */
    nv_ncurses_check_resize(d, FALSE);

    /* 选择输入框的显示颜色：有颜色支持时用绿色，否则用反色 */
    color = d->use_color ? NV_NCURSES_INPUT_COLOR : A_REVERSE;

    /* 在提示消息末尾追加 ": " 构成完整提示 */

    msg_len = strlen(msg) + 2;
    tmp = (char *) malloc(msg_len + 1);
    snprintf(tmp, msg_len, "%s: ", msg);

    /* 将默认值复制到输入缓冲区作为初始内容 */

    memset(buf, 0, MAX_BUF_LEN);
    if (def) strncpy(buf, def, MAX_BUF_LEN - 1);
    buf[MAX_BUF_LEN - 1] = '\0';

 draw_get_input:
    /* 绘制/重绘入口点（窗口大小变化时会跳回此处重绘） */

    /* 销毁已有的消息区域（如有） */

    if (d->message) {
        nv_ncurses_destroy_region(d->message);
        d->message = NULL;
    }

    /*
     * 计算输入框的显示宽度：取默认值长度的两倍，但不小于 MIN_INPUT_LEN(32)
     */

    def_len = def ? strlen(def) : 0;
    input_len = NV_MAX(def_len * 2, MIN_INPUT_LEN);
    width = d->width - 4;  /* 可用宽度 = 终端宽度 - 左右边距各2 */

    /* 将提示消息格式化为按宽度折行的文本行 */

    t = d->format_text_rows(NULL, tmp, width, TRUE);

    /*
     * 布局策略：如果提示消息和输入框能放在同一行就放一行，
     * 否则将输入框放到提示消息下方的新行
     */

    if ((msg_len + input_len + 1) < width) {
        input_x = 1 + msg_len;  /* 输入框紧跟在提示消息后面 */
        lines = 1;
    } else {
        input_x = 2;            /* 输入框另起一行，缩进2字符 */
        lines = t->n + 1;       /* 总行数 = 提示消息行数 + 输入框1行 */
    }

    input_y = lines;  /* 输入框的 y 坐标（区域内相对坐标） */

    /*
     * 计算消息区域的尺寸和在屏幕上的起始位置
     * 区域垂直位置约在屏幕的 1/3 处
     */

    w = d->width - 2;
    h = lines + 2;  /* 高度 = 内容行数 + 上下边距各1行 */
    x = 1;
    y = ((d->height - (3 + h)) / 3) + 1;

    /* 创建消息区域并输出提示文本 */

    d->message = nv_ncurses_create_region(d, x, y, w, h,
                                          NV_NCURSES_MESSAGE_COLOR,
                                          NV_NCURSES_MESSAGE_NO_COLOR);

    nv_ncurses_format_print(d, d->message, 1, 1, d->message->w - 2, lines, t);

    /* 释放格式化后的文本行 */

    nv_ncurses_free_text_rows(t);

    /* 将输入框宽度限制在区域可用宽度内 */

    input_len = NV_MIN(input_len, width - 2);

    curs_set(1); /* 显示光标（输入模式需要光标可见） */

    c = buf_len = strlen(buf);  /* c=光标位置, buf_len=当前缓冲区内容长度 */

    redraw = TRUE; /* 首次进入循环时强制重绘输入框 */

    /* 将区域内相对坐标转换为屏幕绝对坐标 */

    input_x += d->message->x;
    input_y += d->message->y;

    /* ===== 主输入循环 ===== */
    do {
        /* x 是输入框的滚动偏移量：当光标超出显示范围时自动滚动 */
        x = NV_MAX(c - (input_len - 1), 0);

        /* 重绘输入框内容 */

        if (redraw) {
            /* 逐字符绘制输入框中可见的部分 */
            for (i = 0; i < input_len; i++) {
                mvwaddch(nv_stdscr, input_y, input_x + i,
                         BUF_CHAR(buf[i + x]) | color);
            }

            /* 如果文本向左滚动了，在输入框左侧显示 '<' 箭头提示 */

            if (x > 0) {
                mvwaddch(nv_stdscr, input_y, input_x - 1,
                         '<' | d->message->attr);
            } else {
                mvwaddch(nv_stdscr, input_y, input_x - 1,
                         ' ' | d->message->attr);
            }

            /* 如果还有更多文本在右侧不可见，显示 '>' 箭头提示 */

            if (buf_len > (input_len - 1 + x)) {
                mvwaddch(nv_stdscr, input_y, input_x + input_len,
                         '>' | d->message->attr);
            } else {
                mvwaddch(nv_stdscr, input_y, input_x + input_len,
                         ' ' | d->message->attr);
            }

            redraw = FALSE;
        }

        /* 将光标移动到当前编辑位置 */

        wmove(nv_stdscr, input_y, input_x - x + c);
        wrefresh(nv_stdscr);

        /* 等待用户按键输入；如果期间发生窗口大小变化则重绘 */

        if (nv_ncurses_check_resize(d, FALSE)) goto draw_get_input;
        ch = wgetch(nv_stdscr);

        switch (ch) {

        case NV_NCURSES_BACKSPACE:
        case KEY_BACKSPACE:

            /*
             * Backspace：删除光标前的字符。
             * 将光标后的所有字符前移一位，然后递减光标位置和缓冲区长度。
             */

            if (c <= 0) break;
            for (i = c; i <= buf_len; i++) buf[i-1] = buf[i];
            c--;
            buf_len--;
            redraw = TRUE;
            break;

        case KEY_DC:

            /*
             * Delete：删除光标处的字符。
             * 将光标后的所有字符前移一位，递减缓冲区长度（光标位置不变）。
             */

            if (c == buf_len) break;
            for (i = c; i < buf_len; i++) buf[i] = buf[i+1];
            buf_len--;
            redraw = TRUE;
            break;

        case KEY_LEFT:
            /* 左箭头：光标左移 */
            if (c > 0) {
                c--;
                redraw = TRUE;
            }
            break;

        case KEY_RIGHT:
            /* 右箭头：光标右移 */
            if (c < buf_len) {
                c++;
                redraw = TRUE;
            }
            break;

        case NV_NCURSES_CTRL('L'):
            /* Ctrl+L：强制重绘整个屏幕 */
            nv_ncurses_check_resize(d, TRUE);
            goto draw_get_input;
            break;
        }

        /*
         * 如果是可打印字符，则在光标位置插入该字符：
         * 将光标后的内容后移一位，在空出的位置插入新字符。
         */

        if (isprint(ch)) {
            if (buf_len < (MAX_BUF_LEN - 1)) {
                for (i = buf_len; i > c; i--) buf[i] = buf[i-1];
                buf[c] = (char) ch;
                buf_len++;
                c++;
                redraw = TRUE;
            }
        }

        /* 调试模式下，在消息区域显示当前光标位置和按键信息 */
        if (op->debug) {
            mvprintw(d->message->y, d->message->x,
                     "c: %3d  ch: %04o (%d)", c, ch, ch);
            wclrtoeol(nv_stdscr);
            redraw = TRUE;
        }

    } while (ch != NV_NCURSES_ENTER);  /* Enter 键结束输入 */

    /* 清理：销毁消息区域，隐藏光标 */

    nv_ncurses_destroy_region(d->message);
    d->message = NULL;

    curs_set(0); /* 重新隐藏光标 */
    wrefresh(nv_stdscr);

    /* 释放临时字符串，复制并返回用户输入 */
    free(tmp);
    tmp = strdup(buf);
    return tmp;

} /* nv_ncurses_get_input() */



/*
 * nv_ncurses_message() - 按指定级别显示消息对话框。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   level - 消息级别（NV_MSG_LEVEL_LOG/MESSAGE/WARNING/ERROR）
 *   msg   - 消息文本
 *
 * 处理流程：
 *   1. 忽略 LOG 级别消息（ncurses UI 不显示纯日志消息）
 *   2. 根据消息级别确定前缀（"WARNING: " 或 "ERROR: "，普通消息无前缀）
 *   3. 创建消息区域，显示消息文本
 *   4. 在底部绘制居中的 "OK" 按钮
 *   5. 等待用户按 Enter 确认
 *   6. 播放按钮按下的动画效果（按钮下移再恢复）
 *   7. 销毁消息区域
 *
 * 注意：进入此函数时可能已存在消息区域（如进度条显示期间出错），
 *       此时会销毁旧区域，后续的 status_update/status_end 需要重建。
 */

static void nv_ncurses_message(Options *op, int level, const char *msg)
{
    DataStruct *d = (DataStruct *) op->ui.priv;
    int w, h, x, y, ch;
    char *prefix;

    if (!msg) return;

    /* ncurses UI 当前忽略纯日志级别的消息 */

    if (level == NV_MSG_LEVEL_LOG) return;

    /* 根据消息级别确定前缀文本 */

    switch(level) {
    case NV_MSG_LEVEL_MESSAGE:
        prefix = NULL;          /* 普通消息无前缀 */
        break;
    case NV_MSG_LEVEL_WARNING:
        prefix = "WARNING: ";   /* 警告消息加 "WARNING: " 前缀 */
        break;
    case NV_MSG_LEVEL_ERROR:
        prefix = "ERROR: ";     /* 错误消息加 "ERROR: " 前缀 */
        break;
    default:
        return;                 /* 未知级别直接忽略 */
    }

 draw_message:
    /* 绘制/重绘入口点 */

    if (d->message) {

        /*
         * 进入此函数时可能已有消息区域（例如在显示进度条期间遇到了
         * 需要报告的错误）。为处理此情况，先销毁已有的消息区域；
         * nv_ncurses_status_update() 和 nv_ncurses_status_end() 会在
         * 需要时自行重建消息区域。
         */

        nv_ncurses_destroy_region(d->message);
        d->message = NULL;
    }

    /* 创建消息区域并在其中显示消息文本 */

    nv_ncurses_do_message_region(d, prefix, msg, FALSE, 2);

    /* 设置 OK 按钮的尺寸和位置（居中显示在消息区域底部） */

    w = 6;                          /* 按钮宽度 */
    h = 1;                          /* 按钮高度 */
    x = (d->message->w - w) / 2;   /* 水平居中 */
    y = d->message->h - 2;         /* 距消息区域底部2行 */

    /* 绘制 OK 按钮（高亮、未按下状态） */

    nv_ncurses_draw_button(d, d->message, x, y, w, h, "OK",
                           TRUE, FALSE);
    wrefresh(nv_stdscr);

    /* 等待用户按 Enter 键确认 */

    do {
        /* 如果发生了窗口大小变化，跳回顶部重绘整个对话框 */

        if (nv_ncurses_check_resize(d, FALSE)) goto draw_message;
        ch = wgetch(nv_stdscr);

        switch (ch) {
        case NV_NCURSES_CTRL('L'):
            /* Ctrl+L：强制重绘屏幕 */
            nv_ncurses_check_resize(d, TRUE);
            goto draw_message;
            break;
        }
    } while (ch != NV_NCURSES_ENTER);

    /* 播放按钮按下动画：先绘制按下状态，再恢复正常状态 */

    nv_ncurses_erase_button(d->message, x, y, w, h);
    nv_ncurses_draw_button(d, d->message, x, y, w, h, "OK", TRUE, TRUE);  /* 按下 */
    wrefresh(nv_stdscr);
    usleep(NV_NCURSES_BUTTON_PRESS_TIME);  /* 等待 125ms */

    nv_ncurses_erase_button(d->message, x, y, w, h);
    nv_ncurses_draw_button(d, d->message, x, y, w, h, "OK", TRUE, FALSE); /* 恢复 */
    wrefresh(nv_stdscr);
    usleep(NV_NCURSES_BUTTON_PRESS_TIME);  /* 等待 125ms */

    /* 销毁消息区域 */

    nv_ncurses_destroy_region(d->message);
    d->message = NULL;
    wrefresh(nv_stdscr);

} /* nv_ncurses_message() */




/*
 * nv_ncurses_command_output() - 显示命令执行输出。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   msg - 命令输出文本
 *
 * 当前实现：
 *   此函数当前未实现任何功能，直接返回。
 *   ncurses UI 暂时不显示命令输出（如内核模块编译日志等）。
 *   命令输出仍会被写入日志文件。
 */

static void nv_ncurses_command_output(Options *op, const char *msg)
{
    /*
     * ncurses UI 当前不显示命令输出
     */

    return;

} /* nv_ncurses_command_output() */




/*
 * nv_ncurses_approve_command_list() - 显示将要执行的命令列表并请求用户批准。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   cl    - 待执行的命令列表（包含所有安装操作的描述）
 *   descr - 安装目标描述（如 "NVIDIA Accelerated Graphics Driver..."）
 *
 * 返回值：
 *   TRUE  - 用户选择了 "Yes"（批准执行）
 *   FALSE - 用户选择了 "No"（拒绝执行，将导致安装中止）
 *
 * 处理流程：
 *   1. 构建询问文本（包含安装目标描述）
 *   2. 将命令列表转换为可显示的文本
 *   3. 使用带分页浏览的提示对话框显示命令列表和 Yes/No 按钮
 *   4. 用户可以滚动查看所有命令后做出选择
 */

static int nv_ncurses_approve_command_list(Options *op, CommandList *cl,
                                           const char *descr)
{
    DataStruct *d = (DataStruct *) op->ui.priv;
    char *commandlist, *question;
    int ret, len;
    const char *buttons[2] = {"Yes", "No"};

    /* 构建询问字符串 */

    len = strlen(descr) + 256;
    question = (char *) malloc(len + 1);
    snprintf(question, len, "The following operations will be performed to "
             "install the %s.  Is this acceptable?", descr);

    /* 将命令列表转换为可分页显示的文本 */
    commandlist = nv_ncurses_create_command_list_text(d, cl);

    /* 显示带分页浏览的提示对话框 */
    ret = nv_ncurses_paged_prompt(op, question, "Proposed CommandList",
                                  commandlist, buttons, 2, 0);

    free(question);
    free(commandlist);

    return ret == 0;  /* 返回值 0 对应 "Yes" 按钮 */
} /* nv_ncurses_approve_command_list() */




/*
 * nv_ncurses_yes_no() - 显示是/否选择对话框。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   def - 默认选择（TRUE=默认Yes, FALSE=默认No）
 *   msg - 问题文本
 *
 * 返回值：
 *   TRUE  - 用户选择了 "Yes"
 *   FALSE - 用户选择了 "No"
 *
 * 实现方式：委托给 nv_ncurses_multiple_choice()，提供 "Yes" 和 "No" 两个按钮。
 */

static int nv_ncurses_yes_no(Options *op, const int def, const char *msg)
{
    const char *buttons[2] = { "Yes", "No" };

    /* def=TRUE 时默认选中按钮索引为 0(Yes)，def=FALSE 时为 1(No) */
    return (nv_ncurses_multiple_choice(op, msg, buttons, 2,
                                       (def) ? 0 : 1) == 0);
} /* nv_ncurses_yes_no() */


/*
 * nv_ncurses_multiple_choice() - 显示带多个选择按钮的对话框。
 *
 * 参数：
 *   op             - 安装器选项结构
 *   question       - 问题文本
 *   buttons        - 按钮标签字符串数组
 *   num_buttons    - 按钮数量
 *   default_button - 默认选中按钮的索引
 *
 * 返回值：
 *   用户选择的按钮索引（0 到 num_buttons-1）
 *
 * 实现方式：委托给 nv_ncurses_paged_prompt()，不提供分页文本。
 */

static int nv_ncurses_multiple_choice(Options *op, const char *question,
                                      const char * const *buttons, int num_buttons,
                                      int default_button)
{
    return nv_ncurses_paged_prompt(op, question, NULL, NULL, buttons,
                                   num_buttons, default_button);
} /* nv_ncurses_multiple_choice() */




/*
 * nv_ncurses_status_begin() - 初始化并显示进度条。
 *
 * 参数：
 *   op    - 安装器选项结构
 *   title - 进度条标题（如 "Installing..."）
 *   msg   - 进度条下方的描述消息（如当前操作的文件名）
 *
 * 处理流程：
 *   1. 检查窗口是否需要重绘
 *   2. 缓存进度条标题（后续重绘时需要）
 *   3. 创建进度条区域（包含标题、分隔线、空进度条）
 *   4. 在进度条上方显示描述消息
 *   5. 刷新屏幕
 *
 * 此函数与 status_update() 和 status_end() 配合使用：
 *   begin -> 多次 update -> end
 */

static void nv_ncurses_status_begin(Options *op,
                                    const char *title, const char *msg)
{
    DataStruct *d = (DataStruct *) op->ui.priv;

    nv_ncurses_check_resize(d, FALSE);

    /* 缓存进度条标题，在窗口重绘时使用 */

    d->progress_title = strdup(title);

    /* 创建进度条区域（含标题、分隔线、空进度条） */

    nv_ncurses_do_progress_bar_region(d);

    /* 在分隔线上方写入描述消息（如有必要会截断） */

    nv_ncurses_do_progress_bar_message(d, msg, d->message->h - 3,
                                       d->message->w);

    wrefresh(nv_stdscr);

} /* nv_ncurses_status_begin() */




/*
 * nv_ncurses_status_update() - 更新进度条显示。
 *
 * 参数：
 *   op      - 安装器选项结构
 *   percent - 完成百分比（0.0 到 1.0 之间的浮点数）
 *   msg     - 进度条上方显示的状态消息
 *
 * 处理流程：
 *   1. 检查窗口是否需要重绘，如果消息区域被销毁则重建进度条区域
 *   2. 以非阻塞模式检查是否有按键输入（如 Ctrl+L 重绘请求）
 *   3. 计算进度条的填充长度
 *   4. 更新状态消息文本
 *   5. 绘制进度条（已完成部分用反色显示，未完成部分正常显示）
 *   6. 在进度条中间显示百分比数字（如 "42%"）
 *
 * 进度条视觉效果说明：
 *   - 有颜色模式：绿色背景表示已完成，黑色背景表示未完成，百分比数字居中
 *   - 无颜色模式：用反色块表示已完成，'-' 字符表示未完成，两端有 '[' ']' 边框
 */

static void nv_ncurses_status_update(Options *op, const float percent,
                                     const char *msg)
{
    int i, n, h, ch, w;
    int p[4];        /* 百分比字符串在进度条中的显示位置（4个字符的位置） */
    char v[4];       /* 百分比字符串内容（如 '4','2','%','\0' 表示 "42%"） */
    DataStruct *d = (DataStruct *) op->ui.priv;
    int color_offset, color_flag;
    char status_char;

    /*
     * 如果消息区域被销毁（可能被 nv_ncurses_message() 占用后释放）
     * 或窗口大小发生了变化，需要重建整个进度条区域。
     */

    if (nv_ncurses_check_resize(d, FALSE) || !d->message) {
        if (d->message) nv_ncurses_destroy_region(d->message);
        nv_ncurses_do_progress_bar_region(d);
    }

    /* 临时将 getch() 设为非阻塞模式以消费待处理的按键事件 */

    nodelay(nv_stdscr, TRUE);

    while ((ch = wgetch(nv_stdscr)) != ERR) {
        /*
         * 如果用户按了 Ctrl+L 请求重绘屏幕，
         * 则重建整个进度条区域
         */
        if (ch == NV_NCURSES_CTRL('L')) {
            nv_ncurses_check_resize(d, TRUE);
            nv_ncurses_destroy_region(d->message);
            nv_ncurses_do_progress_bar_region(d);
        }
    }

    /* 恢复 getch() 为阻塞模式 */

    nodelay(nv_stdscr, FALSE);

    /* 计算进度条的填充宽度（已完成部分的字符数） */

    w = d->message->w - 2;                       /* 进度条可用宽度 */
    n = ((int) (percent * (float) w));            /* 已完成的字符数 */
    n = NV_MAX(n, 2);                             /* 至少显示2个字符 */

    init_position(p, d->message->w);              /* 计算百分比文本在进度条中的居中位置 */
    init_percentage_string(v, (int) (100.0 * percent)); /* 将百分比转换为字符串 */

    h = d->message->h;

    /* 更新进度条上方的状态消息 */

    nv_ncurses_do_progress_bar_message(d, msg, h - 3, d->message->w - 2);

    /* 根据颜色支持情况选择绘制参数 */

    if (d->use_color) {
        color_offset = 0;                         /* 有颜色时无边框偏移 */
        color_flag = NV_NCURSES_INPUT_COLOR;      /* 使用绿色/黑色颜色对 */
        status_char = ' ';                        /* 未完成部分用空格 */
    } else {
        color_offset = 1;                         /* 无颜色时预留 '[' ']' 边框位置 */
        color_flag = 0;                           /* 不使用颜色 */
        status_char = '-';                        /* 未完成部分用 '-' */
    }

    /* 绘制进度条主体 */

    for (i = 1 + color_offset; i <= w - color_offset; i++) {
        /* 已完成部分用 A_REVERSE（反色）显示 */
        int reverse_flag = i > n - color_offset ? 0 : A_REVERSE;
        mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + i,
                 choose_char(i, p, v, ' ') | reverse_flag | color_flag);
    }

    /* 在进度条未完成区域中显示百分比数字 */

    for (i = 0; i < 4; i++) {
        if (p[i] >= n + 1 - color_offset) {
            mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + p[i],
                     (v[i] ? v[i] : status_char) | color_flag);
        }
    }

    wrefresh(nv_stdscr);

} /* nv_ncurses_status_update() */


/*
 * nv_ncurses_update_indeterminate() - 更新不确定进度动画。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   msg - 进度条上方显示的状态消息
 *
 * 当操作的总进度无法预估时（如检测硬件、下载文件等），使用此函数
 * 显示一个滚动的动画条代替确定百分比的进度条。
 *
 * 动画原理：
 *   使用一个 32 位的位模式（pattern），初始值 0x7ff（低11位为1），
 *   每次调用时将该模式循环左移1位。进度条中每个位置根据 pattern 的
 *   对应位来决定是否反色显示，形成一个不断滚动的光带效果。
 *
 * 注意：此函数包含 100ms 的 usleep 调用来控制动画速度。
 */

static void nv_ncurses_update_indeterminate(Options *op, const char *msg)
{
    DataStruct *d = op->ui.priv;
    static uint32_t pattern = 0x7ff;  /* 滚动动画位模式（11个连续1位构成光带） */
    int x;

    /* 如有必要，重建进度条区域 */
    if (nv_ncurses_check_resize(d, FALSE) || !d->message) {
        if (d->message) nv_ncurses_destroy_region(d->message);
        nv_ncurses_do_progress_bar_region(d);
    }

    /* 更新状态消息 */
    nv_ncurses_do_progress_bar_message(d, msg, d->message->h - 3,
                                       d->message->w - 2);

    /* 根据 pattern 位模式绘制滚动动画 */
    for (x = 1; x < d->message->w - 1; x++) {
        int color_flag;

        if (d->use_color) {
            color_flag = NV_NCURSES_INPUT_COLOR;
        } else {
            color_flag = 0;
        }

        /* 如果当前位置对应的 pattern 位为1，则反色显示（形成光带） */
        if (pattern & (1 << (x % 32))) {
            color_flag |= A_REVERSE;
        }

        mvwaddch(nv_stdscr, d->message->y + d->message->h - 2,
                 d->message->x + x, ' ' | color_flag);
    }

    wrefresh(nv_stdscr);

    /* 循环左移位模式，使光带向右滚动 */
    pattern = pattern << 1 | (pattern >> 31 & 1);

    usleep(100000);  /* 等待 100ms 控制动画速度 */
}



/*
 * nv_ncurses_status_end() - 将进度条绘制到 100% 完成状态。
 *
 * 参数：
 *   op  - 安装器选项结构
 *   msg - 最终的状态消息
 *
 * 处理流程：
 *   1. 如有必要重建进度条区域
 *   2. 将进度条绘制为完全填满状态（100%）
 *   3. 更新状态消息
 *   4. 释放进度条标题缓存
 *   5. 注意：此处故意不释放消息区域，因为后续可能还需要它
 */

static void nv_ncurses_status_end(Options *op, const char *msg)
{
    int i, n, h;
    int p[4];       /* 百分比文本在进度条中的位置 */
    char v[4];      /* 百分比文本内容（"100%"） */
    DataStruct *d = (DataStruct *) op->ui.priv;

    /*
     * 如果消息区域被销毁或窗口大小变化，重建进度条区域
     */

    if (nv_ncurses_check_resize(d, FALSE) || !d->message) {
        if (d->message) nv_ncurses_destroy_region(d->message);
        nv_ncurses_do_progress_bar_region(d);
    }

    n = d->message->w - 2;  /* 进度条可用宽度 */

    init_position(p, d->message->w);       /* 计算 "100%" 文本的居中位置 */
    init_percentage_string(v, 100.0);      /* 生成 "100%" 字符数组 */

    h = d->message->h;

    /* 更新状态消息 */

    nv_ncurses_do_progress_bar_message(d, msg, h - 3, d->message->w - 2);

    /* 绘制完全填满的进度条 */

    if (d->use_color) {
        /* 有颜色模式：整条反色显示，中间嵌入 "100%" 文字 */
        for (i = 1; i < (n+1); i++) {
            mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + i,
                    choose_char(i, p, v, ' ') |
                    A_REVERSE | NV_NCURSES_INPUT_COLOR);
        }
    } else {
        /* 无颜色模式：整条反色显示（在 '[' ']' 边框内） */
        for (i = 2; i < (n); i++) {
            mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + i,
                    choose_char(i, p, v, ' ') | A_REVERSE);
        }
    }

    wrefresh(nv_stdscr);

    /* 释放进度条标题缓存 */
    free(d->progress_title);
    d->progress_title = NULL;

    /* 故意不释放消息区域——后续操作可能还会使用它 */

} /* nv_ncurses_status_end() */




/*
 * nv_ncurses_close() - 关闭 ncurses UI 并释放所有资源。
 *
 * 参数：
 *   op - 安装器选项结构（可能为 NULL，见下方注意事项）
 *
 * 处理流程：
 *   1. 如果 op 非空，销毁页眉和页脚区域，释放 DataStruct
 *   2. 清空屏幕
 *   3. 调用 endwin() 结束 ncurses 模式，恢复终端状态
 *
 * 注意：当此函数从信号处理器（如 SIGTERM）中调用时，op 可能为 NULL，
 *       此时仅执行 ncurses 清理（清屏 + endwin），不释放 DataStruct。
 */

static void nv_ncurses_close(Options *op)
{
    DataStruct *d = NULL;

    if (op) {

        /* op 可能为 NULL（从信号处理器中调用时） */

        d = (DataStruct *) op->ui.priv;
        nv_ncurses_destroy_region(d->header);
        nv_ncurses_destroy_region(d->footer);
        free(d);
    }

    wclear(nv_stdscr);   /* 清空屏幕 */
    wrefresh(nv_stdscr);  /* 刷新清空后的屏幕 */

    endwin();  /* 结束 ncurses 模式，恢复终端的原始设置 */
}



/****************************************************************************/
/*
 * ===== 页眉和页脚操作的内部辅助函数 =====
 */




/*
 * nv_ncurses_set_header() - 在页眉区域写入标题文本。
 *
 * 参数：
 *   d     - ncurses UI 私有数据结构
 *   title - 要显示的标题字符串
 *
 * 处理流程：
 *   1. 复制标题字符串并缓存到 d->title（释放旧值）
 *   2. 计算标题在页眉区域内的水平居中位置
 *   3. 清空页眉区域，然后在居中位置写入标题
 *
 * 注意：此函数不调用 wrefresh()，调用方需要自行刷新屏幕。
 */

static void nv_ncurses_set_header(DataStruct *d, const char *title)
{
    int x, y;
    char *tmp;

    tmp = strdup(title);

    if (d->title) free(d->title);
    d->title = tmp;

    /* 计算水平居中位置 */
    x = (d->header->w - strlen(d->title)) / 2;
    y = 0;

    /* 设置页眉属性，清空区域，写入标题 */
    wattrset(nv_stdscr, d->header->attr);

    nv_ncurses_clear_region(d->header);
    mvwaddstr(nv_stdscr, d->header->y + y, d->header->x + x, (char *) d->title);
    wattrset(nv_stdscr, A_NORMAL);

} /* nv_ncurses_set_header() */




/*
 * nv_ncurses_set_footer() - 在页脚区域写入左侧和右侧文本。
 *
 * 参数：
 *   d     - ncurses UI 私有数据结构
 *   left  - 页脚左侧文本（如安装器名称）
 *   right - 页脚右侧文本（如 "www.nvidia.com" 或分页百分比）
 *
 * 处理流程：
 *   1. 复制两个字符串并缓存到 d->footer_left 和 d->footer_right
 *   2. 清空页脚区域
 *   3. 在左侧偏移1字符处写入左侧文本
 *   4. 在右侧对齐处写入右侧文本
 *
 * 注意：此函数不调用 wrefresh()，调用方需要自行刷新屏幕。
 */

static void nv_ncurses_set_footer(DataStruct *d, const char *left,
                                  const char *right)
{
    int x, y;
    char *tmp0, *tmp1;

    tmp0 = strdup(left);
    tmp1 = strdup(right);

    /* 释放旧的缓存字符串 */
    if (d->footer_left) free(d->footer_left);
    if (d->footer_right) free(d->footer_right);

    d->footer_left = tmp0;
    d->footer_right = tmp1;

    wattrset(nv_stdscr, d->footer->attr);

    nv_ncurses_clear_region(d->footer);

    /* 在左侧写入文本（缩进1字符） */
    if (d->footer_left) {
        y = 0;
        x = 1;
        mvwaddstr(nv_stdscr, d->footer->y + y, d->footer->x + x,
                  d->footer_left);
    }
    /* 在右侧写入文本（右对齐，留1字符边距） */
    if (d->footer_right) {
        y = 0;
        x = d->footer->w - strlen(d->footer_right) - 1;
        mvwaddstr(nv_stdscr, d->footer->y + y, d->footer->x + x,
                  d->footer_right);
    }

    wattrset(nv_stdscr, A_NORMAL);

} /* nv_ncurses_set_footer() */




/****************************************************************************/
/*
 * ===== RegionStruct 操作的内部辅助函数 =====
 */




/*
 * nv_ncurses_create_region() - 在指定位置创建新的屏幕区域。
 *
 * 参数：
 *   d             - ncurses UI 私有数据结构
 *   x, y          - 区域在 stdscr 上的起始位置
 *   w, h          - 区域的宽度和高度
 *   color         - 有颜色支持时使用的显示属性
 *   no_color_attr - 无颜色支持时使用的回退显示属性
 *
 * 返回值：
 *   新创建的 RegionStruct 指针（调用方负责通过 destroy_region 释放）
 *
 * 处理流程：
 *   1. 分配 RegionStruct 并设置位置、尺寸
 *   2. 根据颜色支持选择显示属性
 *   3. 创建空格填充的行字符串（用于快速清空区域）
 *   4. 用当前属性清空区域内容
 *
 * 注意：此函数不调用 wrefresh()，调用方需要自行刷新。
 */

static RegionStruct *nv_ncurses_create_region(DataStruct *d,
                                              int x, int y, int w, int h,
                                              int color,
                                              int no_color_attr)
{
    RegionStruct *region =
        (RegionStruct *) malloc(sizeof(RegionStruct));

    region->x = x;
    region->y = y;
    region->w = w;
    region->h = h;

    /* 根据是否支持颜色选择显示属性 */
    if (d->use_color) region->attr = color;
    else              region->attr = no_color_attr;

    /* 创建一个由空格填充的字符串，长度等于区域宽度，用于快速清空区域 */

    region->line = (char *) malloc(w + 1);
    memset(region->line, ' ', w);
    region->line[w] = '\0';

    /* 用当前属性清空区域 */

    wattrset(nv_stdscr, region->attr);
    nv_ncurses_clear_region(region);
    wattrset(nv_stdscr, A_NORMAL);

    return region;

} /* nv_ncurses_create_region() */




/*
 * nv_ncurses_clear_region() - 用空格填充清空区域的每一行。
 *
 * 参数：
 *   region - 要清空的区域结构
 *
 * 处理流程：
 *   遍历区域的每一行，在每行的起始位置输出 region->line（空格串），
 *   从而用空格覆盖区域中的所有内容。
 *
 * 注意：此函数不调用 wrefresh()，也不设置任何显示属性。
 *       调用前应通过 wattrset() 设置好期望的属性。
 */

static void nv_ncurses_clear_region(RegionStruct *region)
{
    int i;

    for (i = region->y; i < (region->y + region->h); i++) {
        mvwaddstr(nv_stdscr, i, region->x, region->line);
    }
} /* nv_ncurses_clear_region() */




/*
 * nv_ncurses_destroy_region() - 清空并释放 RegionStruct。
 *
 * 参数：
 *   region - 要销毁的区域结构（可以为 NULL，此时直接返回）
 *
 * 处理流程：
 *   1. 使用 A_NORMAL 属性清空区域（将区域恢复为默认外观）
 *   2. 释放空格行字符串
 *   3. 释放 RegionStruct 本身
 *
 * 注意：此函数不调用 wrefresh()。
 */

static void nv_ncurses_destroy_region(RegionStruct *region)
{
    if (!region) return;

    wattrset(nv_stdscr, A_NORMAL);
    nv_ncurses_clear_region(region);
    free(region->line);
    free(region);

} /* nv_ncurses_destroy_region() */
    

                                  

/****************************************************************************/
/*
 * ===== 按钮绘制的内部辅助函数 =====
 */




/*
 * nv_ncurses_draw_button() - 在指定区域的指定位置绘制一个按钮。
 *
 * 参数：
 *   d      - ncurses UI 私有数据结构
 *   region - 按钮所在的区域（坐标相对于此区域）
 *   x, y   - 按钮在区域内的位置（相对坐标）
 *   w, h   - 按钮的宽度和高度
 *   str    - 按钮标签文本（如 "OK"、"Yes"、"No"）
 *   hilite - 是否高亮显示（TRUE 时标签文本反色显示，表示当前选中）
 *   down   - 是否以按下状态绘制（TRUE 时按钮整体向右下偏移1像素，
 *            模拟按钮被按下的视觉效果）
 *
 * 绘制细节：
 *   1. 如果 down=TRUE，将按钮坐标右移和下移各1（模拟按下效果）
 *   2. 先用背景属性填充整个按钮矩形区域
 *   3. 然后在按钮中央写入标签文本（水平居中、垂直居中）
 *   4. 高亮模式下标签文本额外添加反色属性
 */

static void nv_ncurses_draw_button(DataStruct *d, RegionStruct *region,
                                   int x, int y, int w, int h,
                                   const char *str, bool hilite, bool down)
{
    int i, j, n, attr = 0;

    /* 按下状态：坐标向右下各偏移1，产生按下的视觉效果 */
    if (down) x++, y++;

    /* 计算标签文本在按钮内的水平居中偏移 */
    n = strlen(str);
    n = (n > w) ? 0 : ((w - n) / 2);

    /* 根据颜色支持和高亮状态确定按钮背景属性 */
    if (d->use_color) {
        attr = NV_NCURSES_BUTTON_COLOR;   /* 有颜色：白字红底 */
    } else if (hilite) {
        attr = A_REVERSE;                  /* 无颜色但高亮：反色 */
    } else {
        attr = 0;                          /* 无颜色无高亮：正常 */
    }

    /* 用背景属性填充按钮矩形区域 */
    for (j = y; j < (y + h); j++) {
        for (i = x; i < (x + w); i++) {
            mvwaddch(nv_stdscr, region->y + j, region->x + i, ' ' | attr);
        }
    }

    /* 高亮时额外添加反色属性到标签文本 */
    if (hilite) attr |= A_REVERSE;

    /* 在按钮中央写入标签文本 */
    wattron(nv_stdscr, attr);
    mvwaddstr(nv_stdscr, region->y + y + h/2, region->x + x + n, str);
    wattroff(nv_stdscr, attr);

} /* nv_ncurses_draw_button() */




/*
 * nv_ncurses_erase_button() - 擦除指定位置的按钮。
 *
 * 参数：
 *   region - 按钮所在的区域
 *   x, y   - 按钮在区域内的位置
 *   w, h   - 按钮的宽度和高度
 *
 * 将按钮占据的矩形区域用空格和区域默认属性覆盖，
 * 使按钮从屏幕上消失。用于按钮动画效果中清除旧状态。
 *
 * 注意：擦除范围包含 (x,y) 到 (x+w, y+h)，即比按钮稍大1行1列，
 *       以确保完全覆盖 down=TRUE 状态下偏移绘制的按钮。
 */

static void nv_ncurses_erase_button(RegionStruct *region,
                                    int x, int y, int w, int h)
{
    int i, j;

    for (j = y; j <= (y + h); j++) {
        for (i = x; i <= (x + w); i++) {
            mvwaddch(nv_stdscr, region->y + j, region->x + i,
                     ' ' | region->attr);
        }
    }

} /* nv_ncurses_erase_button() */




/*****************************************************************************/
/*
 * ===== 区域绘制辅助函数 =====
 *
 * nv_ncurses_do_message_region()       - 创建消息区域并显示文本
 * nv_ncurses_do_progress_bar_region()  - 创建进度条区域
 * nv_ncurses_do_progress_bar_message() - 在进度条区域中更新消息文本
 */

/*
 * nv_ncurses_do_message_region() - 创建消息区域并在其中显示格式化的文本。
 *
 * 参数：
 *   d               - ncurses UI 私有数据结构
 *   prefix          - 消息前缀（如 "WARNING: "），可以为 NULL
 *   msg             - 消息正文
 *   top             - 垂直定位方式：
 *                     TRUE  = 紧挨页眉下方（y=2）
 *                     FALSE = 屏幕高度的 1/3 处
 *   num_extra_lines - 消息文本下方预留的额外行数
 *                     （用于放置按钮等控件，如传入 2 则预留 2 行）
 *
 * 处理流程：
 *   1. 将消息文本格式化为适合区域宽度的行
 *   2. 计算区域总高度 = 文本行数 + 额外行数 + 上下边距
 *   3. 根据 top 参数确定垂直位置
 *   4. 创建消息区域并打印格式化文本
 *   5. 区域创建后保存到 d->message，调用方使用完毕后负责销毁
 */

static void nv_ncurses_do_message_region(DataStruct *d, const char *prefix,
                                         const char *msg, int top,
                                         int num_extra_lines)
{
    int w, h, x, y;
    TextRows *t;

    /*
     * 计算所需的宽度和高度
     * （需要考虑调用方请求的额外行数，用于放置按钮等）
     */

    w = d->width - 2;                              /* 区域宽度 = 终端宽度 - 左右边距 */
    t = d->format_text_rows(prefix, msg, w - 2, TRUE);  /* 格式化文本（留2字符内边距） */
    h = t->n + num_extra_lines + 2;                /* 区域高度 = 文本行数 + 额外行 + 上下边距 */

    /*
     * 计算区域垂直起始位置：
     * top=TRUE 时紧挨页眉下方（y=2），
     * top=FALSE 时放在屏幕的约 1/3 处
     */

    x = 1;
    if (top) y = 2;
    else     y = ((d->height - (3 + h)) / 3) + 1;

    /* 创建消息区域 */

    d->message = nv_ncurses_create_region(d, x, y, w, h,
                                          NV_NCURSES_MESSAGE_COLOR,
                                          NV_NCURSES_MESSAGE_NO_COLOR);

    /* 在消息区域中打印格式化后的文本 */
    nv_ncurses_format_print(d, d->message, 1, 1, d->message->w - 2,
                            d->message->h - (1 + num_extra_lines), t);

    /* 释放格式化后的文本行 */

    nv_ncurses_free_text_rows(t);

} /* nv_ncurses_do_message_region() */




/*
 * nv_ncurses_do_progress_bar_region() - 创建进度条区域并绘制初始内容。
 *
 * 参数：
 *   d - ncurses UI 私有数据结构
 *
 * 处理流程：
 *   1. 通过 do_message_region() 创建消息区域，显示进度条标题
 *      （预留3行额外空间：状态消息行、分隔线行、进度条行）
 *   2. 在标题下方绘制水平分隔线（'_' 字符）
 *   3. 在分隔线下方绘制空的进度条（0% 状态）
 *
 * 区域布局（从上到下）：
 *   +-----------------------------+
 *   | 进度条标题文本              |  <- 文本区域（可能多行）
 *   |_____________________________|  <- 分隔线（h-4行）
 *   | 状态消息（当前操作描述）    |  <- 状态消息行（h-3行）
 *   | [------0%------]            |  <- 进度条（h-2行）
 *   +-----------------------------+
 */

static void nv_ncurses_do_progress_bar_region(DataStruct *d)
{
    int n, h, i, p[4];
    char v[4];

    /* 创建消息区域并显示进度条标题，预留3行（状态消息+分隔线+进度条） */

    nv_ncurses_do_message_region(d, NULL, d->progress_title, FALSE, 3);

    n = d->message->w - 2;   /* 进度条可用宽度 */
    h = d->message->h;

    wattrset(nv_stdscr, d->message->attr);

    /* 绘制水平分隔线 */

    for (i = 1; i <= n; i++) {
        mvwaddch(nv_stdscr, d->message->y + h - 4, d->message->x + i,
                NV_NCURSES_HLINE | d->message->attr);
    }

    /* 绘制初始的空进度条（显示 "0%"） */

    init_position(p, n + 2);       /* 计算百分比文本的居中位置 */
    init_percentage_string(v, 0);  /* 生成 "0%" 字符数组 */
    v[2] = '0';                    /* 确保显示 "0"（init_percentage_string 会将前导0置空） */

    if (d->use_color) {
        /* 有颜色模式：空的绿色底色进度条，中间显示 "0%" */
        for (i = 1; i <= n; i++) {
            mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + i,
                    choose_char(i, p, v, ' ') | NV_NCURSES_INPUT_COLOR);
        }
    } else {
        /* 无颜色模式：用 '[' ']' 做边框，'-' 填充，中间显示 "0%" */
        mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + 1, '[');
        for (i = 2; i < n; i++)
            mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + i,
                    choose_char(i, p, v, '-'));
        mvwaddch(nv_stdscr, d->message->y + h - 2, d->message->x + n, ']');
    }

} /* nv_ncurses_do_progress_bar_region() */



/*
 * nv_ncurses_do_progress_bar_message() - 在进度条区域的指定行写入状态消息。
 *
 * 参数：
 *   d   - ncurses UI 私有数据结构
 *   str - 要显示的状态消息（如 "Installing: libcuda.so.535.129.03"）
 *   y   - 消息行在消息区域内的 y 偏移
 *   w   - 消息的最大显示宽度（超出部分会被截断）
 *
 * 处理流程：
 *   1. 先用空格行清空消息行
 *   2. 如果 str 非空，截断到最大宽度后输出
 */

static void nv_ncurses_do_progress_bar_message(DataStruct *d, const char *str,
                                               int y, int w)
{
    char *tmp;

    /* 用空格清空消息行 */

    wattrset(nv_stdscr, d->message->attr);
    mvwaddstr(nv_stdscr, d->message->y + y, d->message->x, d->message->line);

    /* 输出消息文本（如有必要截断到 w 字符） */

    if (str) {
        tmp = malloc(w + 1);
        strncpy(tmp, str, w);
        tmp[w] = '\0';
        mvwaddstr(nv_stdscr, d->message->y + y, d->message->x + 1, tmp);
        free(tmp);
    }

} /* nv_ncurses_do_progress_bar_message() */




/***************************************************************************/
/*
 * ===== 分页浏览器函数 =====
 *
 * 这些函数实现了类似 `less` 的文本浏览器功能，
 * 用于在有限的屏幕空间内查看较长的文本内容。
 */


/*
 * nv_ncurses_create_pager() - 创建分页浏览器。
 *
 * 参数：
 *   d     - ncurses UI 私有数据结构
 *   x, y  - 浏览器在 stdscr 上的起始位置
 *   w, h  - 浏览器的宽度和高度
 *   t     - 要显示的文本行数据
 *   label - 页脚左侧显示的标签（如 "Proposed CommandList"）
 *   cur   - 初始滚动位置（首次显示时从第 cur 行开始）
 *
 * 返回值：
 *   新创建的 PagerStruct 指针
 *
 * 处理流程：
 *   1. 分配 PagerStruct 并初始化各字段
 *   2. 创建一个用于显示文本的屏幕区域
 *   3. 计算每页行数（page = 区域高度 - 2）
 *   4. 调用 pager_update 进行首次绘制
 */

static PagerStruct *nv_ncurses_create_pager(DataStruct *d,
                                            int x, int y, int w, int h,
                                            TextRows *t, const char *label,
                                            int cur)
{
    PagerStruct *p = (PagerStruct *) malloc(sizeof(PagerStruct));

    p->t = t;
    p->region = nv_ncurses_create_region(d, x, y, w, h, A_NORMAL, A_NORMAL);
    p->label = label;

    p->cur = cur;           /* 设置初始滚动位置 */

    p->page = h - 2;       /* 每页行数（预留上下各1行边距） */

    nv_ncurses_pager_update(d, p);  /* 执行首次绘制 */

    return p;

} /* nv_ncurses_create_pager() */




/*
 * nv_ncurses_destroy_pager() - 销毁分页浏览器并释放相关资源。
 *
 * 参数：
 *   p - 要销毁的 PagerStruct 指针
 *
 * 注意：不释放 p->t（TextRows），因为 TextRows 由调用方管理。
 */

static void nv_ncurses_destroy_pager(PagerStruct *p)
{
    nv_ncurses_destroy_region(p->region);  /* 销毁显示区域 */
    free(p);

} /* nv_ncurses_destroy_pager () */



/*
 * nv_ncurses_pager_update() - 重绘浏览器中的文本并更新页脚状态信息。
 *
 * 参数：
 *   d - ncurses UI 私有数据结构
 *   p - 分页浏览器结构
 *
 * 处理流程：
 *   1. 根据当前滚动位置 p->cur 计算可见文本范围
 *   2. 在区域中逐行绘制可见的文本（先用空格清行再写文本）
 *   3. 计算当前滚动百分比
 *   4. 在页脚右侧显示位置指示：
 *      - "All" 表示文本全部可见（无需滚动）
 *      - "Top" 表示在顶部
 *      - "Bot" 表示在底部
 *      - 百分比数字表示中间位置
 *
 * 注意：此函数不调用 wrefresh()。
 */

static void nv_ncurses_pager_update(DataStruct *d, PagerStruct *p)
{
    int i, maxy, percent, denom;
    char tmp[10];

    if (!p) return;

    /* 计算可见文本的最大行号 */

    maxy = (p->cur + (p->region->h - 1));
    if (maxy > p->t->n) maxy = p->t->n;

    /* 逐行绘制可见的文本 */

    wattrset(nv_stdscr, p->region->attr);

    for (i = p->cur; i < maxy; i++) {
        /* 先用空格清空该行 */
        mvwaddstr(nv_stdscr, p->region->y + i - p->cur, p->region->x,
                  p->region->line);
        /* 再写入文本内容 */
        if (p->t->t[i]) {
            mvwaddstr(nv_stdscr, p->region->y + i - p->cur, p->region->x,
                      p->t->t[i]);
        }
    }

    /* 计算当前滚动百分比 */

    denom = p->t->n - (p->region->h - 1);  /* 可滚动的最大行数 */
    if (denom < 1) percent = 100;
    else percent = ((100.0 * (float) (p->cur)) / (float) denom);

    /* 生成位置指示字符串 */

    if (p->t->n <= (p->region->h - 1)) snprintf(tmp, 10, "All");    /* 全部可见 */
    else if (percent <= 0)          snprintf(tmp, 10, "Top");        /* 在顶部 */
    else if (percent >= 100)        snprintf(tmp, 10, "Bot");        /* 在底部 */
    else                            snprintf(tmp, 10, "%3d%%", percent); /* 中间位置 */

    /* 更新页脚显示 */

    nv_ncurses_set_footer(d, p->label, tmp);

} /* nv_ncurses_pager_update() */



/*
 * nv_ncurses_pager_handle_events() - 处理影响分页浏览器的按键事件。
 *
 * 参数：
 *   d  - ncurses UI 私有数据结构
 *   p  - 分页浏览器结构
 *   ch - 用户按下的键值
 *
 * 支持的按键：
 *   KEY_UP    - 向上滚动1行
 *   KEY_DOWN  - 向下滚动1行
 *   KEY_PPAGE - 向上翻一页（PageUp）
 *   KEY_NPAGE - 向下翻一页（PageDown）
 *
 * 每次滚动后自动调用 pager_update 重绘文本和页脚状态。
 */

static void nv_ncurses_pager_handle_events(DataStruct *d,
                                           PagerStruct *p, int ch)
{
    int n;

    if (!p) return;
    n = p->t->n - (p->region->h - 1);  /* 可滚动的最大位置 */

    switch (ch) {
    case KEY_UP:
        /* 上箭头：向上滚动1行 */
        if (p->cur > 0) {
            p->cur--;
            nv_ncurses_pager_update(d, p);
            wrefresh(nv_stdscr);
        }
        break;

    case KEY_DOWN:
        /* 下箭头：向下滚动1行 */
        if (p->cur < n) {
            p->cur++;
            nv_ncurses_pager_update(d, p);
            wrefresh(nv_stdscr);
        }
        break;

    case KEY_PPAGE:
        /* PageUp：向上翻一页 */
        if (p->cur > 0) {
            p->cur -= p->page;
            if (p->cur < 0) p->cur = 0;  /* 不超过顶部 */
            nv_ncurses_pager_update(d, p);
            wrefresh(nv_stdscr);
        }
        break;

    case KEY_NPAGE:
        /* PageDown：向下翻一页 */
        if (p->cur < n) {
            p->cur += p->page;
            if (p->cur > n) p->cur = n;  /* 不超过底部 */
            nv_ncurses_pager_update(d, p);
            wrefresh(nv_stdscr);
        }
        break;
    }
} /* nv_ncurses_pager_handle_events() */

/*
 * draw_buttons() - 绘制一组按钮，高亮显示当前选中的按钮。
 *
 * 参数：
 *   d           - ncurses UI 私有数据结构
 *   buttons     - 按钮标签字符串数组
 *   num_buttons - 按钮总数
 *   button      - 当前选中的按钮索引（高亮显示）
 *   button_w    - 每个按钮的统一宽度
 *   buttons_x   - 每个按钮的 x 坐标数组
 *   button_y    - 所有按钮的 y 坐标（按钮在同一行）
 */

static void draw_buttons(DataStruct *d, const char * const *buttons,
                         int num_buttons, int button, int button_w,
                         const int *buttons_x, int button_y)
{
    int i;

    for (i = 0; i < num_buttons; i++) {
        nv_ncurses_draw_button(d, d->message, buttons_x[i], button_y,
                               button_w, 1, buttons[i], button == i, FALSE);
    }
}

/*
 * nv_ncurses_paged_prompt() - 带可滚动文本区域的多选提示对话框。
 *
 * 这是 ncurses UI 中最复杂的对话框函数，组合了：
 *   - 问题文本区域（显示在顶部或中间）
 *   - 多个可选按钮（Tab/左右箭头切换选择）
 *   - 可选的可滚动文本浏览区域（上下箭头/PageUp/PageDown 滚动）
 *
 * 参数：
 *   op             - 安装器选项结构
 *   question       - 问题文本
 *   pager_title    - 分页文本区域的标题（可以为 NULL）
 *   pager_text     - 分页文本内容（可以为 NULL）
 *   buttons        - 按钮标签字符串数组
 *   num_buttons    - 按钮数量
 *   default_button - 默认选中按钮的索引
 *
 * 返回值：
 *   用户选择的按钮索引
 *
 * 当 pager_title 和 pager_text 均为 NULL 时，退化为普通的多选对话框。
 * 当两者均非 NULL 时，问题区域在顶部，分页文本区域在下方占据剩余空间。
 *
 * 支持的按键：
 *   Tab / 右箭头 - 切换到下一个按钮
 *   左箭头       - 切换到上一个按钮
 *   上下箭头     - 在分页文本中滚动
 *   PageUp/Down  - 在分页文本中翻页
 *   Ctrl+L       - 强制重绘
 *   Enter        - 确认当前选择
 */

static int nv_ncurses_paged_prompt(Options *op, const char *question,
                                   const char *pager_title,
                                   const char *pager_text,
                                   const char * const *buttons,
                                   int num_buttons, int default_button)
{
    DataStruct *d = (DataStruct *) op->ui.priv;
    TextRows *t_pager = NULL;     /* 分页文本的格式化行数据 */
    int ch, cur = 0;              /* cur: 分页浏览器的滚动位置（用于重绘后恢复） */
    int i, button_w = 0, button_y, button = default_button;
    int buttons_x[num_buttons];   /* 每个按钮的 x 坐标 */
    PagerStruct *p = NULL;        /* 分页浏览器实例 */

    /* 检查窗口是否发生了大小变化 */

    nv_ncurses_check_resize(d, FALSE);

    /* 计算所有按钮的统一宽度（取最长标签 + 左右各2字符边距） */
    for (i = 0; i < num_buttons; i++) {
        int len = strlen(buttons[i]);
        if (len > button_w) {
            button_w = len;
        }
    }
    button_w += 4;

  print_message:
    /* 绘制/重绘入口点（窗口大小变化时跳回此处） */

    /* 销毁已有的消息区域、分页浏览器和文本数据 */

    if (d->message) {

        /*
         * 进入此函数时可能已有消息区域（例如进度条显示期间遇到错误）。
         * 先销毁已有区域，status_update/status_end 会在需要时自行重建。
         */

        nv_ncurses_destroy_region(d->message);
        d->message = NULL;
    }

    if (p) {
        nv_ncurses_destroy_pager(p);
        p = NULL;
    }
    if (t_pager) {
        nv_ncurses_free_text_rows(t_pager);
        t_pager = NULL;
    }

    /* 创建消息区域并显示问题文本 */
    /* 如果有分页文本，问题区域放在顶部（top=TRUE）；否则放在屏幕1/3处 */

    nv_ncurses_do_message_region(d, NULL, question,
                                 (pager_title && pager_text), 2);

    /* 按钮放在消息区域底部倒数第二行 */
    button_y = d->message->h - 2;

    /* 计算每个按钮的水平位置（均匀分布） */
    for (i = 0; i < num_buttons; i++) {
        buttons_x[i] = (i + 1) * (d->message->w / (num_buttons + 1)) -
                       button_w / 2;
    }

    /* 绘制所有按钮 */
    draw_buttons(d, buttons, num_buttons, button, button_w, buttons_x,
                 button_y);

    /* 如果提供了分页文本，创建分页浏览器 */

    if (pager_title && pager_text) {
        /* 格式化分页文本 */
        t_pager = d->format_text_rows(NULL, pager_text, d->message->w, TRUE);
        /* 创建分页浏览器，位于消息区域下方，占据剩余屏幕空间 */
        p = nv_ncurses_create_pager(d, 1, d->message->h + 2, d->message->w,
                                    d->height - d->message->h - 4, t_pager,
                                    pager_title, cur);
    }

    wrefresh(nv_stdscr);

    /* ===== 主事件处理循环 ===== */

    do {
        /* 如果发生了窗口大小变化，保存滚动位置后重绘 */
            if (nv_ncurses_check_resize(d, FALSE)) {
                if (p) {
                    cur = p->cur;  /* 保存当前滚动位置 */
                }
                goto print_message;
        }

        ch = wgetch(nv_stdscr);

        switch (ch) {
            case NV_NCURSES_TAB:
            case KEY_RIGHT:
                /* Tab 或右箭头：切换到下一个按钮（循环） */
                button = (button + 1) % num_buttons;
                draw_buttons(d, buttons, num_buttons, button, button_w,
                             buttons_x, button_y);
                wrefresh(nv_stdscr);
                break;

            case KEY_LEFT:
                /* 左箭头：切换到上一个按钮（循环） */
                button = (button + num_buttons - 1) % num_buttons;
                draw_buttons(d, buttons, num_buttons, button, button_w,
                             buttons_x, button_y);
                wrefresh(nv_stdscr);
                break;

            case NV_NCURSES_CTRL('L'):
                /* Ctrl+L：强制重绘 */
                nv_ncurses_check_resize(d, TRUE);
                if (p) {
                    cur = p->cur;
                }
                goto print_message;
                break;

            default:
                break;
        }

        /* 将按键事件传递给分页浏览器处理（上下箭头、翻页等） */
        if (p) {
            nv_ncurses_pager_handle_events(d, p, ch);
        }
    } while (ch != NV_NCURSES_ENTER);  /* Enter 键确认选择 */

    /* 播放选中按钮的按下动画 */

    nv_ncurses_erase_button(d->message, buttons_x[button], button_y,
                            button_w, 1);
    nv_ncurses_draw_button(d, d->message, buttons_x[button], button_y,
                           button_w, 1, buttons[button], TRUE, TRUE);  /* 按下 */
    wrefresh(nv_stdscr);
    usleep(NV_NCURSES_BUTTON_PRESS_TIME);

    nv_ncurses_erase_button(d->message, buttons_x[button], button_y,
                            button_w, 1);
    nv_ncurses_draw_button(d, d->message, buttons_x[button], button_y,
                           button_w, 1, buttons[button], TRUE, FALSE); /* 恢复 */
    wrefresh(nv_stdscr);
    usleep(NV_NCURSES_BUTTON_PRESS_TIME);

    /* 恢复页脚为默认内容 */

    nv_ncurses_set_footer(d, NV_NCURSES_DEFAULT_FOOTER_LEFT,
                          NV_NCURSES_DEFAULT_FOOTER_RIGHT);

    /* 清理资源 */

    if (t_pager) {
        nv_ncurses_free_text_rows(t_pager);
    }
    if (p) {
        nv_ncurses_destroy_pager(p);
    }
    nv_ncurses_destroy_region(d->message);
    d->message = NULL;

    wrefresh(nv_stdscr);

    return button;  /* 返回用户选择的按钮索引 */
}





/*****************************************************************************/
/*
 * ===== 进度条辅助函数 =====
 *
 * 这些函数用于在进度条中居中显示百分比数字（如 "42%"）。
 * 百分比字符串最多4个字符（如 "100%"），在进度条宽度中居中放置。
 */

/*
 * choose_char() - 判断进度条中位置 i 是否是百分比字符的显示位置。
 *
 * 参数：
 *   i   - 进度条中的当前位置索引
 *   p   - 百分比字符串中4个字符的位置数组
 *   v   - 百分比字符串的4个字符（如 '4','2','%','\0'）
 *   def - 如果当前位置不是百分比字符位置，返回此默认字符
 *
 * 返回值：
 *   如果位置 i 对应百分比字符串的某个字符，返回该字符（若为0则返回def）；
 *   否则返回默认字符 def。
 */

static int choose_char(int i, int p[4], char v[4], char def)
{
    if (p[0] == i) return v[0] ? v[0] : def;
    if (p[1] == i) return v[1] ? v[1] : def;
    if (p[2] == i) return v[2] ? v[2] : def;
    if (p[3] == i) return v[3] ? v[3] : def;
    return def;
}

/*
 * init_percentage_string() - 将百分比数值转换为字符数组。
 *
 * 参数：
 *   v - 输出的4字符数组（百位、十位、个位、'%'）
 *   n - 百分比值（1-100）
 *
 * 例如 n=42 时，v = {'4', '2', '%', '\0'（实际是'%'的位置为v[3]）}
 * 前导零会被置为 0（不显示），如 n=5 时 v = {0, 0, '5', '%'}
 */

static void init_percentage_string(char v[4], int n)
{
    int j;

    n = NV_MAX(n, 1);  /* 至少为1，避免显示空字符串 */

    /* 分解为百位、十位、个位数字 */
    v[0] = (n/100);
    v[1] = (n/10) - (v[0]*10);
    v[2] = (n - (v[0]*100) - (v[1]*10));
    /* 转换为 ASCII 字符 */
    v[0] += '0';
    v[1] += '0';
    v[2] += '0';
    v[3] = '%';

    /* 将前导 '0' 字符置为 0（不显示），保留有效数字 */
    for (j = 0; j < 3; j++) {
        if (v[j] == '0') v[j] = 0;
        else break;
    }
}

/*
 * init_position() - 计算百分比字符串在进度条中的居中位置。
 *
 * 参数：
 *   p - 输出的4元素数组，存放4个字符在进度条中的 x 坐标
 *   w - 进度条的总宽度
 *
 * 例如 w=40 时，4个字符分别在位置 18, 19, 20, 21（居中）
 */

static void init_position(int p[4], int w)
{
    p[0] = 0 + (w - 4)/2;
    p[1] = 1 + (w - 4)/2;
    p[2] = 2 + (w - 4)/2;
    p[3] = 3 + (w - 4)/2;
}



/*****************************************************************************/
/*
 * ===== 其他辅助函数 =====
 */


/*
 * nv_ncurses_create_command_list_text() - 将命令列表转换为可显示的文本字符串。
 *
 * 参数：
 *   d  - ncurses UI 私有数据结构（此处未直接使用）
 *   cl - 命令列表结构（包含所有待执行命令的描述）
 *
 * 返回值：
 *   包含所有命令描述的字符串，每条描述占一行（以换行符分隔）。
 *   调用方负责释放返回的字符串。
 *
 * 处理流程：
 *   遍历命令列表中的每条描述，逐条追加到结果字符串末尾，
 *   每条描述后加换行符。使用手动内存管理进行字符串拼接。
 */

static char *nv_ncurses_create_command_list_text(DataStruct *d, CommandList *cl)
{
    char *ret = strdup("");  /* 从空字符串开始 */
    int i;

    for (i = 0; i < cl->num; i++) {
        const char *str = cl->descriptions[i];

        if (str) {
            int lenret, lenstr;
            char *tmp;

            lenret = strlen(ret);
            lenstr = strlen(str);
            /* 分配新缓冲区：旧内容 + 新描述 + 换行符 + 空终止符 */
            tmp = malloc(lenret + lenstr + 2 /* "\n\0" */);

            tmp[0] = 0;

            /* 拼接旧内容和新描述 */
            memcpy(tmp, ret, lenret);
            memcpy(tmp + lenret, str, lenstr);

            tmp[lenret + lenstr] = '\n';       /* 添加换行符 */
            tmp[lenret + lenstr + 1] = 0;      /* 空终止符 */

            free(ret);
            ret = tmp;
        }
    }

    return ret;
}



/*
 * nv_ncurses_free_text_rows() - 释放 TextRows 数据结构。
 *
 * 参数：
 *   t - 要释放的 TextRows 结构（可以为 NULL）
 *
 * 处理流程：
 *   1. 释放每一行的字符串
 *   2. 释放行指针数组
 *   3. 释放 TextRows 结构本身
 *
 * TextRows 由 format_text_rows() 函数分配，本函数执行对应的释放。
 */

static void nv_ncurses_free_text_rows(TextRows *t)
{
    int i;

    if (!t) return;
    for (i = 0; i < t->n; i++) free(t->t[i]);  /* 释放每行文本 */
    if (t->t) free(t->t);                       /* 释放行指针数组 */
    free(t);                                     /* 释放结构本身 */

} /* nv_ncurses_free_text_rows() */



/*
 * nv_ncurses_format_print() - 在指定区域的指定位置输出格式化后的文本行。
 *
 * 参数：
 *   d      - ncurses UI 私有数据结构
 *   region - 输出目标区域
 *   x, y   - 文本在区域内的起始位置（相对坐标）
 *   w      - 文本区域宽度（此参数当前未直接使用，由 TextRows 自身控制宽度）
 *   h      - 文本区域最大高度（限制输出行数）
 *   t      - 已格式化的文本行数据
 *
 * 返回值：
 *   实际打印的行数
 *
 * 处理流程：
 *   将 TextRows 中的每一行输出到区域内（从 (x,y) 开始），
 *   最多输出 h 行。
 */

static int nv_ncurses_format_print(DataStruct *d, RegionStruct *region,
                                   int x, int y, int w, int h,
                                   TextRows *t)
{
    int i, n;

    n = NV_MIN(t->n, h);  /* 实际输出行数 = min(文本行数, 最大高度) */

    wattrset(nv_stdscr, region->attr);

    for (i = 0; i < n; i++) {
        mvwaddstr(nv_stdscr, region->y + y + i, region->x + x, t->t[i]);
    }

    wattrset(nv_stdscr, A_NORMAL);
    return n;

} /* nv_ncurses_format_print() */



/*
 * nv_ncurses_check_resize() - 检查终端窗口是否发生了大小变化。
 *
 * 参数：
 *   d     - ncurses UI 私有数据结构
 *   force - 是否强制重绘（TRUE 时即使尺寸未变也执行重绘）
 *
 * 返回值：
 *   TRUE  - 检测到尺寸变化（或 force=TRUE），已完成重绘
 *   FALSE - 未检测到尺寸变化
 *
 * 处理流程（当尺寸变化或 force=TRUE 时）：
 *   1. 销毁旧的页眉和页脚区域
 *   2. 清空整个屏幕
 *   3. 更新缓存的终端尺寸
 *   4. 根据新尺寸重新创建页眉和页脚区域
 *   5. 恢复页眉和页脚的文本内容
 *
 * 设计说明：
 *   本函数采用轮询方式检测窗口大小变化，而非捕获 SIGWINCH 信号。
 *   这是因为 SIGWINCH 是异步的，在信号处理器中操作 ncurses 不够安全。
 *   轮询方式虽然有轻微延迟，但能确保在安全的时机处理窗口变化。
 *
 * 注意：此函数只重建页眉和页脚，不重建消息区域。
 *       消息区域的重建由各对话框函数自行处理（通过 goto 跳回重绘入口）。
 */

static int nv_ncurses_check_resize(DataStruct *d, bool force)
{
    int x, y;

    getmaxyx(nv_stdscr, y, x);  /* 查询当前终端尺寸 */

    if (!force) {
        if ((x == d->width) && (y == d->height)) {
            /* 尺寸未变，直接返回 */
            return FALSE;
        }
    }

    /* 检测到窗口大小变化（或强制重绘） */

    /* 销毁旧的页眉和页脚区域 */

    nv_ncurses_destroy_region(d->header);
    nv_ncurses_destroy_region(d->footer);

    wclear(nv_stdscr);  /* 清空整个屏幕 */

    /* 更新缓存的终端尺寸 */

    d->height = y;
    d->width = x;

    /* 根据新尺寸重新创建页眉和页脚 */

    d->header = nv_ncurses_create_region(d, 1, 0, d->width - 2, 1,
                                         NV_NCURSES_HEADER_COLOR,
                                         NV_NCURSES_HEADER_NO_COLOR);

    d->footer = nv_ncurses_create_region(d, 1, d->height - 2, d->width - 2, 1,
                                         NV_NCURSES_FOOTER_COLOR,
                                         NV_NCURSES_FOOTER_NO_COLOR);

    /* 恢复页眉和页脚的文本内容 */
    nv_ncurses_set_header(d, d->title);
    nv_ncurses_set_footer(d, d->footer_left, d->footer_right);

    return TRUE;

} /* nv_ncurses_check_resize() */
