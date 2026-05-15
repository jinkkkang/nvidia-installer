/*
 * gcc mkprecompiled.c -o mkprecompiled -Wall -g
 *
 * mkprecompiled - this program packages up a precompiled kernel
 * module interface with a list of unresolved symbols in the kernel
 * module.
 *
 * normally, this would be done much more simply with a perl or shell
 * script, but I've implemented it in C because we don't want the
 * installer to rely upon any system utilities that it doesn't
 * absolutely need.
 *
 * commandline options:
 *
 * -i, --interface=<filename>
 * -o, --output=<filename>
 * -u, --unpack=<filename>
 * -d, --description=<kernel description>
 *
 * There is nothing specific to the NVIDIA graphics driver in this
 * program, so it should be usable for the nforce drivers, for
 * example.
 *
 * 【文件说明】预编译内核接口包管理工具（独立可执行程序）。
 *
 * 本程序用于创建、解包、查看和匹配预编译内核接口包（precompiled package）。
 * 预编译内核接口包的目的是加快 NVIDIA 驱动在特定内核版本上的安装速度——
 * 如果目标系统的内核版本与包中记录的版本匹配，就无需重新编译内核模块，
 * 直接使用预编译好的接口文件即可。
 *
 * 包文件中可以包含两类文件：
 *   1. 内核接口文件（kernel interface）：预编译的内核接口对象文件，
 *      需要与核心对象文件（core object）链接才能生成最终的内核模块（.ko）
 *   2. 内核模块文件（kernel module）：已经完全链接的内核模块文件
 *
 * 每个包还记录了元信息：驱动版本、内核版本（/proc/version）、描述等。
 *
 * 四个操作模式：
 *   --pack   (-p)  : 将文件打包到预编译包中
 *   --unpack (-u)  : 从包中解出文件
 *   --info   (-i)  : 显示包的元信息和文件列表
 *   --match  (-m)  : 检查包是否匹配当前运行的内核
 *
 * 注意：本程序使用 C 语言实现而非脚本，是为了减少安装器对外部工具的依赖。
 * 程序本身不包含 NVIDIA 图形驱动特有的逻辑，理论上也可用于其他驱动（如 nForce）。
 */

#define BINNAME "mkprecompiled"       /* 程序名称，用于帮助信息和错误消息 */
#define NV_LINE_LEN 256               /* 通用行缓冲区长度 */
#define NV_VERSION_LEN 4096           /* 版本字符串最大长度 */
#define PROC_MOUNT_POINT "/proc"      /* 默认的 procfs 挂载点 */

#include <stdio.h>       /* 标准输入输出 */
#include <string.h>      /* 字符串操作 */
#include <stdlib.h>      /* 标准库函数：malloc, exit 等 */
#include <unistd.h>      /* POSIX 标准函数 */
#include <errno.h>       /* 错误码定义 */
#include <sys/types.h>   /* 系统数据类型 */
#include <sys/stat.h>    /* 文件状态信息（stat） */
#include <fcntl.h>       /* 文件控制 */
#include <sys/mman.h>    /* 内存映射（mmap） */
#include <ctype.h>       /* 字符分类函数（toupper 等） */
#include <stdarg.h>      /* 可变参数支持（va_list 等） */
#include <inttypes.h>    /* 跨平台整数格式化宏（PRIu32 等） */

#include <nvgetopt.h>    /* NVIDIA 自定义的命令行参数解析库 */

typedef unsigned int uint32;    /* 32 位无符号整数类型别名 */
typedef unsigned char uint8;    /* 8 位无符号整数类型别名 */

/*
 * 操作类型枚举。值被设为对应命令行短选项的字符，便于 switch-case 处理。
 */
enum {
    PACK = 'p',      /* 打包：将文件添加到预编译包 */
    UNPACK = 'u',    /* 解包：从预编译包中提取文件 */
    INFO = 'i',      /* 信息：显示预编译包的详细信息 */
    MATCH = 'm',     /* 匹配：检查包是否与当前内核匹配 */
};

/*
 * Options - 命令行选项结构体。
 *
 * 存储从命令行解析出的所有选项和操作参数。
 * 注意：此处的 Options 结构体是 mkprecompiled 独有的定义，
 * 与 nvidia-installer 主程序中的 Options 不同。
 */

typedef struct {
    int action;                              /* 当前操作类型（PACK/UNPACK/INFO/MATCH） */
    char *package_file;                      /* 预编译包文件路径 */
    char *output_directory;                  /* 解包输出目录（仅用于 UNPACK） */
    char *description;                       /* 包描述信息（人类可读） */
    char *proc_version_string;               /* 目标内核的 /proc/version 内容 */
    char *proc_mount_point;                  /* procfs 挂载点路径 */
    char *version;                           /* 驱动版本号 */
    int num_files;                           /* 要打包的文件数量（-1 表示尚未开始计数） */
    struct __precompiled_file_info *new_files; /* 要打包的文件信息数组 */
    struct __precompiled_info *package;        /* 已加载的包信息（从现有包文件读取） */
} Options;


#include "common-utils.h"   /* 通用工具函数（nvalloc, nvstrdup 等） */
#include "crc.h"            /* CRC 校验计算 */
#include "precompiled.h"    /* 预编译包格式的读写函数 */


/*
 * 以下函数是为了解决链接依赖而提供的桩实现（stub）。
 *
 * crc.c 和 precompiled.c 中调用了 ui_warn/ui_error/ui_expert/ui_log 等
 * UI 函数（它们在 nvidia-installer 主程序中有完整实现），但 mkprecompiled
 * 是一个独立程序，不链接 nvidia-installer 的 UI 模块。因此这里提供简单的
 * 替代实现：警告和错误输出到 stderr，专家信息和日志则静默忽略。
 */

void ui_warn(Options *op, const char *fmt, ...);

void ui_warn(Options *op, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void ui_expert(Options *op, const char *fmt, ...);

/* ui_expert 的桩实现：静默忽略（独立工具不需要专家级输出） */
void ui_expert(Options *op, const char *fmt, ...)
{
}

void ui_error(Options *op, const char *fmt, ...);

void ui_error(Options *op, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void ui_log(Options *op, const char *fmt, ...);

/* ui_log 的桩实现：静默忽略（独立工具不需要日志记录） */
void ui_log(Options *op, const char *fmt, ...)
{
}




/*
 * print_help() - 打印完整的使用帮助信息到标准输出。
 *
 * 包括所有操作模式和选项的详细说明。
 */

static void print_help(void)
{
    printf("\n%s: pack/unpack precompiled files, and get information about\n"
           "existing precompiled file packages.\n\n"
           "USAGE: <action> <package-file> [options] \n\n", BINNAME);

    printf("<action> may be one of:\n\n"
           "    -p | --pack     add files to a package\n"
           "    -u | --unpack   unpack files from a package\n"
           "    -i | --info     display information about a package\n"
           "    -m | --match    check if a package matches the running kernel\n"
           "    -h | --help     print this help text and exit\n\n"
           "<package-file> is the package file to pack/unpack/test. It must be\n"
           "an existing, valid package file for the --unpack, --info, and\n"
           "--match actions. For the --pack action, if <package-file> does not\n"
           "exist, it is created; if it exists but is not a valid package file,\n"
           "it is overwritten; and if it exists and is a valid package file,\n"
           "files will be added to the existing package.\n\n"
           "--pack options:\n"
           "    -v | --driver-version      (REQUIRED for new packages)\n"
           "        The version of the packaged components.\n"
           "    -P | --proc-version-string (RECOMMENDED for new packages)\n"
           "        The kernel version, as reported by '/proc/version', for the\n"
           "        target kernel. Default: the contents of the '/proc/version'\n"
           "        file on the current system.\n"
           "    -d | --description         (RECOMMENDED for new packages)\n"
           "        A human readable description of the package.\n"
           "    --kernel-interface <file> --linked-module-name <module-name>\\\n"
           "                              --core-object-name <core-name>\\\n"
           "                            [ --linked-module <linked-kmod-file> \\\n"
           "                              --signed-module <signed-kmod-file> ]\\\n"
           "                            [ --target-directory <target-directory> ]\n"
           "        Pack <file> as a precompiled kernel interface.\n"
           "        <module-name> specifies the name of the kernel module file\n"
           "        that is produced by linking the precompiled kernel interface\n"
           "        with a separate precompiled core object file. <core-name>\n"
           "        specifies the name of the core object file that is linked\n"
           "        together with the precompiled interface to produce the final\n"
           "        kernel module.\n"
           "        A detached module signature may be produced by specifying\n"
           "        both the --linked-module and --signed-module options.\n"
           "        <linked-kmod-file> is a linked .ko file that is the result\n"
           "        of linking the precompiled interface with the remaining\n"
           "        object file(s) required to produce the finished module, and\n"
           "        <signed-kmod-file> is a copy of <linked-kmod-file> which an\n"
           "        appended module signature. In order for the signature to be\n"
           "        correctly applied on the target system, the linking should\n"
           "        be performed with the same linker and flags that will be\n"
           "        used on the target system.\n"
           "        A target directory for unpacking the interface may be\n"
           "        specified with the --target-directory option.\n"
           "        <target-directory> is the name of the directory where the\n"
           "        unpacked interface will be written.\n"
           "        The --linked-module and --signed-module options must be\n"
           "        given after the --kernel-interface option for the kernel\n"
           "        interface file with which they are associated, and before\n"
           "        any additional --kernel-interface or --kernel-module files.\n"
           "    --kernel-module <file> [ --signed ]\\\n"
           "                           [ --target-directory <target-directory> ]\n"
           "        Pack <file> as a precompiled kernel module. The --signed\n"
           "        option specifies that <file> includes a module signature.\n"
           "        The --signed option must be given after the --kernel-module\n"
           "        option for the kernel module with which it is associated,\n"
           "        and before any additional --kernel-interface or\n"
           "        --kernel-module files.\n\n"
           "    If --driver-version, --proc-version-string, or --description\n"
           "    are given with an existing package file, the values in that\n"
           "    package file will be updated with new ones. At least one file\n"
           "    must be given with either --kernel-interface or --kernel-module\n"
           "    when using the --pack option.\n\n"
           "--unpack options:\n"
           "    -o | --output-directory\n"
           "        The target directory where files will be unpacked. Default:\n"
           "        unpack files in the current directory.\n\n"
           "Additional options:\n"
           "    --proc-mount-point\n"
           "        The procfs mount point on the current system, where the \n"
           "        '/proc/version' file may be found. Used by the --match\n"
           "        action, as well as to supply the default value of the\n"
           "        --proc-version-string option of the --pack action.\n"
           "        Default value: '/proc'\n");

} /* print_help() */


/*
 * 长选项值枚举。
 * 从 1024 开始编号，以避免与短选项字符（ASCII 可打印字符，< 128）冲突。
 */
enum {
    PROC_MOUNT_POINT_OPTION = 1024,       /* --proc-mount-point */
    KERNEL_INTERFACE_OPTION,              /* --kernel-interface：指定内核接口文件 */
    KERNEL_MODULE_OPTION,                 /* --kernel-module：指定内核模块文件 */
    SIGNED_FILE_OPTION,                   /* --signed：标记文件包含嵌入式签名 */
    LINKED_MODULE_OPTION,                 /* --linked-module：链接后的模块文件 */
    LINKED_AND_SIGNED_MODULE_OPTION,      /* --signed-module：签名后的模块文件 */
    LINKED_MODULE_NAME_OPTION,            /* --linked-module-name：链接产物名称 */
    CORE_OBJECT_NAME_OPTION,              /* --core-object-name：核心对象文件名称 */
    TARGET_DIRECTORY_OPTION               /* --target-directory：解包目标目录 */
};


/*
 * grow_file_array() - 按需扩展文件信息数组。
 *
 * 当待打包的文件数量达到数组容量上限时，将数组大小翻倍。
 * 首次调用时将 num_files 从 -1（初始值）设为 0。
 *
 * 参数：
 *   op         - 选项结构体（包含 num_files 和 new_files 数组）
 *   array_size - 指向当前数组容量的指针（会被就地修改）
 */
static void grow_file_array(Options *op, int *array_size)
{
    if (op->num_files < 0) {
        op->num_files = 0;
    }

    if (op->num_files >= *array_size) {
        *array_size *= 2;
        op->new_files = nvrealloc(op->new_files,
                                  sizeof(PrecompiledFileInfo) * *array_size);
    }
}


/*
 * file_type_from_option() - 将命令行选项值转换为预编译文件类型。
 *
 * 参数：
 *   option - 命令行选项值（KERNEL_INTERFACE_OPTION 或 KERNEL_MODULE_OPTION）
 *
 * 返回值：对应的 PRECOMPILED_FILE_TYPE_* 常量。
 *         无法识别的选项会导致程序直接退出。
 */
static uint32 file_type_from_option(int option) {
    switch (option) {
        case KERNEL_INTERFACE_OPTION:
            return PRECOMPILED_FILE_TYPE_INTERFACE;
        case KERNEL_MODULE_OPTION:
            return PRECOMPILED_FILE_TYPE_MODULE;
        default:
            fprintf(stderr, "Unrecognized file type!");
            exit(1);
    }
}


/*
 * check_file_option_validity() - 检查文件附属选项的顺序有效性。
 *
 * 某些选项（如 --signed、--linked-module 等）必须在 --kernel-interface
 * 或 --kernel-module 之后指定。此函数确保在引用文件属性之前已经指定了文件。
 *
 * 参数：
 *   op          - 选项结构体
 *   option_name - 正在检查的选项名称（用于错误消息）
 */
static void check_file_option_validity(Options *op, const char *option_name)
{
    if (op->num_files < 0) {
        fprintf(stderr, "The --%s option cannot be specified before a file "
                "name.\n", option_name);
        exit(1);
    }
}


/*
 * create_detached_signature() - 从签名和未签名的模块文件中提取分离式签名。
 *
 * 分离式签名（detached signature）是通过比较签名前后的模块文件来提取的：
 *   - linked_module：链接后但未签名的 .ko 文件
 *   - signed_module：签名后的 .ko 文件（内容 = linked_module + 签名数据）
 *
 * 签名提取方法：取 signed_module 中超出 linked_module 大小的部分字节（尾部）。
 * 提取出的签名存储在 PrecompiledFileInfo 结构中，安装时可以重新附加到
 * 目标系统上链接生成的模块文件上。
 *
 * 此操作仅对内核接口文件有意义（对完整的内核模块文件，签名是嵌入式的）。
 *
 * 参数：
 *   op            - 选项结构体
 *   file          - 文件信息结构体（用于存储签名数据）
 *   linked_module - 未签名的链接模块文件路径（可为 NULL）
 *   signed_module - 已签名的链接模块文件路径（可为 NULL）
 *
 * 返回值：TRUE 表示成功（或不需要签名），FALSE 表示失败。
 */
static int create_detached_signature(Options *op, PrecompiledFileInfo *file,
                                     const char *linked_module,
                                     const char *signed_module)
{
    /* 分离式签名仅适用于内核接口文件 */
    if (file->type != PRECOMPILED_FILE_TYPE_INTERFACE) {
        return TRUE;
    } else if (linked_module && signed_module) {
        /* 两个文件都提供了，可以提取签名 */
        struct stat st;

        if (stat(linked_module, &st) != 0) {
            fprintf(stderr, "Unable to stat the linked kernel module file '%s'."
                    "\n", linked_module);
            return FALSE;
        }

        /* 计算未签名模块的 CRC，安装时用于验证链接结果的一致性 */
        file->linked_module_crc = compute_crc(op, linked_module);
        file->attributes |= PRECOMPILED_ATTR(LINKED_MODULE_CRC);

        /* 从签名模块中提取超出未签名模块大小的尾部字节（即签名） */
        file->signature_size = byte_tail(signed_module, st.st_size,
                                         &(file->signature));

        if (file->signature_size > 0 && file->signature != NULL) {
            file->attributes |= PRECOMPILED_ATTR(DETACHED_SIGNATURE);
            return TRUE;
        } else {
            fprintf(stderr, "Failed to create a detached signature from signed "
                    "kernel module '%s'.\n", signed_module);
        }

    } else if (linked_module || signed_module) {
        /* 只提供了一个文件，无法提取签名 */
        fprintf(stderr, "Both --linked-module and --signed-module must be "
                "specified to create a detached signature for precompiled "
                "kernel interface file '%s'.\n", file->name);
    } else {
        /* 两个文件都未提供，不需要签名，正常返回 */
        return TRUE;
    }

    return FALSE;
}


/*
 * set_action() - 设置操作类型，并检查是否存在重复的操作指定。
 *
 * 每次运行只能指定一个操作（pack/unpack/info/match），
 * 如果多次指定操作会输出错误信息并退出。
 *
 * 参数：
 *   op     - 选项结构体
 *   action - 要设置的操作类型
 */
static void set_action(Options *op, int action)
{
    if (op->action) {
        fprintf(stderr, "Invalid command line; multiple actions cannot be "
                "specified at the same time.\n");
        exit(1);
    }

    op->action = action;
}

/*
 * pack_a_file() - 处理一个待打包的文件，读取其内容并填充 PrecompiledFileInfo。
 *
 * 根据文件类型（内核接口 / 内核模块）调用不同的读取函数，
 * 然后提取分离式签名（如果提供了签名相关文件），
 * 最后清理相关的临时选项值，为处理下一个文件做准备。
 *
 * 参数：
 *   op                  - 选项结构体
 *   file                - 指向 new_files 数组中当前文件位置的指针
 *   name                - 文件路径（调用后被释放）
 *   type                - 文件类型（INTERFACE 或 MODULE）
 *   linked_name         - 链接模块名称（双重指针，处理后被重置为 NULL）
 *   core_name           - 核心对象名称（双重指针，处理后被重置为 NULL）
 *   target_directory    - 解包目标目录（双重指针，处理后被重置为 NULL）
 *   linked_module_file  - 未签名链接模块文件路径（双重指针，处理后被重置为 NULL）
 *   signed_module_file  - 已签名链接模块文件路径（双重指针，处理后被重置为 NULL）
 */
static void pack_a_file(Options *op, PrecompiledFileInfo *file,
                        char *name, uint32 type, char **linked_name,
                        char **core_name, char **target_directory,
                        char **linked_module_file, char **signed_module_file)
{
    if (!*target_directory) {
        *target_directory = nvstrdup("");
    }

    switch(type) {
    case PRECOMPILED_FILE_TYPE_INTERFACE:
        /* 内核接口文件必须指定链接模块名称和核心对象名称 */
        if (*linked_name == NULL || *core_name == NULL) {
            fprintf(stderr, "Each kernel interface file must have both the "
                    "--linked-module-name and --core-object-name options set "
                    "in order to be added to a package.\n");
            exit(1);
        }

        /* 读取内核接口文件内容 */
        if (!precompiled_read_interface(file, name, *linked_name, *core_name,
                                        *target_directory)) {
            fprintf(stderr, "Failed to read kernel interface '%s'.\n", name);
        exit(1);
        }
        break;

    case PRECOMPILED_FILE_TYPE_MODULE:
        /* 读取内核模块文件内容 */
        if (!precompiled_read_module(file, name, *target_directory)) {
            fprintf(stderr, "Failed to read kernel module '%s'.\n", name);
        }
        break;

    default:
        exit(1);
    }
    nvfree(name);

    /* 如果提供了签名相关文件，提取分离式签名 */
    if (!create_detached_signature(op, file, *linked_module_file,
                                   *signed_module_file)) {
        exit(1);
    }

    op->num_files++;

    /* 清理当前文件的所有附属选项值，为处理下一个文件做准备 */
    nvfree(*linked_name);
    nvfree(*core_name);
    nvfree(*target_directory);
    nvfree(*linked_module_file);
    nvfree(*signed_module_file);
    *linked_name = *core_name = *target_directory = *linked_module_file =
    *signed_module_file = NULL;
}

/*
 * parse_commandline() - 解析命令行参数。
 *
 * 进行基本的参数验证，初始化并返回一个堆分配的 Options 结构体。
 *
 * 命令行语法：mkprecompiled <action> <package-file> [options]
 *
 * 特殊处理：
 *   - --kernel-interface 和 --kernel-module 选项可多次出现，
 *     每次出现开始一个新文件的选项组
 *   - --signed、--linked-module、--signed-module 等选项必须在
 *     其关联的文件选项之后、下一个文件选项之前出现
 *
 * 参数：
 *   argc - 命令行参数个数
 *   argv - 命令行参数数组
 *
 * 返回值：初始化完成的 Options 结构体指针。解析失败时程序直接退出。
 */

static Options *parse_commandline(int argc, char *argv[])
{
    Options *op;
    int c, file_array_size = 16;       /* 文件数组的初始容量 */
    uint32 type = 0;                   /* 当前正在处理的文件类型 */
    PrecompiledFileInfo *file = NULL;   /* 当前正在填充的文件信息指针 */
    char *strval, *signed_mod = NULL, *linked_mod = NULL, *filename = NULL,
         *linked_name = NULL, *core_name = NULL, *target_directory = NULL;
    char see_help[1024];

    /* 命令行长选项定义表 */
    static const NVGetoptOption long_options[] = {
        { "pack",                PACK,   NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "unpack",              UNPACK, NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "info",                INFO,   NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "match",               MATCH,  NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "help",                'h',    0,                        NULL, NULL },
        { "description",         'd',    NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "output-directory",    'o',    NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "driver-version",      'v',    NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "proc-version-string", 'P',    NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "proc-mount-point",    PROC_MOUNT_POINT_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "kernel-interface",    KERNEL_INTERFACE_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "kernel-module",       KERNEL_MODULE_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "signed",              SIGNED_FILE_OPTION,
                                         0,                        NULL, NULL },
        { "linked-module",       LINKED_MODULE_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "signed-module",       LINKED_AND_SIGNED_MODULE_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "linked-module-name",  LINKED_MODULE_NAME_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "core-object-name",    CORE_OBJECT_NAME_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { "target-directory",    TARGET_DIRECTORY_OPTION,
                                         NVGETOPT_STRING_ARGUMENT, NULL, NULL },
        { NULL,                  0,   0,                        NULL, NULL }
    };

    /* 构建错误提示中的帮助引导信息 */
    snprintf(see_help, sizeof(see_help), "Please run `%s --help` for usage "
             "information.\n", argv[0]);
    see_help[sizeof(see_help) - 1] = '\0';

    /* 初始化选项结构体 */
    op = (Options *) nvalloc(sizeof(Options));
    op->new_files = nvalloc(sizeof(PrecompiledFileInfo) * file_array_size);
    op->num_files = -1;   /* -1 表示尚未开始处理文件（区别于 0 个文件） */

    op->proc_mount_point = PROC_MOUNT_POINT;

    /* 主解析循环：逐个处理命令行选项 */
    while (1) {
        c = nvgetopt(argc, argv, long_options, &strval,
                     NULL, /* boolval */
                     NULL, /* intval */
                     NULL, /* doubleval */
                     NULL  /* disable_val */);

        if (c == -1)
            break;    /* 所有选项都已处理完毕 */

        switch(c) {
        /* 操作类型选项：每个操作都需要一个包文件路径 */
        case PACK: case UNPACK: case INFO: case MATCH:
            set_action(op, c);
            op->package_file = strval;
            break;

        case 'h': print_help(); exit(0); break;
        case 'f': op->package_file = strval; break;           /* 包文件路径 */
        case 'd': op->description = strval; break;            /* 包描述 */
        case 'o': op->output_directory = strval; break;       /* 解包输出目录 */
        case 'v': op->version = strval; break;                /* 驱动版本号 */
        case 'P': op->proc_version_string = strval; break;    /* 内核版本字符串 */
        case PROC_MOUNT_POINT_OPTION: op->proc_mount_point = strval; break;

        case KERNEL_INTERFACE_OPTION: case KERNEL_MODULE_OPTION:
            /*
             * 遇到新的文件选项：如果之前有未完成的文件，先处理完前一个文件，
             * 然后开始一个新的文件记录。
             */

            grow_file_array(op, &file_array_size);

            if (file) {
                /* 打包前一个文件（读取内容、提取签名等） */
                pack_a_file(op, file, filename, type, &linked_name, &core_name,
                            &target_directory, &linked_mod, &signed_mod);
            }

            /* 指向数组中下一个空位置 */
            file = op->new_files + op->num_files;
            filename = strval;
            type = file_type_from_option(c);

            break;

        case SIGNED_FILE_OPTION:

            check_file_option_validity(op, "signed");

            /*
             * 对于内核接口文件，签名状态通过 linked_module_crc 和
             * detached_signature 的存在来隐含表示，不需要显式的属性标记。
             * 只有对于预编译内核模块，才设置嵌入式签名属性。
             */

            if (type == PRECOMPILED_FILE_TYPE_MODULE) {
                file->attributes |= PRECOMPILED_ATTR(EMBEDDED_SIGNATURE);
            }

            break;

        case LINKED_MODULE_OPTION:

            check_file_option_validity(op, "linked-module");
            linked_mod = strval;

            break;

        case LINKED_AND_SIGNED_MODULE_OPTION:

            check_file_option_validity(op, "signed-module");
            signed_mod = strval;

            break;

        case LINKED_MODULE_NAME_OPTION:

            check_file_option_validity(op, "linked-module-name");
            linked_name = strval;

            break;

        case CORE_OBJECT_NAME_OPTION:

            check_file_option_validity(op, "core-object-name");
            core_name = strval;

            break;

        case TARGET_DIRECTORY_OPTION:

            check_file_option_validity(op, "target-directory");
            target_directory = strval;

            break;

        default:
            fprintf (stderr, "Invalid commandline; %s", see_help);
            exit(0);
        }
    }


    /* 验证选项的完整性和一致性 */

    if (!op->action) {
        fprintf(stderr, "No action specified; one of --pack, --unpack, --info, "
                "or --match options must be given. %s", see_help);
        exit(1);
    }

    switch (op->action) {
    case PACK:
        /* 处理最后一个未完成的文件（如果有） */
        if (file) {
            pack_a_file(op, file, filename, type, &linked_name, &core_name,
                        &target_directory, &linked_mod, &signed_mod);
        }

        /* 打包操作至少需要一个文件 */
        if (op->num_files < 1) {
            fprintf(stderr, "At least one file to pack must be specified "
                    "when using the --pack option; %s", see_help);
            exit(1);
        }
        break;

    case UNPACK:
        /* 解包操作如果未指定输出目录，默认为当前目录 */
        if (!op->output_directory) {
            op->output_directory = ".";
        }
        break;

    case INFO: case MATCH: default: /* XXX 默认分支理论上不应该被执行到 */
        break;
    }

    /* 尝试读取已有的包文件（对于 PACK 操作，可能是追加到现有包） */
    op->package = get_precompiled_info(op, op->package_file, NULL, NULL, NULL);

    if (!op->package && op->action != PACK) {
        /* 非 PACK 操作必须有有效的输入包文件 */
        fprintf(stderr, "Unable to read package file '%s'.\n",
                op->package_file);
        exit(1);
    }

    return op;

} /* parse_commandline() */




/*
 * check_match() - 检查预编译包是否与当前运行的内核匹配。
 *
 * 读取当前系统的 /proc/version 内容，与包中存储的内核版本字符串进行
 * 精确比较（strcmp）。只有完全匹配时才能安全使用预编译的内核模块。
 *
 * 参数：
 *   op  - 选项结构体（包含 procfs 挂载点等配置）
 *   str - 包中存储的 /proc/version 字符串
 *
 * 返回值：1 表示匹配，0 表示不匹配。
 */

static int check_match(Options *op, char *str)
{
    int ret = 0;
    /* 读取当前系统的 /proc/version 文件内容 */
    char *version = read_proc_version(op, op->proc_mount_point);

    if (strcmp(version, str) == 0) {
        ret = 1;
        printf("kernel interface matches.\n");
    } else {
        ret = 0;
        printf("kernel interface doesn't match.\n");
    }

    free(version);

    return ret;

} /* check_match() */



/*
 * main() - 程序入口点。
 *
 * 解析命令行后根据操作类型执行对应的功能：
 *   PACK  : 将新文件追加到预编译包（如果包不存在则创建新包）
 *   UNPACK: 从包中解出所有文件到指定目录
 *   INFO  : 打印包的元信息和所有文件的详细信息
 *   MATCH : 检查包是否与当前运行的内核版本匹配
 *
 * 返回值：0 表示成功，1 表示失败。
 */

int main(int argc, char *argv[])
{
    Options *op;
    int ret = 1;    /* 默认返回失败，成功时显式设为 0 */

    /* 解析命令行参数 */
    op = parse_commandline(argc, argv);


    switch (op->action) {
    int i;

    case PACK:
        /* === 打包操作 === */

        if (!op->package) {
            /* 没有现有包文件（或无法读取），创建新的包 */
            if (!op->version) {
                /* 创建新包时必须指定驱动版本号 */
                fprintf (stderr, "The --driver-version option must be specified "
                         "when using the --pack option to create a new package; "
                         "Please run `%s --help` for usage information.\n",
                         argv[0]);
                exit (1);
            }

            op->package = nvalloc(sizeof(PrecompiledInfo));

            /* 设置包的元信息：描述、内核版本、驱动版本 */
            op->package->description = op->description ? op->description :
                                                         nvstrdup("");
            /* 如果未指定内核版本，从当前系统的 /proc/version 读取 */
            op->package->proc_version_string =
                op->proc_version_string ?
                    op->proc_version_string :
                    read_proc_version(op, op->proc_mount_point);
            op->package->version = op->version;
        }

        /* 将新文件追加到包中 */
        precompiled_append_files(op->package, op->new_files, op->num_files);

        /* 将包写入文件 */
        if (precompiled_pack(op->package, op->package_file)) {
            ret = 0;
        } else {
            fprintf(stderr, "An error occurred while writing the package "
                    "file '%s'.\n", op->package_file);
        }

        break;

    case UNPACK:
        /* === 解包操作 === */

        if (precompiled_unpack(op, op->package, op->output_directory)) {
            ret = 0;
        } else {
            fprintf(stderr, "An error occurred while unpacking the package "
                    "file '%s' to '%s'.\n", op->package_file,
                     op->output_directory);
        }
        break;

    case INFO:
        /* === 信息显示操作 === */

        /* 打印包的元信息 */
        printf("description: %s\n", op->package->description);
        printf("version: %s\n", op->package->version);
        printf("proc version: %s\n", op->package->proc_version_string);
        printf("number of files: %d\n\n", op->package->num_files);

        /* 遍历并打印每个文件的详细信息 */
        for (i = 0; i < op->package->num_files; i++) {
            PrecompiledFileInfo *file = op->package->files + i;
            const char **attrs, **attr;

            /* 获取文件属性的名称列表 */
            attrs = precompiled_file_attribute_names(file->attributes);

            printf("file %d:\n", i + 1);
            printf("  name: '%s'\n", file->name);
            printf("  type: %s\n",
                   precompiled_file_type_name(file->type));
            printf("  attributes: ");

            /* 打印所有属性名称（逗号分隔） */
            for (attr = attrs; *attr; attr++) {
                if (attr > attrs) {
                    printf(", ");
                }
                printf("%s", *attr);
            }

            printf("\n");

            printf("  size: %d bytes\n", file->size);
            printf("  crc: %" PRIu32 "\n", file->crc);
            printf("  target directory: %s\n", file->target_directory);

            /* 内核接口文件有额外的关联信息 */
            if (file->type == PRECOMPILED_FILE_TYPE_INTERFACE) {
                printf("  core object name: %s\n", file->core_object_name);
                printf("  linked module name: %s\n", file->linked_module_name);
                if (file->signature_size) {
                    printf("  linked module crc: %" PRIu32 "\n",
                           file->linked_module_crc);
                    printf("  signature size: %d\n", file->signature_size);
                }
            }

            printf("\n");

        }

        ret = 0;
        break;

    case MATCH:
        /* === 匹配检查操作 === */

        /* 比较包中的内核版本与当前系统的内核版本 */
        ret = check_match(op, op->package->proc_version_string);
        break;

    default: /* XXX 默认分支理论上不应该被执行到 */ break;

    }

    /* 释放包数据结构 */
    free_precompiled(op->package);

    return ret;

} /* main() */
