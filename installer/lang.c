#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "lang.h"
#include "resource.h"

#include <windows.h>

static JvmanInstallerLang active_lang = JVMAN_LANG_EN;

/*
 * 字符串表：lang_table[语言][字符串ID]
 * 所有用户界面文本集中管理，方便添加新语言
 */
static const wchar_t *const lang_table[JVMAN_LANG_COUNT][JVMAN_STR_COUNT] = {
    /* JVMAN_LANG_EN — English */
    [JVMAN_LANG_EN] = {
        [JVMAN_STR_APP_TITLE]            = L"jvman Setup",
        [JVMAN_STR_LANG_DIALOG_TITLE]    = L"jvman Setup - Select Language",
        [JVMAN_STR_LANG_SELECT]          = L"Interface language:",
        [JVMAN_STR_CONFIRM]              = L"OK",
        [JVMAN_STR_CANCEL]               = L"Cancel",
        [JVMAN_STR_LANG_SYSTEM]          = L"System default",

        [JVMAN_STR_INSTALL_PROMPT]       = L"Use the default jvman install directory?\n\n"
                                            L"Choose No to select another folder for a current-user install.\n"
                                            L"An all-users install always uses the protected Program Files directory.",
        [JVMAN_STR_BROWSE_TITLE]         = L"Choose the jvman install directory",
        [JVMAN_STR_ADD_PATH_PROMPT]      = L"Add jvman to PATH?\n\n"
                                            L"Current-user installs also add the stable current\\bin path.\n"
                                            L"Existing PATH entries are kept and duplicates are skipped.",
        [JVMAN_STR_PATH_SCOPE_PROMPT]    = L"Add PATH entries for all users?\n\n"
                                            L"Choose Yes for the system PATH. Choose No for only your user PATH.\n"
                                            L"Yes installs jvman in Program Files and Windows will request administrator permission.",
        [JVMAN_STR_CONFIGURE_JAVA_PROMPT]= L"Configure JAVA_HOME for jvman?\n\n"
                                            L"JAVA_HOME uses the stable current path and becomes valid after the first `jvman use`.\n"
                                            L"The stable current\\bin path follows the earlier PATH choice.\n"
                                            L"This may replace the current user JAVA_HOME value.",
        [JVMAN_STR_DISCOVER_PROMPT]      = L"Discover and register installed JDKs after installation?\n\n"
                                            L"JREs are never registered and the current version is not changed.",
        [JVMAN_STR_INSTALL_LOCATION]     = L"Install location:\n%s",

        [JVMAN_STR_CANNOT_DETERMINE_PATHS] = L"Cannot determine the default install paths.",
        [JVMAN_STR_CANNOT_READ_STATE]      = L"Cannot read installer state.",
        [JVMAN_STR_EXISTING_PATHS_INVALID] = L"The existing installation paths are invalid.",
        [JVMAN_STR_DIR_MISMATCH]           = L"The selected install directory does not match the existing installation.",
        [JVMAN_STR_STATE_MISMATCH]         = L"The existing installation state does not match this directory.",
        [JVMAN_STR_DIR_NOT_SAFE]           = L"The selected install directory is not safe.",
        [JVMAN_STR_NO_MEMORY]              = L"Not enough memory for installer state.",
        [JVMAN_STR_CANNOT_CREATE_DIR]      = L"Cannot create the install directory.",
        [JVMAN_STR_CANNOT_UPDATE_ENV]      = L"Cannot update the Windows environment.",
        [JVMAN_STR_CANNOT_INSTALL]         = L"Cannot install jvman.",
        [JVMAN_STR_DISCOVER_FAILED_TITLE]  = L"jvman Setup",
        [JVMAN_STR_DISCOVER_FAILED_DETAIL] = L"jvman was installed, but automatic JDK discovery failed.\n"
                                              L"You can run `jvman discover --register` later.",

        [JVMAN_STR_INSTALL_SUCCESS]       = L"jvman was installed.",
        [JVMAN_STR_INSTALL_SUCCESS_PATH]  = L"jvman was installed. Open a new terminal before using the updated PATH.",

        [JVMAN_STR_UNINSTALL_CONFIRM]      = L"Choose what to remove. External registered JDK installations are never deleted.",
        [JVMAN_STR_UNINSTALL_SCOPE_PROGRAM_ONLY]
                                            = L"Remove the jvman program only (keep all data and JDKs)",
        [JVMAN_STR_UNINSTALL_SCOPE_DATA]   = L"Remove the program and jvman data (keep managed JDK files)",
        [JVMAN_STR_UNINSTALL_SCOPE_ALL]    = L"Remove the program, all jvman data, and managed JDKs",
        [JVMAN_STR_UNINSTALL_SCOPE_FAILED] = L"Cannot open the uninstall options.",
        [JVMAN_STR_UNINSTALL_CONFIRM_FINAL]= L"This is the final confirmation.\n\n"
                                              L"Remove only the jvman program? Data and JDKs will be kept.",
        [JVMAN_STR_UNINSTALL_CONFIRM_FINAL_DATA]
                                            = L"This is the final confirmation.\n\n"
                                              L"Remove jvman and its data? Managed JDK files under the jdks directory will be kept.",
        [JVMAN_STR_UNINSTALL_CONFIRM_FINAL_ALL]
                                            = L"This is the final confirmation.\n\n"
                                              L"Permanently remove jvman, all jvman data, and every managed JDK? External registered JDKs will be kept.",
        [JVMAN_STR_UNINSTALL_RECORD_INVALID]= L"The installation record is missing or invalid.",
        [JVMAN_STR_UNINSTALL_ENV_FAILED]    = L"Cannot fully restore the Windows environment. "
                                              L"The installation was kept so you can retry.",
        [JVMAN_STR_UNINSTALL_DATA_FAILED]   = L"Cannot safely remove the selected jvman data. "
                                              L"The installation record was kept so you can retry.",
        [JVMAN_STR_UNINSTALL_FILES_FAILED]  = L"Cannot remove the installed files. "
                                              L"The installation record was kept so you can retry.",
        [JVMAN_STR_UNINSTALL_SUCCESS]       = L"jvman was removed. Existing JDKs and data were kept.",
        [JVMAN_STR_UNINSTALL_SUCCESS_DATA]  = L"jvman and its data were removed. Managed JDK files were kept in the jdks directory.",
        [JVMAN_STR_UNINSTALL_SUCCESS_ALL]   = L"jvman, its data, and all managed JDKs were removed. External registered JDKs were kept.",
        [JVMAN_STR_UNINSTALL_PARTIAL]       = L"jvman was partly removed. Restart Windows if the uninstaller is still present.",
        [JVMAN_STR_LEGACY_JAVA_HKLM_RELOCATED] = L"System PATH Java entries were moved to the tail so jvman takes precedence. New terminals will use the selected JDK.",

        [JVMAN_STR_INVALID_ARGS]           = L"Invalid or conflicting installer arguments.",
        [JVMAN_STR_ALREADY_RUNNING]        = L"Another jvman Setup instance is already running.",
        [JVMAN_STR_CANNOT_CREATE_INSTANCE_LOCK] = L"Cannot create the installer instance lock.",

        [JVMAN_STR_USAGE]                  = L"jvman setup\n\n"
                                              L"Options:\n"
                                              L"  /S, /SILENT, or /QUIET       silent install\n"
                                              L"  /DIR=<path>                  choose install directory\n"
                                              L"  /LANG=en|zh-CN               select installer and CLI language\n"
                                              L"  /ADD_TO_PATH                 enable PATH integration\n"
                                              L"  /USER_PATH                   add program and current\\bin paths for current user\n"
                                              L"  /SYSTEM_PATH                 install in Program Files and add system PATH\n"
                                              L"  /NO_PATH                     do not modify PATH\n"
                                              L"  /CONFIGURE_JAVA              configure current-user JAVA_HOME\n"
                                              L"  /NO_CONFIGURE_JAVA           restore installer-managed JAVA_HOME\n"
                                              L"  /REPLACE_JAVA_HOME          allow replacing user JAVA_HOME\n"
                                              L"  /DISCOVER                    register discovered JDKs after install\n"
                                              L"  /PORTABLE /DIR=<path>        extract only jvman.exe\n"
                                              L"  /UNINSTALL                   remove the current-user installation\n"
                                              L"  /UNINSTALL /MACHINE          remove the all-users installation\n"
                                              L"  /REMOVE_DATA                 also remove data except managed JDKs\n"
                                              L"  /REMOVE_DATA /REMOVE_JDKS    also remove managed JDKs",
    },

    /* JVMAN_LANG_ZH_CN — 简体中文 */
    [JVMAN_LANG_ZH_CN] = {
        [JVMAN_STR_APP_TITLE]            = L"jvman 安装程序",
        [JVMAN_STR_LANG_DIALOG_TITLE]    = L"jvman 安装程序 - 选择语言",
        [JVMAN_STR_LANG_SELECT]          = L"界面语言：",
        [JVMAN_STR_CONFIRM]              = L"确定",
        [JVMAN_STR_CANCEL]               = L"取消",
        [JVMAN_STR_LANG_SYSTEM]          = L"跟随系统",

        [JVMAN_STR_INSTALL_PROMPT]       = L"使用 jvman 默认安装目录？\n\n"
                                            L"选择“否”可为当前用户安装选择其他目录。\n"
                                            L"为所有用户安装时始终使用受保护的 Program Files 目录。",
        [JVMAN_STR_BROWSE_TITLE]         = L"选择 jvman 安装目录",
        [JVMAN_STR_ADD_PATH_PROMPT]      = L"将 jvman 添加到 PATH？\n\n"
                                            L"当前用户安装还会添加稳定的 current\\bin 路径。\n"
                                            L"已有的 PATH 条目将被保留，重复项会自动跳过。",
        [JVMAN_STR_PATH_SCOPE_PROMPT]    = L"为所有用户添加 PATH 条目？\n\n"
                                            L"选择“是”将写入系统 PATH，选择“否”仅写入当前用户 PATH。\n"
                                            L"选择“是”会将 jvman 安装到 Program Files，Windows 随后会请求管理员权限。",
        [JVMAN_STR_CONFIGURE_JAVA_PROMPT]= L"是否为 jvman 配置 JAVA_HOME？\n\n"
                                            L"JAVA_HOME 使用稳定的 current 路径，并在首次执行 `jvman use` 后变为有效。\n"
                                            L"稳定的 current\\bin 路径由前面的 PATH 选择管理。\n"
                                            L"这可能会替换当前用户的 JAVA_HOME 值。",
        [JVMAN_STR_DISCOVER_PROMPT]      = L"安装完成后是否自动发现并注册已安装的 JDK？\n\n"
                                            L"JRE 不会被注册，当前使用的版本不会被更改。",
        [JVMAN_STR_INSTALL_LOCATION]     = L"安装位置：\n%s",

        [JVMAN_STR_CANNOT_DETERMINE_PATHS] = L"无法确定默认安装路径。",
        [JVMAN_STR_CANNOT_READ_STATE]      = L"无法读取安装程序状态。",
        [JVMAN_STR_EXISTING_PATHS_INVALID] = L"已有安装路径无效。",
        [JVMAN_STR_DIR_MISMATCH]           = L"所选安装目录与已有安装不匹配。",
        [JVMAN_STR_STATE_MISMATCH]         = L"已有安装状态与此目录不匹配。",
        [JVMAN_STR_DIR_NOT_SAFE]           = L"所选安装目录不安全。",
        [JVMAN_STR_NO_MEMORY]              = L"内存不足，无法初始化安装程序状态。",
        [JVMAN_STR_CANNOT_CREATE_DIR]      = L"无法创建安装目录。",
        [JVMAN_STR_CANNOT_UPDATE_ENV]      = L"无法更新 Windows 环境变量。",
        [JVMAN_STR_CANNOT_INSTALL]         = L"无法安装 jvman。",
        [JVMAN_STR_DISCOVER_FAILED_TITLE]  = L"jvman 安装程序",
        [JVMAN_STR_DISCOVER_FAILED_DETAIL] = L"jvman 已安装，但自动发现 JDK 失败。\n"
                                              L"您可以稍后手动运行 `jvman discover --register`。",

        [JVMAN_STR_INSTALL_SUCCESS]       = L"jvman 安装完成。",
        [JVMAN_STR_INSTALL_SUCCESS_PATH]  = L"jvman 安装完成。请打开新终端以使用更新后的 PATH。",

        [JVMAN_STR_UNINSTALL_CONFIRM]      = L"请选择要移除的内容。通过外部路径注册的 JDK 永远不会被删除。",
        [JVMAN_STR_UNINSTALL_SCOPE_PROGRAM_ONLY]
                                            = L"仅移除 jvman 程序（保留全部数据和 JDK）",
        [JVMAN_STR_UNINSTALL_SCOPE_DATA]   = L"移除程序和 jvman 数据（保留托管 JDK 文件）",
        [JVMAN_STR_UNINSTALL_SCOPE_ALL]    = L"移除程序、全部 jvman 数据和托管 JDK",
        [JVMAN_STR_UNINSTALL_SCOPE_FAILED] = L"无法打开卸载选项。",
        [JVMAN_STR_UNINSTALL_CONFIRM_FINAL]= L"请再次确认。\n\n"
                                              L"仅移除 jvman 程序？数据和 JDK 将保留。",
        [JVMAN_STR_UNINSTALL_CONFIRM_FINAL_DATA]
                                            = L"请再次确认。\n\n"
                                              L"移除 jvman 和相关数据？jdks 目录中的托管 JDK 文件将保留。",
        [JVMAN_STR_UNINSTALL_CONFIRM_FINAL_ALL]
                                            = L"请再次确认。\n\n"
                                              L"永久移除 jvman、全部数据和所有托管 JDK？通过外部路径注册的 JDK 将保留。",
        [JVMAN_STR_UNINSTALL_RECORD_INVALID]= L"安装记录缺失或无效。",
        [JVMAN_STR_UNINSTALL_ENV_FAILED]    = L"无法完全还原 Windows 环境变量。"
                                              L"安装已保留，您可以重试。",
        [JVMAN_STR_UNINSTALL_DATA_FAILED]   = L"无法安全移除所选的 jvman 数据。"
                                              L"安装记录已保留，您可以重试。",
        [JVMAN_STR_UNINSTALL_FILES_FAILED]  = L"无法移除已安装的文件。"
                                              L"安装记录已保留，您可以重试。",
        [JVMAN_STR_UNINSTALL_SUCCESS]       = L"jvman 已移除。已有的 JDK 和数据已保留。",
        [JVMAN_STR_UNINSTALL_SUCCESS_DATA]  = L"jvman 和相关数据已移除。托管 JDK 文件已保留在 jdks 目录中。",
        [JVMAN_STR_UNINSTALL_SUCCESS_ALL]   = L"jvman、相关数据和全部托管 JDK 已移除。外部注册的 JDK 已保留。",
        [JVMAN_STR_UNINSTALL_PARTIAL]       = L"jvman 已部分移除。如果卸载程序仍然存在，请重启 Windows。",
        [JVMAN_STR_LEGACY_JAVA_HKLM_RELOCATED] = L"系统 PATH 中的旧 Java 条目已移至末尾，jvman 的 JAVA_HOME 已优先。新终端将使用所选 JDK。",

        [JVMAN_STR_INVALID_ARGS]           = L"安装参数无效或存在冲突。",
        [JVMAN_STR_ALREADY_RUNNING]        = L"另一个 jvman 安装程序已在运行。",
        [JVMAN_STR_CANNOT_CREATE_INSTANCE_LOCK] = L"无法创建安装程序实例锁。",

        [JVMAN_STR_USAGE]                  = L"jvman 安装程序\n\n"
                                              L"选项：\n"
                                              L"  /S、/SILENT 或 /QUIET        静默安装\n"
                                              L"  /DIR=<路径>                  选择安装目录\n"
                                              L"  /LANG=en|zh-CN               选择安装器和 CLI 语言\n"
                                              L"  /ADD_TO_PATH                 启用 PATH 集成\n"
                                              L"  /USER_PATH                   为当前用户添加程序和 current\\bin 路径\n"
                                              L"  /SYSTEM_PATH                 安装到 Program Files 并添加系统 PATH\n"
                                              L"  /NO_PATH                     不修改 PATH\n"
                                              L"  /CONFIGURE_JAVA              配置当前用户 JAVA_HOME\n"
                                              L"  /NO_CONFIGURE_JAVA           恢复安装器管理的 JAVA_HOME\n"
                                              L"  /REPLACE_JAVA_HOME           允许替换用户 JAVA_HOME\n"
                                              L"  /DISCOVER                    安装后注册发现的 JDK\n"
                                              L"  /PORTABLE /DIR=<路径>        仅解压 jvman.exe\n"
                                              L"  /UNINSTALL                   移除当前用户安装\n"
                                              L"  /UNINSTALL /MACHINE          移除所有用户安装\n"
                                              L"  /REMOVE_DATA                 同时删除托管 JDK 之外的数据\n"
                                              L"  /REMOVE_DATA /REMOVE_JDKS    同时删除托管 JDK",
    },
};

static const wchar_t *const environment_status_table
    [JVMAN_LANG_COUNT][JVMAN_ENV_UNSUPPORTED + 1] = {
    [JVMAN_LANG_EN] = {
        [JVMAN_ENV_OK] = L"ok",
        [JVMAN_ENV_NOT_FOUND] = L"not found",
        [JVMAN_ENV_INVALID_ARGUMENT] = L"invalid argument",
        [JVMAN_ENV_UNSUPPORTED_TYPE] = L"unsupported registry value type",
        [JVMAN_ENV_TOO_LONG] = L"registry value is too long",
        [JVMAN_ENV_NO_MEMORY] = L"out of memory",
        [JVMAN_ENV_WIN32_ERROR] = L"Windows registry operation failed",
        [JVMAN_ENV_ACCESS_DENIED] = L"administrator permission is required for the selected environment change",
        [JVMAN_ENV_METADATA_INVALID] = L"installer metadata is malformed",
        [JVMAN_ENV_CONFLICT] = L"JAVA_HOME was changed by the user",
        [JVMAN_ENV_UNSUPPORTED] = L"installer environment backend is unsupported",
    },
    [JVMAN_LANG_ZH_CN] = {
        [JVMAN_ENV_OK] = L"正常",
        [JVMAN_ENV_NOT_FOUND] = L"未找到",
        [JVMAN_ENV_INVALID_ARGUMENT] = L"参数无效",
        [JVMAN_ENV_UNSUPPORTED_TYPE] = L"不支持的注册表值类型",
        [JVMAN_ENV_TOO_LONG] = L"注册表值过长",
        [JVMAN_ENV_NO_MEMORY] = L"内存不足",
        [JVMAN_ENV_WIN32_ERROR] = L"Windows 注册表操作失败",
        [JVMAN_ENV_ACCESS_DENIED] = L"所选环境变量变更需要管理员权限",
        [JVMAN_ENV_METADATA_INVALID] = L"安装程序元数据格式无效",
        [JVMAN_ENV_CONFLICT] = L"JAVA_HOME 已被用户修改",
        [JVMAN_ENV_UNSUPPORTED] = L"当前系统不支持安装程序环境变量后端",
    },
};

static const wchar_t *const install_status_table
    [JVMAN_LANG_COUNT][JVMAN_INSTALL_SELF_CLEANUP_REQUIRED + 1] = {
    [JVMAN_LANG_EN] = {
        [JVMAN_INSTALL_OK] = L"ok",
        [JVMAN_INSTALL_INVALID_ARGUMENT] = L"invalid argument",
        [JVMAN_INSTALL_INVALID_PATH] = L"invalid path",
        [JVMAN_INSTALL_PATH_TOO_LONG] = L"path too long",
        [JVMAN_INSTALL_PATH_NOT_LOCAL] = L"path is not on a local disk",
        [JVMAN_INSTALL_PATH_REPARSE] = L"reparse point is not allowed",
        [JVMAN_INSTALL_PATH_NOT_FOUND] = L"path not found",
        [JVMAN_INSTALL_IO_ERROR] = L"Windows I/O error",
        [JVMAN_INSTALL_FORMAT_ERROR] = L"invalid setup format",
        [JVMAN_INSTALL_HASH_MISMATCH] = L"payload hash mismatch",
        [JVMAN_INSTALL_MARKER_INVALID] = L"invalid installation marker",
        [JVMAN_INSTALL_NOT_FOUND] = L"not found",
        [JVMAN_INSTALL_NO_MEMORY] = L"out of memory",
        [JVMAN_INSTALL_SELF_CLEANUP_REQUIRED] = L"out-of-process self-cleanup is required",
    },
    [JVMAN_LANG_ZH_CN] = {
        [JVMAN_INSTALL_OK] = L"正常",
        [JVMAN_INSTALL_INVALID_ARGUMENT] = L"参数无效",
        [JVMAN_INSTALL_INVALID_PATH] = L"路径无效",
        [JVMAN_INSTALL_PATH_TOO_LONG] = L"路径过长",
        [JVMAN_INSTALL_PATH_NOT_LOCAL] = L"路径不在本地磁盘上",
        [JVMAN_INSTALL_PATH_REPARSE] = L"不允许重解析点",
        [JVMAN_INSTALL_PATH_NOT_FOUND] = L"未找到路径",
        [JVMAN_INSTALL_IO_ERROR] = L"Windows I/O 错误",
        [JVMAN_INSTALL_FORMAT_ERROR] = L"安装程序格式无效",
        [JVMAN_INSTALL_HASH_MISMATCH] = L"负载哈希校验失败",
        [JVMAN_INSTALL_MARKER_INVALID] = L"安装标记无效",
        [JVMAN_INSTALL_NOT_FOUND] = L"未找到",
        [JVMAN_INSTALL_NO_MEMORY] = L"内存不足",
        [JVMAN_INSTALL_SELF_CLEANUP_REQUIRED] = L"需要通过独立进程完成自清理",
    },
};

static unsigned int jvman_lang_index(void) {
    unsigned int language = (unsigned int)active_lang;
    return language < (unsigned int)JVMAN_LANG_COUNT
               ? language
               : (unsigned int)JVMAN_LANG_EN;
}

static int jvman_lang_dialog_selection(HWND dialog,
                                       unsigned int *choice_out) {
    LRESULT selection;
    LRESULT item_data;
    if (!dialog || !choice_out) return -1;
    selection = SendDlgItemMessageW(
        dialog, IDC_JVMAN_LANGUAGE_COMBO, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return -1;
    item_data = SendDlgItemMessageW(
        dialog, IDC_JVMAN_LANGUAGE_COMBO, CB_GETITEMDATA,
        (WPARAM)selection, 0);
    if (item_data == CB_ERR || item_data < 0 ||
        (unsigned int)item_data > (unsigned int)JVMAN_LANG_COUNT) {
        return -1;
    }
    *choice_out = (unsigned int)item_data;
    return 0;
}

static int jvman_lang_apply_dialog_choice(unsigned int choice) {
    if (choice == (unsigned int)JVMAN_LANG_COUNT) {
        jvman_lang_use_system_default();
        return 0;
    }
    return jvman_lang_set((JvmanInstallerLang)choice);
}

static int jvman_lang_dialog_populate(HWND dialog, unsigned int choice) {
    unsigned int language;
    LRESULT selected_item = CB_ERR;
    LRESULT item;
    if (!dialog || choice > (unsigned int)JVMAN_LANG_COUNT ||
        SendDlgItemMessageW(dialog, IDC_JVMAN_LANGUAGE_COMBO,
                            CB_RESETCONTENT, 0, 0) == CB_ERR) {
        return -1;
    }
    item = SendDlgItemMessageW(
        dialog, IDC_JVMAN_LANGUAGE_COMBO, CB_ADDSTRING, 0,
        (LPARAM)jvman_lang_str(JVMAN_STR_LANG_SYSTEM));
    if (item == CB_ERR || item == CB_ERRSPACE ||
        SendDlgItemMessageW(dialog, IDC_JVMAN_LANGUAGE_COMBO, CB_SETITEMDATA,
                            (WPARAM)item, (LPARAM)JVMAN_LANG_COUNT) == CB_ERR) {
        return -1;
    }
    if (choice == (unsigned int)JVMAN_LANG_COUNT) selected_item = item;
    for (language = 0; language < (unsigned int)JVMAN_LANG_COUNT;
         ++language) {
        item = SendDlgItemMessageW(
            dialog, IDC_JVMAN_LANGUAGE_COMBO, CB_ADDSTRING, 0,
            (LPARAM)jvman_lang_name((JvmanInstallerLang)language));
        if (item == CB_ERR || item == CB_ERRSPACE ||
            SendDlgItemMessageW(dialog, IDC_JVMAN_LANGUAGE_COMBO,
                                CB_SETITEMDATA, (WPARAM)item,
                                (LPARAM)language) == CB_ERR) {
            return -1;
        }
        if (choice == language) selected_item = item;
    }
    return selected_item == CB_ERR ||
                   SendDlgItemMessageW(dialog, IDC_JVMAN_LANGUAGE_COMBO,
                                       CB_SETCURSEL,
                                       (WPARAM)selected_item, 0) == CB_ERR
               ? -1
               : 0;
}

static void jvman_lang_dialog_refresh(HWND dialog) {
    SetWindowTextW(dialog, jvman_lang_str(JVMAN_STR_LANG_DIALOG_TITLE));
    SetDlgItemTextW(dialog, IDC_JVMAN_LANGUAGE_LABEL,
                    jvman_lang_str(JVMAN_STR_LANG_SELECT));
    SetDlgItemTextW(dialog, IDOK, jvman_lang_str(JVMAN_STR_CONFIRM));
    SetDlgItemTextW(dialog, IDCANCEL, jvman_lang_str(JVMAN_STR_CANCEL));
}

static INT_PTR CALLBACK jvman_lang_dialog_proc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)lparam);
            if (jvman_lang_dialog_populate(
                    dialog, (unsigned int)JVMAN_LANG_COUNT) != 0) {
                EndDialog(dialog, -1);
                return TRUE;
            }
            jvman_lang_dialog_refresh(dialog);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDC_JVMAN_LANGUAGE_COMBO &&
                HIWORD(wparam) == CBN_SELCHANGE) {
                unsigned int selected;
                if (jvman_lang_dialog_selection(dialog, &selected) != 0 ||
                    jvman_lang_apply_dialog_choice(selected) != 0 ||
                    jvman_lang_dialog_populate(dialog, selected) != 0) {
                    EndDialog(dialog, -1);
                    return TRUE;
                }
                jvman_lang_dialog_refresh(dialog);
                return TRUE;
            }
            if (LOWORD(wparam) == IDOK) {
                unsigned int selected;
                if (jvman_lang_dialog_selection(dialog, &selected) != 0 ||
                    jvman_lang_apply_dialog_choice(selected) != 0) {
                    MessageBeep(MB_ICONWARNING);
                    return TRUE;
                }
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                (void)jvman_lang_set((JvmanInstallerLang)GetWindowLongPtrW(
                    dialog, DWLP_USER));
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            (void)jvman_lang_set((JvmanInstallerLang)GetWindowLongPtrW(
                dialog, DWLP_USER));
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

int jvman_lang_select_dialog(void) {
    JvmanInstallerLang original = active_lang;
    INT_PTR result;
    jvman_lang_use_system_default();
    result = DialogBoxParamW(
        GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_JVMAN_LANGUAGE), NULL,
        jvman_lang_dialog_proc, (LPARAM)original);
    if (result == IDOK) return 0;
    active_lang = original;
    return result == IDCANCEL ? 1 : -1;
}

void jvman_lang_use_system_default(void) {
    LANGID language = GetUserDefaultUILanguage();
    WORD sublanguage = SUBLANGID(language);
    active_lang = PRIMARYLANGID(language) == LANG_CHINESE &&
                          (sublanguage == SUBLANG_CHINESE_SIMPLIFIED ||
                           sublanguage == SUBLANG_CHINESE_SINGAPORE)
                      ? JVMAN_LANG_ZH_CN
                      : JVMAN_LANG_EN;
}

int jvman_lang_set(JvmanInstallerLang lang) {
    if ((unsigned int)lang >= (unsigned int)JVMAN_LANG_COUNT) return -1;
    active_lang = lang;
    return 0;
}

JvmanInstallerLang jvman_lang_current(void) {
    return active_lang;
}

const wchar_t *jvman_lang_str(JvmanStringId id) {
    unsigned int index = (unsigned int)id;
    unsigned int language = jvman_lang_index();
    const wchar_t *value;
    if (index >= (unsigned int)JVMAN_STR_COUNT) return L"";
    value = lang_table[language][index];
    if (!value) value = lang_table[JVMAN_LANG_EN][index];
    return value ? value : L"";
}

const wchar_t *jvman_lang_name(JvmanInstallerLang lang) {
    switch (lang) {
        case JVMAN_LANG_EN:    return L"English";
        case JVMAN_LANG_ZH_CN: return L"简体中文";
        default:               return L"Unknown";
    }
}

const wchar_t *jvman_lang_environment_status(JvmanEnvironmentStatus status) {
    unsigned int language = jvman_lang_index();
    unsigned int index = (unsigned int)status;
    const wchar_t *value;
    if (index > (unsigned int)JVMAN_ENV_UNSUPPORTED) {
        return language == (unsigned int)JVMAN_LANG_ZH_CN
                   ? L"未知环境变量错误"
                   : L"unknown environment error";
    }
    value = environment_status_table[language][index];
    if (!value) value = environment_status_table[JVMAN_LANG_EN][index];
    return value ? value : L"unknown environment error";
}

const wchar_t *jvman_lang_install_status(JvmanInstallStatus status) {
    unsigned int language = jvman_lang_index();
    unsigned int index = (unsigned int)status;
    const wchar_t *value;
    if (index > (unsigned int)JVMAN_INSTALL_SELF_CLEANUP_REQUIRED) {
        return language == (unsigned int)JVMAN_LANG_ZH_CN
                   ? L"未知安装程序错误"
                   : L"unknown installer error";
    }
    value = install_status_table[language][index];
    if (!value) value = install_status_table[JVMAN_LANG_EN][index];
    return value ? value : L"unknown installer error";
}
