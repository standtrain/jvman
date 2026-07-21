#ifndef JVMAN_INSTALLER_LANG_H
#define JVMAN_INSTALLER_LANG_H

#include "environment.h"
#include "files.h"

#include <wchar.h>

typedef enum JvmanInstallerLang {
    JVMAN_LANG_EN = 0,
    JVMAN_LANG_ZH_CN = 1,
    JVMAN_LANG_COUNT
} JvmanInstallerLang;

/* 所有用户可见字符串的 ID */
typedef enum JvmanStringId {
    /* 通用 */
    JVMAN_STR_APP_TITLE,
    JVMAN_STR_LANG_DIALOG_TITLE,
    JVMAN_STR_LANG_SELECT,
    JVMAN_STR_CONFIRM,
    JVMAN_STR_CANCEL,
    JVMAN_STR_LANG_SYSTEM,

    /* 安装 GUI 对话框 */
    JVMAN_STR_INSTALL_PROMPT,
    JVMAN_STR_BROWSE_TITLE,
    JVMAN_STR_ADD_PATH_PROMPT,
    JVMAN_STR_PATH_SCOPE_PROMPT,
    JVMAN_STR_CONFIGURE_JAVA_PROMPT,
    JVMAN_STR_DISCOVER_PROMPT,
    JVMAN_STR_INSTALL_LOCATION,

    /* 安装过程消息 */
    JVMAN_STR_CANNOT_DETERMINE_PATHS,
    JVMAN_STR_CANNOT_READ_STATE,
    JVMAN_STR_EXISTING_PATHS_INVALID,
    JVMAN_STR_DIR_MISMATCH,
    JVMAN_STR_STATE_MISMATCH,
    JVMAN_STR_DIR_NOT_SAFE,
    JVMAN_STR_NO_MEMORY,
    JVMAN_STR_CANNOT_CREATE_DIR,
    JVMAN_STR_CANNOT_UPDATE_ENV,
    JVMAN_STR_CANNOT_INSTALL,
    JVMAN_STR_DISCOVER_FAILED_TITLE,
    JVMAN_STR_DISCOVER_FAILED_DETAIL,

    /* 安装成功 */
    JVMAN_STR_INSTALL_SUCCESS,
    JVMAN_STR_INSTALL_SUCCESS_PATH,

    /* 卸载 */
    JVMAN_STR_UNINSTALL_CONFIRM,
    JVMAN_STR_UNINSTALL_SCOPE_PROGRAM_ONLY,
    JVMAN_STR_UNINSTALL_SCOPE_DATA,
    JVMAN_STR_UNINSTALL_SCOPE_ALL,
    JVMAN_STR_UNINSTALL_SCOPE_FAILED,
    JVMAN_STR_UNINSTALL_CONFIRM_FINAL,
    JVMAN_STR_UNINSTALL_CONFIRM_FINAL_DATA,
    JVMAN_STR_UNINSTALL_CONFIRM_FINAL_ALL,
    JVMAN_STR_UNINSTALL_RECORD_INVALID,
    JVMAN_STR_UNINSTALL_ENV_FAILED,
    JVMAN_STR_UNINSTALL_DATA_FAILED,
    JVMAN_STR_UNINSTALL_FILES_FAILED,
    JVMAN_STR_UNINSTALL_SUCCESS,
    JVMAN_STR_UNINSTALL_SUCCESS_DATA,
    JVMAN_STR_UNINSTALL_SUCCESS_ALL,
    JVMAN_STR_UNINSTALL_PARTIAL,

    /* 错误 */
    JVMAN_STR_INVALID_ARGS,
    JVMAN_STR_ALREADY_RUNNING,
    JVMAN_STR_CANNOT_CREATE_INSTANCE_LOCK,

    /* 命令行帮助 */
    JVMAN_STR_USAGE,

    JVMAN_STR_COUNT
} JvmanStringId;

/* Show the language dialog: 0 accepted, 1 canceled, -1 on dialog failure. */
int jvman_lang_select_dialog(void);

/* 根据 Windows UI 语言设置弹窗前的默认语言 */
void jvman_lang_use_system_default(void);

/* 设置当前语言，无效值不会改变当前语言 */
int jvman_lang_set(JvmanInstallerLang lang);

/* 获取当前语言下指定 ID 的字符串 */
const wchar_t *jvman_lang_str(JvmanStringId id);

/* 获取语言的显示名称 */
const wchar_t *jvman_lang_name(JvmanInstallerLang lang);

/* 获取安装器内部状态的本地化用户消息 */
const wchar_t *jvman_lang_environment_status(JvmanEnvironmentStatus status);
const wchar_t *jvman_lang_install_status(JvmanInstallStatus status);

#endif
