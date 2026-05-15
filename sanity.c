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
 * 【文件说明】安装完整性检查（sanity check）实现。
 * 当用户使用 --sanity 选项时调用，检查已安装的 NVIDIA 驱动是否完好：
 *   1. 验证是否存在已安装的驱动
 *   2. 检查备份日志中记录的所有已安装文件是否仍然存在
 *   3. 报告检查结果
 *
 * sanity.c
 */

#include "nvidia-installer.h"
#include "user-interface.h"
#include "backup.h"
#include "sanity.h"


/*
 * sanity() - 对已安装的 NVIDIA 驱动执行完整性检查。
 *
 * 流程：
 *   1. 调用 get_installed_driver_version_and_descr() 获取已安装驱动的版本和描述
 *   2. 如果没有找到已安装驱动，报错并返回 FALSE
 *   3. 调用 test_installed_files() 检查所有已安装文件是否存在
 *   4. 如果有文件缺失或被修改，提示用户重新安装
 *   5. 所有检查通过则报告安装正常
 *
 * 返回值：TRUE 表示检查通过，FALSE 表示检查失败
 */

int sanity(Options *op)
{
    char *descr, *version;
    int ret;

    /* 步骤1：检查是否存在已安装的驱动 */

    ret = get_installed_driver_version_and_descr(op, &version, &descr);

    if (!ret) {
        ui_error(op, "Unable to find any installed NVIDIA driver.  The sanity "
                 "check feature is only intended to be used with an existing "
                 "NVIDIA driver installation.");
        return FALSE;
    }

    /* 显示当前已安装驱动的信息 */
    ui_message(op, "The currently installed driver is: '%s' "
               "(version: %s).  nvidia-installer will now check "
               "that all installed files still exist.",
               descr, version);

    /* 步骤2：检查备份日志中记录的所有已安装文件是否仍存在于原位置 */

    if (!test_installed_files(op)) {
        ui_message(op, "The '%s' installation has been altered "
                   "since it was originally installed.  It is recommended "
                   "that you reinstall.", descr);
        return FALSE;
    }

    /*
     * 【标注：未实现】以下是计划中但尚未实现的额外检查项：
     *
     * - 检查是否存在冲突的库文件
     * - 检查 /dev/nvidia* 设备文件的权限是否被 PAM 修改
     * - 检查 /dev/zero 的权限是否正确
     * - 检查内核配置问题（IPC、MTRR 支持等）
     */

    /* 所有检查通过，报告安装正常 */
    ui_message(op, "'%s' (version: %s) appears to be installed "
               "correctly.", descr, version);

    nvfree(descr);
    nvfree(version);

    return TRUE;

} /* sanity() */
