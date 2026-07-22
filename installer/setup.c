#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "common.h"
#include "environment.h"
#include "files.h"
#include "lang.h"
#include "pathlist.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define INSTALLER_MAX_ARGS 32
#define INSTALLER_TEXT_MAX 8192
#define INSTALLER_COMMAND_CHARS 32768
#define INSTALLER_COPY_BUFFER_SIZE (64u * 1024u)
#define INSTALLER_TEMP_ATTEMPTS 128u
#define INSTALLER_MUTEX_NAME L"Local\\jvman.Setup.1F07284D-788B-4F89-A327-DA0F15511708"
#define INSTALLER_MACHINE_LOCK_SUFFIX \
    L".setup.6C6E4784-89F7-45A5-A7AF-77F3EC836B76.lock"
#define INSTALLER_ALREADY_RUNNING_EXIT 3
#define INSTALLER_ELEVATED_RESUME_SWITCH L"/_JVMAN_MACHINE_ELEVATED_V1"
#define INSTALLER_INTERNAL_LANG_OPTION L"/_JVMAN_LANG"
#define INSTALLER_PUBLIC_LANG_OPTION L"/LANG"
#define INSTALLER_LEGACY_INSTALL_OPTION L"/_JVMAN_LEGACY_INSTALL"
#define INSTALLER_LEGACY_DATA_OPTION L"/_JVMAN_LEGACY_DATA"
#define INSTALLER_LEGACY_ID_OPTION L"/_JVMAN_LEGACY_ID"
#define INSTALLER_LEGACY_FLAGS_OPTION L"/_JVMAN_LEGACY_FLAGS"
#define INSTALLER_LEGACY_CLEANUP_ONLY_SWITCH L"/_JVMAN_LEGACY_CLEANUP_ONLY"
#define INSTALLER_LEGACY_RESTORE_ONLY_SWITCH L"/_JVMAN_LEGACY_RESTORE_ONLY"
#define INSTALLER_JAVA_HKLM_RELOCATE_SWITCH L"/_JVMAN_JAVA_HKLM_RELOCATE_ONLY"
#define INSTALLER_JAVA_HKLM_RESTORE_SWITCH L"/_JVMAN_JAVA_HKLM_RESTORE_ONLY"
#define INSTALLER_UNINSTALL_CONFIRMED_SWITCH \
    L"/_JVMAN_UNINSTALL_CONFIRMED"
#define INSTALLER_MUTEX_HANDOFF_TIMEOUT_MS 30000u
#define INSTALLER_LEGACY_CLEANUP_EXIT_BASE 64u
#define INSTALLER_LEGACY_CLEANUP_RESULT_BITS 3u
#define INSTALLER_LEGACY_CLEANUP_RESULT_MASK 0x07u
#define INSTALLER_LEGACY_APP_PATH 0x01u
#define INSTALLER_LEGACY_JAVA_PATH 0x02u
#define INSTALLER_LEGACY_PATH_MASK \
    (INSTALLER_LEGACY_APP_PATH | INSTALLER_LEGACY_JAVA_PATH)
#define INSTALLER_SOURCE_COMPONENT_LIMIT 128u

/* MinGW headers hide FileDispositionInfoEx unless NTDDI_VERSION is raised,
 * while the numeric FILE_INFO_BY_HANDLE_CLASS value is stable on Windows 10.
 * Keep a local compatibility definition so the installer still targets the
 * SDK baseline used by the rest of the project. */
typedef struct JvmanFileDispositionInfoEx {
    DWORD Flags;
} JvmanFileDispositionInfoEx;
#define JVMAN_FILE_DISPOSITION_INFO_EX_CLASS ((FILE_INFO_BY_HANDLE_CLASS)21)
#define JVMAN_FILE_DISPOSITION_FLAG_DELETE 0x00000001u
#define JVMAN_FILE_DISPOSITION_FLAG_POSIX 0x00000002u
#define JVMAN_FILE_DISPOSITION_FLAG_IGNORE_READONLY 0x00000010u

typedef struct InstallerOptions {
    int silent;
    int portable;
    int uninstall;
    int remove_data;
    int remove_jdks;
    int uninstall_scope_set;
    int uninstall_confirmed;
    int add_path;
    int path_scope_set;
    JvmanEnvironmentScope path_scope;
    int configure_java;
    int replace_java_home;
    int discover;
    int machine_mode;
    int elevated_resume;
    int legacy_cleanup_only;
    int legacy_restore_only;
    int legacy_cleanup_applied;
    unsigned int legacy_cleanup_flags;
    /* 0.5.0: opt-out flag; 1 attempts to move Java-family entries in the
     * machine PATH to the tail after installing. -1 explicitly disables it. */
    int relocate_legacy_java;
    int java_hklm_relocate_only;
    int java_hklm_restore_only;
    int language_set;
    JvmanInstallerLang language;
    int install_dir_set;
    wchar_t install_dir[JVMAN_INSTALL_PATH_CHARS];
    wchar_t legacy_install_dir[JVMAN_INSTALL_PATH_CHARS];
    wchar_t legacy_data_home[JVMAN_INSTALL_PATH_CHARS];
    wchar_t legacy_install_id[JVMAN_INSTALL_MARKER_ID_CHARS];
} InstallerOptions;

typedef struct InstallerCommandBuilder {
    wchar_t *buffer;
    size_t capacity;
    size_t length;
} InstallerCommandBuilder;

typedef struct InstallerSourceLock {
    HANDLE file;
    HANDLE directories[INSTALLER_SOURCE_COMPONENT_LIMIT];
    size_t directory_count;
} InstallerSourceLock;

static int installer_message_box(const wchar_t *message,
                                 const wchar_t *title, UINT type) {
    LANGID language = jvman_lang_current() == JVMAN_LANG_ZH_CN
                          ? MAKELANGID(LANG_CHINESE,
                                      SUBLANG_CHINESE_SIMPLIFIED)
                          : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    return MessageBoxExW(NULL, message, title, type, language);
}

static void installer_uninstall_scope_dialog_refresh(HWND dialog) {
    if (!dialog) return;
    SetWindowTextW(dialog, jvman_lang_str(JVMAN_STR_APP_TITLE));
    SetDlgItemTextW(dialog, IDC_JVMAN_UNINSTALL_SCOPE_PROMPT,
                    jvman_lang_str(JVMAN_STR_UNINSTALL_CONFIRM));
    SetDlgItemTextW(dialog, IDC_JVMAN_UNINSTALL_PROGRAM_ONLY,
                    jvman_lang_str(JVMAN_STR_UNINSTALL_SCOPE_PROGRAM_ONLY));
    SetDlgItemTextW(dialog, IDC_JVMAN_UNINSTALL_DATA,
                    jvman_lang_str(JVMAN_STR_UNINSTALL_SCOPE_DATA));
    SetDlgItemTextW(dialog, IDC_JVMAN_UNINSTALL_ALL,
                    jvman_lang_str(JVMAN_STR_UNINSTALL_SCOPE_ALL));
    SetDlgItemTextW(dialog, IDOK, jvman_lang_str(JVMAN_STR_CONFIRM));
    SetDlgItemTextW(dialog, IDCANCEL, jvman_lang_str(JVMAN_STR_CANCEL));
}

static INT_PTR CALLBACK installer_uninstall_scope_dialog_proc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    InstallerOptions *options = (InstallerOptions *)GetWindowLongPtrW(
        dialog, DWLP_USER);
    switch (message) {
        case WM_INITDIALOG: {
            int selected = IDC_JVMAN_UNINSTALL_PROGRAM_ONLY;
            options = (InstallerOptions *)lparam;
            if (!options) {
                EndDialog(dialog, -1);
                return TRUE;
            }
            SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)options);
            if (options->remove_jdks) selected = IDC_JVMAN_UNINSTALL_ALL;
            else if (options->remove_data) selected = IDC_JVMAN_UNINSTALL_DATA;
            CheckRadioButton(
                dialog, IDC_JVMAN_UNINSTALL_PROGRAM_ONLY,
                IDC_JVMAN_UNINSTALL_ALL, selected);
            installer_uninstall_scope_dialog_refresh(dialog);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                if (!options) {
                    EndDialog(dialog, -1);
                    return TRUE;
                }
                options->remove_jdks =
                    IsDlgButtonChecked(dialog, IDC_JVMAN_UNINSTALL_ALL) ==
                    BST_CHECKED;
                options->remove_data = options->remove_jdks ||
                    IsDlgButtonChecked(dialog, IDC_JVMAN_UNINSTALL_DATA) ==
                        BST_CHECKED;
                options->uninstall_scope_set = 1;
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

static int installer_select_uninstall_scope(InstallerOptions *options) {
    INT_PTR result;
    if (!options) return -1;
    result = DialogBoxParamW(
        GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_JVMAN_UNINSTALL_SCOPE),
        NULL, installer_uninstall_scope_dialog_proc, (LPARAM)options);
    if (result == IDOK) return 0;
    if (result == IDCANCEL) return 1;
    return -1;
}

static int installer_confirm_uninstall(InstallerOptions *options) {
    const wchar_t *confirmation;
    int scope_result;
    if (!options || !options->uninstall || options->silent) return -1;
    if (options->machine_mode) options->uninstall_scope_set = 1;
    if (!options->uninstall_scope_set) {
        scope_result = installer_select_uninstall_scope(options);
        if (scope_result != 0) return scope_result;
    }
    confirmation = options->remove_jdks
                       ? jvman_lang_str(
                             JVMAN_STR_UNINSTALL_CONFIRM_FINAL_ALL)
                       : options->remove_data
                             ? jvman_lang_str(
                                   JVMAN_STR_UNINSTALL_CONFIRM_FINAL_DATA)
                             : jvman_lang_str(
                                   JVMAN_STR_UNINSTALL_CONFIRM_FINAL);
    if (installer_message_box(
            confirmation, jvman_lang_str(JVMAN_STR_APP_TITLE),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return 1;
    }
    options->uninstall_confirmed = 1;
    return 0;
}

static void installer_copy(wchar_t *out, size_t capacity, const wchar_t *value) {
    size_t length;
    if (!out || capacity == 0) return;
    if (!value) {
        out[0] = L'\0';
        return;
    }
    length = wcslen(value);
    if (length >= capacity) length = capacity - 1u;
    wmemcpy(out, value, length);
    out[length] = L'\0';
}

static int installer_set_field(wchar_t **field, const wchar_t *value) {
    wchar_t *copy;
    size_t length;
    if (!field || !value) return -1;
    length = wcslen(value);
    if (length >= JVMAN_INSTALL_PATH_CHARS) return -1;
    copy = (wchar_t *)malloc((length + 1u) * sizeof(*copy));
    if (!copy) return -1;
    memcpy(copy, value, (length + 1u) * sizeof(*copy));
    free(*field);
    *field = copy;
    return 0;
}

static int installer_join(wchar_t *out, size_t capacity,
                          const wchar_t *left, const wchar_t *right) {
    size_t left_length;
    size_t right_start = 0;
    size_t right_length;
    int separator;
    if (!out || capacity == 0 || !left || !right) return -1;
    left_length = wcslen(left);
    right_length = wcslen(right);
    while (right_start < right_length &&
           (right[right_start] == L'\\' || right[right_start] == L'/')) {
        ++right_start;
    }
    right_length -= right_start;
    separator = left_length != 0 && left[left_length - 1u] != L'\\' &&
                left[left_length - 1u] != L'/';
    if (left_length + (size_t)separator + right_length + 1u > capacity) return -1;
    memcpy(out, left, left_length * sizeof(*out));
    if (separator) out[left_length++] = L'\\';
    memcpy(out + left_length, right + right_start, right_length * sizeof(*out));
    out[left_length + right_length] = L'\0';
    return 0;
}

static int installer_format(wchar_t *out, size_t capacity,
                            const wchar_t *format, ...) {
    va_list args;
    int written;
    if (!out || capacity == 0 || !format) return -1;
    va_start(args, format);
    written = _vsnwprintf(out, capacity, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity) {
        out[0] = L'\0';
        return -1;
    }
    return 0;
}

static int installer_option_equal(const wchar_t *left, const wchar_t *right) {
    return left && right && _wcsicmp(left, right) == 0;
}

static int installer_is_switch(const wchar_t *value, const wchar_t *name) {
    return installer_option_equal(value, name) ||
           (value && value[0] == L'-' && value[1] == L'-' &&
            installer_option_equal(value + 1, name));
}

static int installer_command_append_char(InstallerCommandBuilder *builder,
                                         wchar_t value) {
    if (!builder || !builder->buffer || builder->capacity == 0 ||
        builder->length >= builder->capacity - 1u) {
        return -1;
    }
    builder->buffer[builder->length++] = value;
    builder->buffer[builder->length] = L'\0';
    return 0;
}

static int installer_command_append_repeat(InstallerCommandBuilder *builder,
                                           wchar_t value, size_t count) {
    while (count-- != 0u) {
        if (installer_command_append_char(builder, value) != 0) return -1;
    }
    return 0;
}

/* Encode one argument as the inverse of CommandLineToArgvW. */
static int installer_command_append_argument(InstallerCommandBuilder *builder,
                                             const wchar_t *argument) {
    size_t index = 0u;
    int quote = 0;
    if (!builder || !argument) return -1;
    if (builder->length != 0u &&
        installer_command_append_char(builder, L' ') != 0) return -1;
    if (*argument == L'\0') {
        quote = 1;
    } else {
        for (index = 0u; argument[index] != L'\0'; ++index) {
            if (argument[index] == L' ' || argument[index] == L'\t' ||
                argument[index] == L'\n' || argument[index] == L'\r' ||
                argument[index] == L'\v' ||
                argument[index] == L'"') {
                quote = 1;
                break;
            }
        }
    }
    if (!quote) {
        for (index = 0u; argument[index] != L'\0'; ++index) {
            if (installer_command_append_char(builder, argument[index]) != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (installer_command_append_char(builder, L'"') != 0) return -1;
    index = 0u;
    while (argument[index] != L'\0') {
        size_t slash_count = 0u;
        while (argument[index] == L'\\') {
            ++slash_count;
            ++index;
        }
        if (argument[index] == L'"') {
            if (slash_count > (SIZE_MAX - 1u) / 2u ||
                installer_command_append_repeat(builder, L'\\',
                                                  slash_count * 2u + 1u) != 0 ||
                installer_command_append_char(builder, L'"') != 0) {
                return -1;
            }
            ++index;
        } else if (argument[index] == L'\0') {
            if (slash_count > SIZE_MAX / 2u ||
                installer_command_append_repeat(builder, L'\\',
                                                  slash_count * 2u) != 0) {
                return -1;
            }
        } else {
            if (installer_command_append_repeat(builder, L'\\', slash_count) != 0 ||
                installer_command_append_char(builder, argument[index]) != 0) {
                return -1;
            }
            ++index;
        }
    }
    return installer_command_append_char(builder, L'"');
}

static int installer_command_append_option(InstallerCommandBuilder *builder,
                                           const wchar_t *name,
                                           const wchar_t *value) {
    wchar_t *argument;
    size_t name_length;
    size_t value_length;
    size_t capacity;
    int result;
    if (!builder || !name || !value) return -1;
    name_length = wcslen(name);
    value_length = wcslen(value);
    if (name_length > SIZE_MAX - value_length - 2u) return -1;
    capacity = name_length + value_length + 2u;
    argument = (wchar_t *)malloc(capacity * sizeof(*argument));
    if (!argument) return -1;
    if (installer_format(argument, capacity, L"%s=%s", name, value) != 0) {
        free(argument);
        return -1;
    }
    result = installer_command_append_argument(builder, argument);
    free(argument);
    return result;
}

static HANDLE installer_create_shared_mutex(const wchar_t *name) {
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_ATTRIBUTES security;
    HANDLE mutex;
    if (!name || !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                     L"D:P(A;;GA;;;AU)", SDDL_REVISION_1,
                     &descriptor, NULL)) {
        return NULL;
    }
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;
    mutex = CreateMutexW(&security, FALSE, name);
    LocalFree(descriptor);
    return mutex;
}

static HANDLE installer_acquire_single_instance(int *already_running) {
    HANDLE mutex;
    DWORD wait_result;
    if (!already_running) return NULL;
    *already_running = 0;
    mutex = installer_create_shared_mutex(INSTALLER_MUTEX_NAME);
    if (!mutex) return NULL;
    wait_result = WaitForSingleObject(mutex, 0u);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
        return mutex;
    }
    if (wait_result == WAIT_TIMEOUT) {
        *already_running = 1;
    }
    CloseHandle(mutex);
    return NULL;
}

static HANDLE installer_acquire_machine_instance(
    int wait_for_owner, int *already_running) {
    static const wchar_t lock_security[] =
        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
    JvmanInstallPaths paths;
    wchar_t lock_path[JVMAN_INSTALL_PATH_CHARS];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_ATTRIBUTES security;
    ULONGLONG deadline;
    HANDLE file;
    if (!already_running) return NULL;
    *already_running = 0;
    if (jvman_install_paths_machine_default(&paths) != JVMAN_INSTALL_OK ||
        installer_format(lock_path,
                         sizeof(lock_path) / sizeof(lock_path[0]),
                         L"%s%s", paths.install_dir,
                         INSTALLER_MACHINE_LOCK_SUFFIX) != 0 ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(
            lock_security, SDDL_REVISION_1, &descriptor, NULL)) {
        return NULL;
    }
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    deadline = GetTickCount64() + INSTALLER_MUTEX_HANDOFF_TIMEOUT_MS;
    for (;;) {
        BY_HANDLE_FILE_INFORMATION information;
        DWORD error;
        file = CreateFileW(
            lock_path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, &security,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_DELETE_ON_CLOSE,
            NULL);
        if (file != INVALID_HANDLE_VALUE) {
            if (GetFileInformationByHandle(file, &information) &&
                (information.dwFileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
                    0) {
                LocalFree(descriptor);
                return file;
            }
            CloseHandle(file);
            LocalFree(descriptor);
            return NULL;
        }
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION &&
            error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED) {
            LocalFree(descriptor);
            return NULL;
        }
        if (!wait_for_owner || GetTickCount64() >= deadline) {
            *already_running = 1;
            LocalFree(descriptor);
            return NULL;
        }
        Sleep(25u);
    }
}

static HANDLE installer_acquire_single_instance_after_handoff(
    int *already_running) {
    HANDLE mutex;
    DWORD wait_result;
    if (!already_running) return NULL;
    *already_running = 0;
    mutex = installer_create_shared_mutex(INSTALLER_MUTEX_NAME);
    if (!mutex) return NULL;
    wait_result = WaitForSingleObject(mutex,
                                      INSTALLER_MUTEX_HANDOFF_TIMEOUT_MS);
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
        return mutex;
    }
    if (wait_result == WAIT_TIMEOUT) *already_running = 1;
    CloseHandle(mutex);
    return NULL;
}

static int installer_parse_value(const wchar_t *argument, const wchar_t *name,
                                 wchar_t *out, size_t capacity) {
    size_t name_length;
    const wchar_t *value;
    if (!argument || !name || !out || capacity == 0) return 0;
    name_length = wcslen(name);
    if (_wcsnicmp(argument, name, name_length) != 0 ||
        (argument[name_length] != L'=' && argument[name_length] != L':')) {
        return 0;
    }
    value = argument + name_length + 1u;
    if (!*value || wcslen(value) >= capacity) return -1;
    installer_copy(out, capacity, value);
    return 1;
}

static int installer_parse_language(const wchar_t *value,
                                    JvmanInstallerLang *language_out) {
    if (!value || !language_out) return -1;
    if (_wcsicmp(value, L"en") == 0 || _wcsicmp(value, L"en-US") == 0 ||
        wcscmp(value, L"0") == 0) {
        *language_out = JVMAN_LANG_EN;
        return 0;
    }
    if (_wcsicmp(value, L"zh") == 0 || _wcsicmp(value, L"zh-CN") == 0 ||
        _wcsicmp(value, L"zh_CN") == 0 || wcscmp(value, L"1") == 0) {
        *language_out = JVMAN_LANG_ZH_CN;
        return 0;
    }
    return -1;
}

static const wchar_t *installer_language_tag(void) {
    return jvman_lang_current() == JVMAN_LANG_ZH_CN ? L"zh-CN" : L"en";
}

static int installer_parse_legacy_flags(const wchar_t *value,
                                        unsigned int *flags_out) {
    unsigned int flags;
    if (!value || !flags_out || value[0] < L'1' || value[0] > L'3' ||
        value[1] != L'\0') {
        return -1;
    }
    flags = (unsigned int)(value[0] - L'0');
    if ((flags & ~(INSTALLER_LEGACY_APP_PATH |
                   INSTALLER_LEGACY_JAVA_PATH)) != 0u) {
        return -1;
    }
    *flags_out = flags;
    return 0;
}

static int installer_parse_options(int argc, wchar_t **argv,
                                   InstallerOptions *options) {
    int index;
    if (!options) return -1;
    memset(options, 0, sizeof(*options));
    options->add_path = 1;
    options->path_scope = JVMAN_ENV_SCOPE_USER;
    options->configure_java = 0;
    options->relocate_legacy_java = 1;   /* opt-out: default enabled */
    if (argc < 1 || argc > INSTALLER_MAX_ARGS || !argv) return -1;
    for (index = 1; index < argc; ++index) {
        const wchar_t *argument = argv[index];
        wchar_t value[JVMAN_INSTALL_PATH_CHARS];
        int parsed;
        if (!argument || wcslen(argument) >= JVMAN_INSTALL_PATH_CHARS) return -1;
        if (installer_is_switch(argument, INSTALLER_ELEVATED_RESUME_SWITCH)) {
            if (options->elevated_resume) return -1;
            options->elevated_resume = 1;
        } else if (installer_is_switch(argument, L"/S") ||
            installer_is_switch(argument, L"/SILENT") ||
            installer_is_switch(argument, L"/QUIET") ||
            installer_is_switch(argument, L"--quiet")) {
            if (options->silent) return -1;
            options->silent = 1;
        } else if (installer_is_switch(argument, L"/PORTABLE") ||
                   installer_is_switch(argument, L"--portable")) {
            if (options->portable) return -1;
            options->portable = 1;
        } else if (installer_is_switch(argument, L"/UNINSTALL") ||
                   installer_is_switch(argument, L"--uninstall")) {
            if (options->uninstall) return -1;
            options->uninstall = 1;
        } else if (installer_is_switch(argument, L"/REMOVE_DATA") ||
                   installer_is_switch(argument, L"--remove-data")) {
            if (options->remove_data) return -1;
            options->remove_data = 1;
            options->uninstall_scope_set = 1;
        } else if (installer_is_switch(argument, L"/REMOVE_JDKS") ||
                   installer_is_switch(argument, L"--remove-jdks")) {
            if (options->remove_jdks) return -1;
            options->remove_jdks = 1;
            options->uninstall_scope_set = 1;
        } else if (installer_is_switch(argument, L"/ADD_TO_PATH") ||
                   installer_is_switch(argument, L"--add-to-path")) {
            if (options->add_path != 1) return -1;
            options->add_path = 1;
        } else if (installer_is_switch(argument, L"/NO_PATH") ||
                   installer_is_switch(argument, L"/NO_ADD_TO_PATH") ||
                   installer_is_switch(argument, L"--no-add-to-path")) {
            if (options->add_path == 0) return -1;
            options->add_path = 0;
        } else if (installer_is_switch(argument, L"/USER_PATH") ||
                   installer_is_switch(argument, L"/CURRENT_USER_PATH") ||
                   installer_is_switch(argument, L"--user-path") ||
                   installer_is_switch(argument, L"--current-user-path")) {
            if (options->path_scope_set &&
                options->path_scope != JVMAN_ENV_SCOPE_USER) return -1;
            options->path_scope = JVMAN_ENV_SCOPE_USER;
            options->path_scope_set = 1;
        } else if (installer_is_switch(argument, L"/SYSTEM_PATH") ||
                   installer_is_switch(argument, L"/MACHINE_PATH") ||
                   installer_is_switch(argument, L"/ALL_USERS_PATH") ||
                   installer_is_switch(argument, L"--system-path") ||
                   installer_is_switch(argument, L"--machine-path") ||
                   installer_is_switch(argument, L"--all-users-path")) {
            if (options->path_scope_set &&
                options->path_scope != JVMAN_ENV_SCOPE_MACHINE) return -1;
            options->path_scope = JVMAN_ENV_SCOPE_MACHINE;
            options->path_scope_set = 1;
            options->machine_mode = 1;
        } else if (installer_is_switch(argument, L"/MACHINE") ||
                   installer_is_switch(argument, L"/ALL_USERS")) {
            if (options->machine_mode) return -1;
            options->machine_mode = 1;
        } else if (installer_is_switch(argument, L"/CONFIGURE_JAVA") ||
                   installer_is_switch(argument, L"--configure-java")) {
            if (options->configure_java != 0) return -1;
            options->configure_java = 1;
        } else if (installer_is_switch(argument, L"/NO_CONFIGURE_JAVA") ||
                   installer_is_switch(argument, L"--no-configure-java")) {
            if (options->configure_java != 0) return -1;
            options->configure_java = -1;
        } else if (installer_is_switch(argument, L"/REPLACE_JAVA_HOME") ||
                   installer_is_switch(argument, L"--replace-java-home")) {
            if (options->replace_java_home) return -1;
            options->replace_java_home = 1;
        } else if (installer_is_switch(argument, L"/DISCOVER") ||
                   installer_is_switch(argument, L"--discover")) {
            if (options->discover) return -1;
            options->discover = 1;
        } else if (installer_is_switch(
                       argument, INSTALLER_LEGACY_CLEANUP_ONLY_SWITCH)) {
            if (options->legacy_cleanup_only) return -1;
            options->legacy_cleanup_only = 1;
        } else if (installer_is_switch(
                       argument, INSTALLER_LEGACY_RESTORE_ONLY_SWITCH)) {
            if (options->legacy_restore_only) return -1;
            options->legacy_restore_only = 1;
        } else if (installer_is_switch(argument, L"/RELOCATE_LEGACY_JAVA_PATH") ||
                   installer_is_switch(argument, L"--relocate-legacy-java-path")) {
            if (options->relocate_legacy_java != 1) return -1;
            /* already the default; keep 1 for symmetry */
            options->relocate_legacy_java = 1;
        } else if (installer_is_switch(argument, L"/NO_RELOCATE_LEGACY_JAVA_PATH") ||
                   installer_is_switch(argument, L"--no-relocate-legacy-java-path")) {
            if (options->relocate_legacy_java == -1) return -1;
            options->relocate_legacy_java = -1;
        } else if (installer_is_switch(
                       argument, INSTALLER_JAVA_HKLM_RELOCATE_SWITCH)) {
            if (options->java_hklm_relocate_only) return -1;
            options->java_hklm_relocate_only = 1;
        } else if (installer_is_switch(
                       argument, INSTALLER_JAVA_HKLM_RESTORE_SWITCH)) {
            if (options->java_hklm_restore_only) return -1;
            options->java_hklm_restore_only = 1;
        } else if (installer_is_switch(
                       argument, INSTALLER_UNINSTALL_CONFIRMED_SWITCH)) {
            if (options->uninstall_confirmed) return -1;
            options->uninstall_confirmed = 1;
        } else if (installer_is_switch(argument, L"/HELP") ||
                   installer_is_switch(argument, L"--help") ||
                   installer_is_switch(argument, L"/?")) {
            return 1;
        } else {
            parsed = installer_parse_value(
                argument, INSTALLER_INTERNAL_LANG_OPTION, value,
                sizeof(value) / sizeof(*value));
            if (parsed < 0 || (parsed && options->language_set) ||
                (parsed && installer_parse_language(value,
                                                     &options->language) != 0)) {
                return -1;
            }
            if (parsed) {
                options->language_set = 1;
                continue;
            }
            parsed = installer_parse_value(
                argument, INSTALLER_PUBLIC_LANG_OPTION, value,
                sizeof(value) / sizeof(*value));
            if (!parsed) {
                parsed = installer_parse_value(
                    argument, L"--lang", value,
                    sizeof(value) / sizeof(*value));
            }
            if (!parsed) {
                parsed = installer_parse_value(
                    argument, L"--language", value,
                    sizeof(value) / sizeof(*value));
            }
            if (parsed < 0 || (parsed && options->language_set) ||
                (parsed && installer_parse_language(value,
                                                     &options->language) != 0)) {
                return -1;
            }
            if (parsed) {
                options->language_set = 1;
                continue;
            }
            parsed = installer_parse_value(
                argument, INSTALLER_LEGACY_INSTALL_OPTION, value,
                sizeof(value) / sizeof(*value));
            if (parsed < 0 ||
                (parsed && options->legacy_install_dir[0] != L'\0')) {
                return -1;
            }
            if (parsed) {
                installer_copy(options->legacy_install_dir,
                               sizeof(options->legacy_install_dir) /
                                   sizeof(options->legacy_install_dir[0]),
                               value);
                continue;
            }
            parsed = installer_parse_value(
                argument, INSTALLER_LEGACY_DATA_OPTION, value,
                sizeof(value) / sizeof(*value));
            if (parsed < 0 ||
                (parsed && options->legacy_data_home[0] != L'\0')) {
                return -1;
            }
            if (parsed) {
                installer_copy(options->legacy_data_home,
                               sizeof(options->legacy_data_home) /
                                   sizeof(options->legacy_data_home[0]),
                               value);
                continue;
            }
            parsed = installer_parse_value(
                argument, INSTALLER_LEGACY_ID_OPTION, value,
                sizeof(value) / sizeof(*value));
            if (parsed < 0 ||
                (parsed && (options->legacy_install_id[0] != L'\0' ||
                            wcslen(value) >=
                                sizeof(options->legacy_install_id) /
                                    sizeof(options->legacy_install_id[0])))) {
                return -1;
            }
            if (parsed) {
                installer_copy(options->legacy_install_id,
                               sizeof(options->legacy_install_id) /
                                   sizeof(options->legacy_install_id[0]),
                               value);
                continue;
            }
            parsed = installer_parse_value(
                argument, INSTALLER_LEGACY_FLAGS_OPTION, value,
                sizeof(value) / sizeof(*value));
            if (parsed < 0 || (parsed && options->legacy_cleanup_flags != 0u) ||
                (parsed && installer_parse_legacy_flags(
                               value, &options->legacy_cleanup_flags) != 0)) {
                return -1;
            }
            if (parsed) continue;
            parsed = installer_parse_value(argument, L"/DIR", value, sizeof(value) / sizeof(*value));
            if (!parsed) parsed = installer_parse_value(argument, L"/INSTALL-DIR", value,
                                                         sizeof(value) / sizeof(*value));
            if (!parsed) parsed = installer_parse_value(argument, L"--dir", value,
                                                         sizeof(value) / sizeof(*value));
            if (!parsed) parsed = installer_parse_value(argument, L"--install-dir", value,
                                                         sizeof(value) / sizeof(*value));
            if (parsed < 0 || (parsed && options->install_dir_set)) return -1;
            if (parsed) {
                installer_copy(options->install_dir,
                               sizeof(options->install_dir) / sizeof(*options->install_dir),
                               value);
                options->install_dir_set = 1;
            } else if (installer_is_switch(argument, L"/DIR") ||
                       installer_is_switch(argument, L"/INSTALL-DIR") ||
                       installer_is_switch(argument, L"--dir") ||
                       installer_is_switch(argument, L"--install-dir")) {
                if (++index >= argc || !argv[index] ||
                    wcslen(argv[index]) >= sizeof(options->install_dir) /
                                             sizeof(*options->install_dir) ||
                    options->install_dir_set) return -1;
                installer_copy(options->install_dir,
                               sizeof(options->install_dir) / sizeof(*options->install_dir),
                               argv[index]);
                options->install_dir_set = 1;
            } else {
                return -1;
            }
        }
    }
    if (options->portable && (options->uninstall || options->configure_java ||
                              options->discover || options->path_scope_set ||
                              options->machine_mode || options->elevated_resume ||
                              options->remove_data || options->remove_jdks ||
                              options->legacy_cleanup_only ||
                              options->legacy_cleanup_flags != 0u ||
                              !options->install_dir_set)) {
        return -1;
    }
    if (options->uninstall && (options->portable || options->install_dir_set ||
                               options->add_path == 0 || options->configure_java != 0 ||
                               options->replace_java_home || options->discover ||
                               options->path_scope_set)) {
        return -1;
    }
    if ((options->remove_data || options->remove_jdks) &&
        !options->uninstall) {
        return -1;
    }
    if (options->remove_jdks && !options->remove_data) return -1;
    if (options->machine_mode && options->uninstall &&
        (options->remove_data || options->remove_jdks)) {
        return -1;
    }
    if (!options->add_path && options->path_scope_set) return -1;
    if (options->replace_java_home && options->configure_java != 1) return -1;
    if (options->machine_mode && !options->uninstall) {
        if (options->configure_java == -1) options->configure_java = 0;
        if (!options->add_path ||
            (options->path_scope_set &&
             options->path_scope != JVMAN_ENV_SCOPE_MACHINE) ||
            options->install_dir_set || options->configure_java != 0 ||
            options->replace_java_home || options->discover) {
            return -1;
        }
        options->path_scope = JVMAN_ENV_SCOPE_MACHINE;
        options->path_scope_set = 1;
    }
    if (options->elevated_resume &&
        ((!options->machine_mode && !options->legacy_cleanup_only &&
          !options->legacy_restore_only) ||
         !options->language_set)) {
        return -1;
    }
    if (options->legacy_cleanup_flags != 0u) {
        if (!options->elevated_resume ||
            options->legacy_install_dir[0] == L'\0' ||
            options->legacy_data_home[0] == L'\0' ||
            options->legacy_install_id[0] == L'\0') {
            return -1;
        }
    } else if (options->legacy_cleanup_only || options->legacy_restore_only ||
               options->legacy_install_dir[0] != L'\0' ||
               options->legacy_data_home[0] != L'\0' ||
               options->legacy_install_id[0] != L'\0') {
        return -1;
    }
    if ((options->legacy_cleanup_only || options->legacy_restore_only) &&
        (options->machine_mode || options->uninstall || options->portable ||
         options->install_dir_set || options->configure_java != 0 ||
         options->replace_java_home || options->discover ||
          options->path_scope_set ||
          options->legacy_cleanup_only == options->legacy_restore_only)) {
        return -1;
    }
    if (options->uninstall_confirmed &&
        (!options->elevated_resume || !options->uninstall)) {
        return -1;
    }
    return 0;
}

static void installer_show_usage(void) {
    installer_message_box(
        jvman_lang_str(JVMAN_STR_USAGE),
        jvman_lang_str(JVMAN_STR_APP_TITLE), MB_OK | MB_ICONINFORMATION);
}

static int installer_make_id(wchar_t *out, size_t capacity) {
    GUID guid;
    wchar_t text[64];
    int length;
    if (!out || capacity < 2 || CoCreateGuid(&guid) != S_OK) return -1;
    length = StringFromGUID2(&guid, text, (int)(sizeof(text) / sizeof(*text)));
    if (length < 4 || (size_t)(length - 3) >= capacity) return -1;
    /* Drop the braces; the marker format intentionally remains simple ASCII. */
    memcpy(out, text + 1, (size_t)(length - 3) * sizeof(*out));
    out[length - 3] = L'\0';
    return 0;
}

static int installer_set_metadata_defaults(JvmanInstallerMetadata *metadata,
                                            const JvmanInstallPaths *paths,
                                            int existing) {
    if (!metadata || !paths) return -1;
    if (!existing) {
        if (installer_set_field(&metadata->version, JVMAN_VERSION_W) != 0 ||
            installer_set_field(&metadata->install_dir, paths->install_dir) != 0 ||
            installer_set_field(&metadata->data_home, paths->data_home) != 0) {
            return -1;
        }
        if (!metadata->install_id) {
            wchar_t id[JVMAN_INSTALL_MARKER_ID_CHARS];
            if (installer_make_id(id, sizeof(id) / sizeof(*id)) != 0 ||
                installer_set_field(&metadata->install_id, id) != 0) return -1;
        }
    }
    return 0;
}

static int installer_metadata_matches(const JvmanInstallerMetadata *metadata,
                                      const JvmanInstallPaths *paths) {
    return metadata && paths && metadata->install_dir && metadata->data_home &&
           _wcsicmp(metadata->install_dir, paths->install_dir) == 0 &&
           _wcsicmp(metadata->data_home, paths->data_home) == 0 &&
           metadata->install_id && *metadata->install_id;
}

static int installer_report(const wchar_t *title, const wchar_t *message,
                            UINT type, int silent) {
    if (!silent) installer_message_box(message, title, type);
    return 1;
}

static int installer_report_status(const wchar_t *title, const wchar_t *prefix,
                                   const wchar_t *detail, int silent) {
    wchar_t message[INSTALLER_TEXT_MAX];
    if (installer_format(message, sizeof(message) / sizeof(*message),
                         L"%s\n\n%s", prefix, detail ? detail : L"unknown error") != 0) {
        installer_copy(message, sizeof(message) / sizeof(*message), prefix);
    }
    return installer_report(title, message, MB_OK | MB_ICONERROR, silent);
}

static int installer_registered_uninstaller_matches(
    const wchar_t *module, const JvmanInstallerMetadata *metadata,
    JvmanInstallPaths *paths_out) {
    JvmanInstallPaths paths;
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    if (!module || !metadata || !metadata->install_dir ||
        !metadata->data_home ||
        jvman_install_paths_init(&paths, metadata->install_dir,
                                 metadata->data_home) != JVMAN_INSTALL_OK ||
        _wcsicmp(module, paths.uninstall_path) != 0) {
        return 0;
    }
    if (!metadata->install_id || !*metadata->install_id ||
        jvman_install_marker_read(
            &paths, marker,
            sizeof(marker) / sizeof(marker[0])) != JVMAN_INSTALL_OK ||
        _wcsicmp(marker, metadata->install_id) != 0) {
        return -1;
    }
    if (paths_out) *paths_out = paths;
    return 1;
}

/* A copied setup bundle becomes an uninstaller only when its canonical
 * registered path and installation marker both match persisted metadata. */
static int installer_detect_registered_uninstaller(
    int argc, InstallerOptions *options) {
    JvmanInstallerMetadata metadata;
    JvmanInstallPaths expected_machine_paths;
    JvmanInstallPaths matched_paths;
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    DWORD module_length;
    DWORD attributes;
    int found = 0;
    int matched = 0;
    JvmanEnvironmentStatus env_status;
    if (!options || (argc != 1 && !(argc == 2 && options->silent))) return 0;
    module_length = GetModuleFileNameW(
        NULL, module, (DWORD)(sizeof(module) / sizeof(module[0])));
    if (module_length == 0 ||
        module_length >= sizeof(module) / sizeof(module[0])) {
        return 0;
    }
    attributes = GetFileAttributesW(module);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return 0;
    }

    /* The canonical machine uninstaller must authenticate only against HKLM.
     * Never let current-user metadata downgrade an elevated uninstall. */
    if (jvman_install_paths_machine_default(&expected_machine_paths) ==
            JVMAN_INSTALL_OK &&
        _wcsicmp(module, expected_machine_paths.uninstall_path) == 0) {
        jvman_installer_metadata_init(&metadata);
        env_status = jvman_installer_metadata_load_scoped(
            JVMAN_ENV_SCOPE_MACHINE, &metadata, &found);
        if (env_status == JVMAN_ENV_OK && found) {
            matched = installer_registered_uninstaller_matches(
                module, &metadata, &matched_paths);
            if (matched > 0 &&
                (metadata.java_home_owned || metadata.java_path_owned ||
                 metadata.app_path_scope !=
                     (uint32_t)JVMAN_ENV_SCOPE_MACHINE ||
                 _wcsicmp(matched_paths.install_dir,
                          expected_machine_paths.install_dir) != 0 ||
                 _wcsicmp(matched_paths.data_home,
                          expected_machine_paths.data_home) != 0)) {
                matched = -1;
            }
        }
        jvman_installer_metadata_free(&metadata);
        if (matched <= 0) return -1;
        options->uninstall = 1;
        options->machine_mode = 1;
        return 1;
    }

    jvman_installer_metadata_init(&metadata);
    env_status = jvman_installer_metadata_load(&metadata, &found);
    if (env_status == JVMAN_ENV_OK && found) {
        matched = installer_registered_uninstaller_matches(
            module, &metadata, NULL);
    }
    jvman_installer_metadata_free(&metadata);
    if (matched < 0) return -1;
    if (matched > 0) {
        options->uninstall = 1;
        return 1;
    }
    return 0;
}

static int installer_process_is_elevated(void) {
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    int result = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    memset(&elevation, 0, sizeof(elevation));
    if (GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &size) &&
        size == sizeof(elevation) && elevation.TokenIsElevated) {
        result = 1;
    }
    CloseHandle(token);
    return result;
}

static void installer_source_lock_init(InstallerSourceLock *lock) {
    size_t index;
    if (!lock) return;
    lock->file = INVALID_HANDLE_VALUE;
    lock->directory_count = 0u;
    for (index = 0u; index < INSTALLER_SOURCE_COMPONENT_LIMIT; ++index) {
        lock->directories[index] = INVALID_HANDLE_VALUE;
    }
}

static void installer_source_lock_release(InstallerSourceLock *lock) {
    if (!lock) return;
    if (lock->file != INVALID_HANDLE_VALUE) {
        CloseHandle(lock->file);
        lock->file = INVALID_HANDLE_VALUE;
    }
    while (lock->directory_count != 0u) {
        HANDLE directory = lock->directories[--lock->directory_count];
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        lock->directories[lock->directory_count] = INVALID_HANDLE_VALUE;
    }
}

/* Keep every directory component and the setup image open without delete
 * sharing. This prevents a user-writable download path or junction from being
 * switched between the UAC prompt and the elevated child opening the bundle. */
static int installer_source_lock_acquire(InstallerSourceLock *lock) {
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    wchar_t root[4];
    DWORD length;
    size_t index;
    BY_HANDLE_FILE_INFORMATION information;
    if (!lock) return -1;
    installer_source_lock_init(lock);
    length = GetModuleFileNameW(
        NULL, module, (DWORD)(sizeof(module) / sizeof(module[0])));
    if (length < 4u || length >= sizeof(module) / sizeof(module[0]) ||
        module[1] != L':' || (module[2] != L'\\' && module[2] != L'/')) {
        return -1;
    }
    for (index = 0u; index < (size_t)length; ++index) {
        if (module[index] == L'/') module[index] = L'\\';
    }
    root[0] = module[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
    if (GetDriveTypeW(root) != DRIVE_FIXED) return -1;
    for (index = 3u; index < (size_t)length; ++index) {
        HANDLE directory;
        wchar_t saved;
        if (module[index] != L'\\') continue;
        if (lock->directory_count >= INSTALLER_SOURCE_COMPONENT_LIMIT) {
            installer_source_lock_release(lock);
            return -1;
        }
        saved = module[index];
        module[index] = L'\0';
        directory = CreateFileW(
            module, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        module[index] = saved;
        if (directory == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(directory, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
            installer_source_lock_release(lock);
            return -1;
        }
        lock->directories[lock->directory_count++] = directory;
    }
    lock->file = CreateFileW(
        module, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        NULL);
    if (lock->file == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(lock->file, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        (information.nFileSizeHigh == 0u && information.nFileSizeLow == 0u)) {
        installer_source_lock_release(lock);
        return -1;
    }
    return 0;
}

static int installer_get_trusted_self_path(
    wchar_t module[], size_t module_capacity,
    wchar_t directory[], size_t directory_capacity) {
    wchar_t root[4];
    wchar_t *separator;
    DWORD length;
    DWORD attributes;
    if (!module || module_capacity < 4u || !directory ||
        directory_capacity < 4u || module_capacity > UINT32_MAX) {
        return -1;
    }
    length = GetModuleFileNameW(NULL, module, (DWORD)module_capacity);
    if (length < 4u || length >= module_capacity || module[1] != L':' ||
        (module[2] != L'\\' && module[2] != L'/')) {
        return -1;
    }
    root[0] = module[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
    if (GetDriveTypeW(root) != DRIVE_FIXED) return -1;
    attributes = GetFileAttributesW(module);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return -1;
    }
    separator = wcsrchr(module, L'\\');
    if (!separator) separator = wcsrchr(module, L'/');
    if (!separator || separator == module ||
        (size_t)(separator - module) >= directory_capacity) {
        return -1;
    }
    memcpy(directory, module,
           (size_t)(separator - module) * sizeof(*directory));
    directory[separator - module] = L'\0';
    return 0;
}

static int installer_build_elevated_parameters(
    const InstallerOptions *options, wchar_t *command, size_t capacity) {
    InstallerCommandBuilder builder;
    if (!options || !command || capacity == 0u || options->elevated_resume ||
        (!options->machine_mode && !options->legacy_cleanup_only &&
         !options->legacy_restore_only)) {
        return -1;
    }
    builder.buffer = command;
    builder.capacity = capacity;
    builder.length = 0u;
    command[0] = L'\0';
    if (installer_command_append_argument(
            &builder, INSTALLER_ELEVATED_RESUME_SWITCH) != 0 ||
        installer_command_append_option(
            &builder, INSTALLER_INTERNAL_LANG_OPTION,
            installer_language_tag()) != 0) {
        return -1;
    }
    if (options->silent &&
        installer_command_append_argument(&builder, L"/S") != 0) {
        return -1;
    }
    if (options->legacy_cleanup_flags != 0u) {
        wchar_t flags[16];
        if (installer_format(flags, sizeof(flags) / sizeof(flags[0]), L"%u",
                             options->legacy_cleanup_flags) != 0 ||
            installer_command_append_option(
                &builder, INSTALLER_LEGACY_INSTALL_OPTION,
                options->legacy_install_dir) != 0 ||
            installer_command_append_option(
                &builder, INSTALLER_LEGACY_DATA_OPTION,
                options->legacy_data_home) != 0 ||
            installer_command_append_option(
                &builder, INSTALLER_LEGACY_ID_OPTION,
                options->legacy_install_id) != 0 ||
            installer_command_append_option(
                &builder, INSTALLER_LEGACY_FLAGS_OPTION, flags) != 0) {
            return -1;
        }
    }
    if (options->legacy_cleanup_only) {
        return installer_command_append_argument(
            &builder, INSTALLER_LEGACY_CLEANUP_ONLY_SWITCH);
    }
    if (options->legacy_restore_only) {
        return installer_command_append_argument(
            &builder, INSTALLER_LEGACY_RESTORE_ONLY_SWITCH);
    }
    if (options->uninstall) {
        if (installer_command_append_argument(&builder, L"/UNINSTALL") != 0 ||
            installer_command_append_argument(&builder, L"/MACHINE") != 0) {
            return -1;
        }
        if (options->uninstall_confirmed &&
            installer_command_append_argument(
                &builder, INSTALLER_UNINSTALL_CONFIRMED_SWITCH) != 0) {
            return -1;
        }
        if (options->remove_data &&
            installer_command_append_argument(&builder, L"/REMOVE_DATA") != 0) {
            return -1;
        }
        if (options->remove_jdks &&
            installer_command_append_argument(&builder, L"/REMOVE_JDKS") != 0) {
            return -1;
        }
    } else if (installer_command_append_argument(&builder, L"/SYSTEM_PATH") !=
               0) {
        return -1;
    }
    return 0;
}

/*
 * 0.5.0: spawn a narrow elevated helper that only executes the HKLM legacy
 * Java PATH relocation (is_restore=0) or its inverse (is_restore=1). No
 * install-state parameters are transported; the helper reads HKLM/HKCU
 * directly. Returns 0 on success, non-zero on failure or user cancel.
 */
static int installer_spawn_java_hklm_helper(int is_restore, int silent) {
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    wchar_t directory[JVMAN_INSTALL_PATH_CHARS];
    wchar_t parameters[128];
    SHELLEXECUTEINFOW execute;
    DWORD wait_result;
    DWORD exit_code = 1u;
    if (installer_get_trusted_self_path(
            module, sizeof(module) / sizeof(module[0]), directory,
            sizeof(directory) / sizeof(directory[0])) != 0) {
        return -1;
    }
    if (_snwprintf_s(parameters, sizeof(parameters) / sizeof(parameters[0]),
                     _TRUNCATE, L"%s",
                     is_restore ? INSTALLER_JAVA_HKLM_RESTORE_SWITCH
                                : INSTALLER_JAVA_HKLM_RELOCATE_SWITCH) < 0) {
        return -1;
    }
    memset(&execute, 0, sizeof(execute));
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = module;
    execute.lpParameters = parameters;
    execute.lpDirectory = directory;
    execute.nShow = silent ? SW_HIDE : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) return -1;
    if (!execute.hProcess) return -1;
    wait_result = WaitForSingleObject(execute.hProcess, INFINITE);
    if (wait_result == WAIT_OBJECT_0 &&
        GetExitCodeProcess(execute.hProcess, &exit_code)) {
        CloseHandle(execute.hProcess);
        return exit_code == 0 ? 0 : -1;
    }
    CloseHandle(execute.hProcess);
    return -1;
}

static int installer_run_elevated_machine(
    const InstallerOptions *options, HANDLE *instance_mutex,
    int *cancelled_out, int *launched_out, int *legacy_cleanup_out,
    unsigned int *legacy_removed_out) {
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    wchar_t directory[JVMAN_INSTALL_PATH_CHARS];
    wchar_t *parameters = NULL;
    SHELLEXECUTEINFOW execute;
    DWORD wait_result;
    DWORD exit_code = 1u;
    int result = 1;
    if (!options || !instance_mutex || !*instance_mutex || !cancelled_out ||
        !launched_out || !legacy_cleanup_out || !legacy_removed_out) {
        return 1;
    }
    *cancelled_out = 0;
    *launched_out = 0;
    *legacy_cleanup_out = 0;
    *legacy_removed_out = 0u;
    parameters = (wchar_t *)calloc(INSTALLER_COMMAND_CHARS,
                                   sizeof(*parameters));
    if (!parameters ||
        installer_get_trusted_self_path(
            module, sizeof(module) / sizeof(module[0]), directory,
            sizeof(directory) / sizeof(directory[0])) != 0 ||
        installer_build_elevated_parameters(
            options, parameters, INSTALLER_COMMAND_CHARS) != 0) {
        free(parameters);
        return 1;
    }
    memset(&execute, 0, sizeof(execute));
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = module;
    execute.lpParameters = parameters;
    execute.lpDirectory = directory;
    execute.nShow = options->silent ? SW_HIDE : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        if (GetLastError() == ERROR_CANCELLED) *cancelled_out = 1;
        free(parameters);
        return 1;
    }
    free(parameters);
    if (!execute.hProcess) return 1;
    *launched_out = 1;

    /* The elevated child recognizes its private resume flag and waits for
     * this mutex to disappear. Release ownership only after creation has
     * succeeded so a cancelled UAC prompt leaves the parent serialized. */
    ReleaseMutex(*instance_mutex);
    CloseHandle(*instance_mutex);
    *instance_mutex = NULL;
    wait_result = WaitForSingleObject(execute.hProcess, INFINITE);
    if (wait_result == WAIT_OBJECT_0 &&
        GetExitCodeProcess(execute.hProcess, &exit_code) && exit_code <= 255u) {
        if (exit_code >= INSTALLER_LEGACY_CLEANUP_EXIT_BASE &&
            exit_code < INSTALLER_LEGACY_CLEANUP_EXIT_BASE +
                            ((INSTALLER_LEGACY_PATH_MASK + 1u) <<
                             INSTALLER_LEGACY_CLEANUP_RESULT_BITS)) {
            DWORD encoded = exit_code - INSTALLER_LEGACY_CLEANUP_EXIT_BASE;
            *legacy_cleanup_out = 1;
            *legacy_removed_out =
                (unsigned int)(encoded >>
                               INSTALLER_LEGACY_CLEANUP_RESULT_BITS) &
                INSTALLER_LEGACY_PATH_MASK;
            exit_code = encoded & INSTALLER_LEGACY_CLEANUP_RESULT_MASK;
        }
        result = (int)exit_code;
    }
    CloseHandle(execute.hProcess);
    return result;
}

static int installer_parse_process_id(const wchar_t *text, DWORD *value_out) {
    uint64_t value = 0;
    size_t index;
    if (!text || !*text || !value_out) return -1;
    for (index = 0; text[index] != L'\0'; ++index) {
        unsigned int digit;
        if (index >= 10u || text[index] < L'0' || text[index] > L'9') return -1;
        digit = (unsigned int)(text[index] - L'0');
        if (value > (UINT32_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    if (value == 0 || value == GetCurrentProcessId()) return -1;
    *value_out = (DWORD)value;
    return 0;
}

static int installer_create_cleanup_copy(
    const JvmanInstallPaths *paths, JvmanEnvironmentScope metadata_scope,
    wchar_t path[], size_t path_capacity) {
    JvmanInstallPaths expected_machine_paths;
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    wchar_t base_directory[JVMAN_INSTALL_PATH_CHARS];
    wchar_t filename[96];
    HANDLE source = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    uint64_t remaining;
    unsigned char buffer[INSTALLER_COPY_BUFFER_SIZE];
    DWORD length;
    DWORD attributes;
    unsigned int attempt;
    if (!paths || !path || path_capacity == 0 ||
        (metadata_scope != JVMAN_ENV_SCOPE_USER &&
         metadata_scope != JVMAN_ENV_SCOPE_MACHINE)) {
        return -1;
    }
    length = GetModuleFileNameW(NULL, module,
                                (DWORD)(sizeof(module) / sizeof(module[0])));
    if (length == 0 || length >= sizeof(module) / sizeof(module[0])) return -1;
    if (metadata_scope == JVMAN_ENV_SCOPE_MACHINE) {
        if (jvman_install_paths_machine_default(&expected_machine_paths) !=
                JVMAN_INSTALL_OK ||
            _wcsicmp(paths->install_dir,
                     expected_machine_paths.install_dir) != 0 ||
            _wcsicmp(paths->data_home, expected_machine_paths.data_home) != 0) {
            return -1;
        }
        attributes = GetFileAttributesW(paths->install_dir);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                           FILE_ATTRIBUTE_REPARSE_POINT)) !=
                FILE_ATTRIBUTE_DIRECTORY) {
            return -1;
        }
        installer_copy(base_directory,
                       sizeof(base_directory) / sizeof(base_directory[0]),
                       paths->install_dir);
    } else {
        length = GetTempPathW(
            (DWORD)(sizeof(base_directory) / sizeof(base_directory[0])),
            base_directory);
        if (length == 0 ||
            length >= sizeof(base_directory) / sizeof(base_directory[0])) {
            return -1;
        }
    }
    source = CreateFileW(module, GENERIC_READ,
                         FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (source == INVALID_HANDLE_VALUE || !GetFileSizeEx(source, &size) ||
        size.QuadPart <= 0 ||
        (uint64_t)size.QuadPart >
            (uint64_t)JVMAN_SETUP_PAYLOAD_LIMIT + (16u * 1024u * 1024u)) {
        if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
        return -1;
    }
    for (attempt = 0; attempt < INSTALLER_TEMP_ATTEMPTS; ++attempt) {
        if (installer_format(filename, sizeof(filename) / sizeof(filename[0]),
                             L"jvman-cleanup-%08lx-%08lx-%03u.exe",
                             (unsigned long)GetCurrentProcessId(),
                             (unsigned long)GetTickCount(), attempt) != 0 ||
            installer_join(path, path_capacity, base_directory, filename) != 0) {
            CloseHandle(source);
            return -1;
        }
        output = CreateFileW(
            path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (output != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            CloseHandle(source);
            return -1;
        }
    }
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(source);
        return -1;
    }
    remaining = (uint64_t)size.QuadPart;
    while (remaining != 0) {
        DWORD requested = remaining > sizeof(buffer) ? (DWORD)sizeof(buffer) :
                                                        (DWORD)remaining;
        DWORD read_count = 0;
        DWORD written = 0;
        if (!ReadFile(source, buffer, requested, &read_count, NULL) ||
            read_count == 0 ||
            !WriteFile(output, buffer, read_count, &written, NULL) ||
            written != read_count) {
            CloseHandle(source);
            CloseHandle(output);
            DeleteFileW(path);
            return -1;
        }
        remaining -= read_count;
    }
    CloseHandle(source);
    if (!FlushFileBuffers(output)) {
        CloseHandle(output);
        DeleteFileW(path);
        return -1;
    }
    if (!CloseHandle(output)) {
        DeleteFileW(path);
        return -1;
    }
    return 0;
}

static int installer_start_self_cleanup(const JvmanInstallPaths *paths,
                                        const wchar_t *installation_id,
                                        JvmanEnvironmentScope metadata_scope) {
    wchar_t helper[JVMAN_INSTALL_PATH_CHARS];
    wchar_t process_id[32];
    const wchar_t *scope_argument;
    wchar_t *command = NULL;
    InstallerCommandBuilder builder;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    int result = -1;
    if (!paths || !installation_id ||
        (metadata_scope != JVMAN_ENV_SCOPE_USER &&
         metadata_scope != JVMAN_ENV_SCOPE_MACHINE) ||
        installer_create_cleanup_copy(paths, metadata_scope, helper,
            sizeof(helper) / sizeof(helper[0])) != 0) {
        return -1;
    }
    scope_argument = metadata_scope == JVMAN_ENV_SCOPE_MACHINE ? L"1" : L"0";
    command = (wchar_t *)malloc(INSTALLER_COMMAND_CHARS * sizeof(*command));
    if (!command || installer_format(
            process_id, sizeof(process_id) / sizeof(process_id[0]), L"%lu",
            (unsigned long)GetCurrentProcessId()) != 0) {
        free(command);
        DeleteFileW(helper);
        return -1;
    }
    builder.buffer = command;
    builder.capacity = INSTALLER_COMMAND_CHARS;
    builder.length = 0u;
    command[0] = L'\0';
    if (installer_command_append_argument(&builder, helper) != 0 ||
        installer_command_append_argument(&builder, L"/CLEANUP") != 0 ||
        installer_command_append_argument(&builder, process_id) != 0 ||
        installer_command_append_argument(&builder, paths->install_dir) != 0 ||
        installer_command_append_argument(&builder, paths->data_home) != 0 ||
        installer_command_append_argument(&builder, installation_id) != 0 ||
        installer_command_append_argument(&builder, scope_argument) != 0) {
        free(command);
        DeleteFileW(helper);
        return -1;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (CreateProcessW(helper, command, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                       NULL, NULL, &startup, &process)) {
        HANDLE delete_later;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        result = 0;
        /* Once the child image is mapped, mark its temporary source for
         * deletion.  Windows removes it when the child releases the image. */
        delete_later = CreateFileW(
            helper, DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (delete_later != INVALID_HANDLE_VALUE) CloseHandle(delete_later);
    }
    free(command);
    if (result != 0) DeleteFileW(helper);
    return result;
}

static int installer_delete_file_retry(const wchar_t *path) {
    unsigned int attempt;
    for (attempt = 0; attempt < 100u; ++attempt) {
        DWORD error;
        if (DeleteFileW(path)) return 0;
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return 0;
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            return -1;
        }
        Sleep(50);
    }
    return -1;
}

static int installer_cleanup_filename_valid(const wchar_t *filename) {
    static const wchar_t prefix[] = L"jvman-cleanup-";
    static const wchar_t suffix[] = L".exe";
    const size_t prefix_length = sizeof(prefix) / sizeof(prefix[0]) - 1u;
    const size_t suffix_length = sizeof(suffix) / sizeof(suffix[0]) - 1u;
    const size_t expected_length = prefix_length + 8u + 1u + 8u + 1u + 3u +
                                   suffix_length;
    size_t index;
    if (!filename || wcslen(filename) != expected_length ||
        _wcsnicmp(filename, prefix, prefix_length) != 0) {
        return 0;
    }
    index = prefix_length;
    for (; index < prefix_length + 8u; ++index) {
        wchar_t value = filename[index];
        if (!((value >= L'0' && value <= L'9') ||
              (value >= L'a' && value <= L'f') ||
              (value >= L'A' && value <= L'F'))) {
            return 0;
        }
    }
    if (filename[index++] != L'-') return 0;
    for (; index < prefix_length + 17u; ++index) {
        wchar_t value = filename[index];
        if (!((value >= L'0' && value <= L'9') ||
              (value >= L'a' && value <= L'f') ||
              (value >= L'A' && value <= L'F'))) {
            return 0;
        }
    }
    if (filename[index++] != L'-') return 0;
    if (filename[index] < L'0' || filename[index] > L'9' ||
        filename[index + 1u] < L'0' || filename[index + 1u] > L'9' ||
        filename[index + 2u] < L'0' || filename[index + 2u] > L'9') {
        return 0;
    }
    index += 3u;
    return _wcsicmp(filename + index, suffix) == 0;
}

static int installer_cleanup_helper_path_valid(
    const wchar_t *module_path, const JvmanInstallPaths *paths,
    JvmanEnvironmentScope metadata_scope) {
    JvmanInstallPaths expected_machine_paths;
    wchar_t temp_directory[JVMAN_INSTALL_PATH_CHARS];
    const wchar_t *base_directory;
    const wchar_t *filename;
    size_t base_length;
    size_t module_length;
    DWORD length;
    DWORD attributes;
    if (!module_path || !paths ||
        (metadata_scope != JVMAN_ENV_SCOPE_USER &&
         metadata_scope != JVMAN_ENV_SCOPE_MACHINE)) {
        return 0;
    }
    if (metadata_scope == JVMAN_ENV_SCOPE_MACHINE) {
        if (jvman_install_paths_machine_default(&expected_machine_paths) !=
                JVMAN_INSTALL_OK ||
            _wcsicmp(paths->install_dir,
                     expected_machine_paths.install_dir) != 0 ||
            _wcsicmp(paths->data_home, expected_machine_paths.data_home) != 0) {
            return 0;
        }
        base_directory = paths->install_dir;
    } else {
        length = GetTempPathW(
            (DWORD)(sizeof(temp_directory) / sizeof(temp_directory[0])),
            temp_directory);
        if (length == 0 ||
            length >= sizeof(temp_directory) / sizeof(temp_directory[0])) {
            return 0;
        }
        base_directory = temp_directory;
    }
    base_length = wcslen(base_directory);
    while (base_length > 3u &&
           (base_directory[base_length - 1u] == L'\\' ||
            base_directory[base_length - 1u] == L'/')) {
        --base_length;
    }
    module_length = wcslen(module_path);
    if (module_length <= base_length + 1u ||
        _wcsnicmp(module_path, base_directory, base_length) != 0 ||
        (module_path[base_length] != L'\\' &&
         module_path[base_length] != L'/')) {
        return 0;
    }
    filename = module_path + base_length + 1u;
    if (wcschr(filename, L'\\') || wcschr(filename, L'/') ||
        !installer_cleanup_filename_valid(filename)) {
        return 0;
    }
    attributes = GetFileAttributesW(module_path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                          FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

static int installer_mark_cleanup_helper_for_deletion(
    const wchar_t *module_path, const JvmanInstallPaths *paths,
    JvmanEnvironmentScope metadata_scope) {
    /* A running image cannot normally be unlinked on Windows.  Rename its
     * default data stream to a private ADS, then request POSIX deletion of
     * the now-unlinked image.  Scope-aware path validation prevents the hidden
     * cleanup mode from being used against an arbitrary executable. */
    wchar_t stream_name[64];
    size_t stream_length;
    size_t rename_size;
    FILE_RENAME_INFO *rename_info = NULL;
    FILE_DISPOSITION_INFO disposition;
    JvmanFileDispositionInfoEx disposition_ex;
    HANDLE file = INVALID_HANDLE_VALUE;
    int result = -1;
    if (!installer_cleanup_helper_path_valid(module_path, paths,
                                             metadata_scope)) {
        return -1;
    }
    if (installer_format(stream_name,
                         sizeof(stream_name) / sizeof(stream_name[0]),
                         L":jvman-delete-%08lx",
                         (unsigned long)GetCurrentProcessId()) != 0) {
        return -1;
    }
    stream_length = wcslen(stream_name) * sizeof(wchar_t);
    if (stream_length > UINT32_MAX ||
        sizeof(FILE_RENAME_INFO) > SIZE_MAX - stream_length) return -1;
    rename_size = sizeof(FILE_RENAME_INFO) + stream_length;
    rename_info = (FILE_RENAME_INFO *)calloc(1, rename_size);
    if (!rename_info) return -1;
    rename_info->ReplaceIfExists = TRUE;
    rename_info->RootDirectory = NULL;
    rename_info->FileNameLength = (DWORD)stream_length;
    memcpy(rename_info->FileName, stream_name, stream_length);
    file = CreateFileW(module_path, DELETE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !SetFileInformationByHandle(file, FileRenameInfo, rename_info,
                                    (DWORD)rename_size)) {
        goto done;
    }
    CloseHandle(file);
    file = CreateFileW(module_path, DELETE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                   sizeof(disposition))) {
        result = 0;
    } else {
        disposition_ex.Flags = JVMAN_FILE_DISPOSITION_FLAG_DELETE |
                               JVMAN_FILE_DISPOSITION_FLAG_POSIX |
                               JVMAN_FILE_DISPOSITION_FLAG_IGNORE_READONLY;
        if (SetFileInformationByHandle(
                file, JVMAN_FILE_DISPOSITION_INFO_EX_CLASS,
                                       &disposition_ex,
                                       sizeof(disposition_ex))) {
            result = 0;
        }
    }
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(rename_info);
    return result;
}

static int installer_cleanup_worker(int argc, wchar_t **argv) {
    JvmanInstallPaths paths;
    JvmanInstallPaths expected_machine_paths;
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    DWORD parent_id;
    DWORD length;
    HANDLE parent;
    HANDLE instance_mutex = NULL;
    HANDLE machine_lock = NULL;
    DWORD wait_result;
    DWORD attributes;
    JvmanInstallerMetadata metadata;
    int metadata_found = 0;
    int already_running = 0;
    int result = 1;
    JvmanEnvironmentStatus metadata_status;
    JvmanEnvironmentScope metadata_scope;
    if (argc != 7 || !argv || !installer_is_switch(argv[1], L"/CLEANUP") ||
        installer_parse_process_id(argv[2], &parent_id) != 0 ||
        (!installer_option_equal(argv[6], L"0") &&
         !installer_option_equal(argv[6], L"1")) ||
        jvman_install_paths_init(&paths, argv[3], argv[4]) != JVMAN_INSTALL_OK) {
        return 2;
    }
    metadata_scope = installer_option_equal(argv[6], L"1")
                         ? JVMAN_ENV_SCOPE_MACHINE
                         : JVMAN_ENV_SCOPE_USER;
    if (metadata_scope == JVMAN_ENV_SCOPE_MACHINE &&
        (jvman_install_paths_machine_default(&expected_machine_paths) !=
             JVMAN_INSTALL_OK ||
         _wcsicmp(paths.install_dir,
                  expected_machine_paths.install_dir) != 0 ||
         _wcsicmp(paths.data_home, expected_machine_paths.data_home) != 0)) {
        return 2;
    }
    length = GetModuleFileNameW(NULL, module,
                                (DWORD)(sizeof(module) / sizeof(module[0])));
    if (length == 0 || length >= sizeof(module) / sizeof(module[0]) ||
        _wcsicmp(module, paths.uninstall_path) == 0 ||
        !installer_cleanup_helper_path_valid(module, &paths, metadata_scope) ||
        jvman_install_marker_read(&paths, marker,
                                  sizeof(marker) / sizeof(marker[0])) !=
            JVMAN_INSTALL_OK ||
        _wcsicmp(marker, argv[5]) != 0) {
        return 2;
    }
    (void)installer_mark_cleanup_helper_for_deletion(module, &paths,
                                                     metadata_scope);
    parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
    if (parent) {
        wait_result = WaitForSingleObject(parent, INFINITE);
        CloseHandle(parent);
        if (wait_result != WAIT_OBJECT_0) return 1;
    } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
        return 1;
    }
    /* ERROR_INVALID_PARAMETER means the validated parent PID already exited
     * before this helper could open it. Locking and validation below still
     * gate every deletion, so the cleanup can safely continue in that race. */
    instance_mutex = installer_acquire_single_instance_after_handoff(
        &already_running);
    if (!instance_mutex) return 1;
    if (metadata_scope == JVMAN_ENV_SCOPE_MACHINE) {
        machine_lock = installer_acquire_machine_instance(1, &already_running);
        if (!machine_lock) goto done;
    }

    /* Rebuild and re-authenticate all paths while holding the same locks as a
     * new install.  A replacement install must never be deleted by an older
     * cleanup worker. */
    if (jvman_install_paths_init(&paths, argv[3], argv[4]) != JVMAN_INSTALL_OK ||
        (metadata_scope == JVMAN_ENV_SCOPE_MACHINE &&
         (jvman_install_paths_machine_default(&expected_machine_paths) !=
              JVMAN_INSTALL_OK ||
          _wcsicmp(paths.install_dir,
                   expected_machine_paths.install_dir) != 0 ||
          _wcsicmp(paths.data_home, expected_machine_paths.data_home) != 0)) ||
        jvman_install_marker_read(&paths, marker,
                                  sizeof(marker) / sizeof(marker[0])) !=
            JVMAN_INSTALL_OK ||
        _wcsicmp(marker, argv[5]) != 0) {
        goto done;
    }

    /* The parent removes its registry record only after all environment and
     * ARP operations succeed. If it crashed or a registry write failed, leave
     * the authenticated files in place for a retry. */
    jvman_installer_metadata_init(&metadata);
    metadata_status = jvman_installer_metadata_load_scoped(
        metadata_scope, &metadata, &metadata_found);
    jvman_installer_metadata_free(&metadata);
    if (metadata_status != JVMAN_ENV_OK || metadata_found ||
        installer_delete_file_retry(paths.uninstall_path) != 0 ||
        installer_delete_file_retry(paths.marker_path) != 0) {
        goto done;
    }
    attributes = GetFileAttributesW(paths.install_dir);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        !RemoveDirectoryW(paths.install_dir)) {
        DWORD error = GetLastError();
        if (error != ERROR_DIR_NOT_EMPTY && error != ERROR_FILE_NOT_FOUND &&
            error != ERROR_PATH_NOT_FOUND) {
            goto done;
        }
    }
    result = 0;

done:
    if (machine_lock) CloseHandle(machine_lock);
    if (instance_mutex) {
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
    }
    return result;
}

static JvmanEnvironmentScope installer_path_scope_from_metadata(uint32_t scope) {
    return scope == (uint32_t)JVMAN_ENV_SCOPE_MACHINE
               ? JVMAN_ENV_SCOPE_MACHINE
               : JVMAN_ENV_SCOPE_USER;
}

static int installer_prepare_legacy_cleanup(InstallerOptions *options) {
    JvmanInstallerMetadata metadata;
    JvmanInstallPaths paths;
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    JvmanEnvironmentStatus status;
    unsigned int flags = 0u;
    int found = 0;
    if (!options || options->elevated_resume || options->portable ||
        (options->machine_mode && options->uninstall)) {
        return 0;
    }
    jvman_installer_metadata_init(&metadata);
    status = jvman_installer_metadata_load_scoped(
        JVMAN_ENV_SCOPE_USER, &metadata, &found);
    if (status != JVMAN_ENV_OK) {
        jvman_installer_metadata_free(&metadata);
        return -1;
    }
    if (!found) {
        jvman_installer_metadata_free(&metadata);
        return 0;
    }
    if (metadata.app_path_owned &&
        metadata.app_path_scope == (uint32_t)JVMAN_ENV_SCOPE_MACHINE) {
        flags |= INSTALLER_LEGACY_APP_PATH;
    }
    if (metadata.java_path_owned &&
        metadata.java_path_scope == (uint32_t)JVMAN_ENV_SCOPE_MACHINE) {
        flags |= INSTALLER_LEGACY_JAVA_PATH;
    }
    if (flags == 0u) {
        jvman_installer_metadata_free(&metadata);
        return 0;
    }
    if (!metadata.install_dir || !metadata.data_home || !metadata.install_id ||
        jvman_install_paths_init(&paths, metadata.install_dir,
                                 metadata.data_home) != JVMAN_INSTALL_OK ||
        jvman_install_marker_read(
            &paths, marker, sizeof(marker) / sizeof(marker[0])) !=
            JVMAN_INSTALL_OK ||
        _wcsicmp(marker, metadata.install_id) != 0) {
        jvman_installer_metadata_free(&metadata);
        return -1;
    }
    installer_copy(options->legacy_install_dir,
                   sizeof(options->legacy_install_dir) /
                       sizeof(options->legacy_install_dir[0]),
                   paths.install_dir);
    installer_copy(options->legacy_data_home,
                   sizeof(options->legacy_data_home) /
                       sizeof(options->legacy_data_home[0]),
                   paths.data_home);
    installer_copy(options->legacy_install_id,
                   sizeof(options->legacy_install_id) /
                       sizeof(options->legacy_install_id[0]),
                   metadata.install_id);
    options->legacy_cleanup_flags = flags;
    options->legacy_cleanup_only = !options->machine_mode;
    jvman_installer_metadata_free(&metadata);
    return 1;
}

static JvmanEnvironmentStatus installer_validate_legacy_paths(
    const InstallerOptions *options, JvmanInstallPaths *paths,
    wchar_t java_bin[], size_t java_bin_capacity) {
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    if (!options || !paths || options->legacy_cleanup_flags == 0u ||
        (options->legacy_cleanup_flags & ~INSTALLER_LEGACY_PATH_MASK) != 0u ||
        jvman_install_paths_init(paths, options->legacy_install_dir,
                                 options->legacy_data_home) != JVMAN_INSTALL_OK ||
        jvman_install_marker_read(
            paths, marker, sizeof(marker) / sizeof(marker[0])) !=
            JVMAN_INSTALL_OK ||
        _wcsicmp(marker, options->legacy_install_id) != 0) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    if ((options->legacy_cleanup_flags & INSTALLER_LEGACY_JAVA_PATH) != 0u &&
        (!java_bin || java_bin_capacity == 0u ||
         installer_join(java_bin, java_bin_capacity, paths->data_home,
                        L"current\\bin") != 0)) {
        return JVMAN_ENV_TOO_LONG;
    }
    return JVMAN_ENV_OK;
}

/*
 * 0.5.0: Move Java-family entries in the HKLM Path to the tail so a
 * user-owned jvman entry takes precedence. Must run in the elevated helper
 * child. Persists prior/managed snapshots to the machine metadata so the
 * corresponding restore helper can roll back on uninstall.
 */
static JvmanEnvironmentStatus installer_relocate_java_hklm_execute(void) {
    JvmanEnvironmentPathSnapshot before;
    JvmanEnvironmentPathSnapshot expected_current;
    JvmanEnvironmentPathSnapshot rewritten;
    JvmanInstallerMetadata metadata;
    JvmanPathListStatus pathlist_status;
    JvmanEnvironmentStatus status;
    wchar_t *rewritten_value = NULL;
    int changed = 0;
    int metadata_found = 0;
    size_t rewritten_length;

    jvman_environment_path_snapshot_init(&before);
    jvman_environment_path_snapshot_init(&expected_current);
    jvman_environment_path_snapshot_init(&rewritten);
    jvman_installer_metadata_init(&metadata);

    status = jvman_environment_path_snapshot_capture(JVMAN_ENV_SCOPE_MACHINE,
                                                     &before);
    if (status != JVMAN_ENV_OK) return status;
    if (!before.present || before.byte_count == 0u || !before.value) {
        jvman_environment_path_snapshot_free(&before);
        return JVMAN_ENV_OK;
    }

    pathlist_status = jvman_pathlist_move_java_family_to_end(
        before.value, &rewritten_value, &changed);
    if (pathlist_status != JVMAN_PATHLIST_OK) {
        jvman_environment_path_snapshot_free(&before);
        free(rewritten_value);
        return JVMAN_ENV_METADATA_INVALID;
    }
    if (!changed || !rewritten_value) {
        free(rewritten_value);
        jvman_environment_path_snapshot_free(&before);
        return JVMAN_ENV_OK;
    }

    /* expected_current = clone of before, used as CAS anchor for restore. */
    expected_current.valid = 1;
    expected_current.present = 1;
    expected_current.type = before.type;
    expected_current.byte_count = before.byte_count;
    expected_current.value = (wchar_t *)malloc(before.byte_count);
    if (!expected_current.value) {
        free(rewritten_value);
        jvman_environment_path_snapshot_free(&before);
        return JVMAN_ENV_NO_MEMORY;
    }
    memcpy(expected_current.value, before.value, before.byte_count);

    rewritten_length = wcslen(rewritten_value);
    rewritten.valid = 1;
    rewritten.present = 1;
    rewritten.type = before.type ? before.type : (uint32_t)REG_SZ;
    rewritten.byte_count = (uint32_t)((rewritten_length + 1u) * sizeof(wchar_t));
    rewritten.value = rewritten_value;
    rewritten_value = NULL; /* ownership transferred to snapshot */

    /* CAS write: only overwrite if HKLM Path is still equal to `before`. */
    status = jvman_environment_path_snapshot_restore(
        JVMAN_ENV_SCOPE_MACHINE, &rewritten, &expected_current, &changed);
    jvman_environment_path_snapshot_free(&rewritten);

    if (status != JVMAN_ENV_OK) {
        jvman_environment_path_snapshot_free(&before);
        jvman_environment_path_snapshot_free(&expected_current);
        return status;
    }

    /* Persist prior + managed values to the machine metadata so uninstall
     * can restore. Preserve existing metadata fields when loading. */
    status = jvman_installer_metadata_load_scoped(
        JVMAN_ENV_SCOPE_MACHINE, &metadata, &metadata_found);
    if (status == JVMAN_ENV_NOT_FOUND) {
        status = JVMAN_ENV_OK;
        metadata_found = 0;
    }
    if (status != JVMAN_ENV_OK) goto done;

    if (!metadata_found) {
        /* Minimal metadata so metadata_validate passes. */
        metadata.version = _wcsdup(L"legacy-java-hklm");
        metadata.install_id = _wcsdup(L"legacy-java-hklm");
        metadata.install_dir = _wcsdup(L"");
        metadata.data_home = _wcsdup(L"");
        if (!metadata.version || !metadata.install_id ||
            !metadata.install_dir || !metadata.data_home) {
            status = JVMAN_ENV_NO_MEMORY;
            goto done;
        }
    }
    /* If we've already been here and there's an existing managed snapshot,
     * keep the original prior_value so repeat runs remain idempotent. */
    if (!metadata.legacy_java_hklm_owned) {
        metadata.legacy_java_hklm_owned = 1;
        metadata.legacy_java_hklm_prior_present = 1;
        metadata.legacy_java_hklm_prior_type = before.type;
        free(metadata.legacy_java_hklm_prior_value);
        metadata.legacy_java_hklm_prior_value = _wcsdup(before.value);
        if (!metadata.legacy_java_hklm_prior_value) {
            status = JVMAN_ENV_NO_MEMORY;
            goto done;
        }
    }
    free(metadata.legacy_java_hklm_managed_value);
    metadata.legacy_java_hklm_managed_value = NULL;
    {
        /* Recompute the post-relocation value from the still-valid
         * expected_current snapshot; the earlier `rewritten` snapshot's
         * buffer was already released by path_snapshot_free above. */
        wchar_t *reformed = NULL;
        int reformed_changed = 0;
        pathlist_status = jvman_pathlist_move_java_family_to_end(
            expected_current.value, &reformed, &reformed_changed);
        if (pathlist_status != JVMAN_PATHLIST_OK || !reformed) {
            free(reformed);
            status = JVMAN_ENV_METADATA_INVALID;
            goto done;
        }
        metadata.legacy_java_hklm_managed_value = reformed;
    }

    status = jvman_installer_metadata_save_scoped(JVMAN_ENV_SCOPE_MACHINE,
                                                  &metadata);
done:
    jvman_environment_path_snapshot_free(&before);
    jvman_environment_path_snapshot_free(&expected_current);
    jvman_installer_metadata_free(&metadata);
    return status;
}

/*
 * 0.5.0: Restore the HKLM Path to the value captured by
 * installer_relocate_java_hklm_execute. Silently succeeds if metadata does
 * not indicate this helper touched the path, or if the current HKLM Path no
 * longer matches the value we wrote (user manually edited it).
 */
static JvmanEnvironmentStatus installer_restore_java_hklm_execute(void) {
    JvmanInstallerMetadata metadata;
    JvmanEnvironmentPathSnapshot expected_current;
    JvmanEnvironmentPathSnapshot prior;
    JvmanEnvironmentStatus status;
    int metadata_found = 0;
    int changed = 0;

    jvman_installer_metadata_init(&metadata);
    jvman_environment_path_snapshot_init(&expected_current);
    jvman_environment_path_snapshot_init(&prior);

    status = jvman_installer_metadata_load_scoped(
        JVMAN_ENV_SCOPE_MACHINE, &metadata, &metadata_found);
    if (status == JVMAN_ENV_NOT_FOUND) return JVMAN_ENV_OK;
    if (status != JVMAN_ENV_OK) return status;
    if (!metadata_found || !metadata.legacy_java_hklm_owned) {
        jvman_installer_metadata_free(&metadata);
        return JVMAN_ENV_OK;
    }
    if (!metadata.legacy_java_hklm_managed_value) {
        jvman_installer_metadata_free(&metadata);
        return JVMAN_ENV_OK;
    }

    /* expected_current = the value we wrote at relocate time. */
    {
        size_t managed_length = wcslen(metadata.legacy_java_hklm_managed_value);
        expected_current.valid = 1;
        expected_current.present = 1;
        expected_current.type = metadata.legacy_java_hklm_prior_present
                                    ? metadata.legacy_java_hklm_prior_type
                                    : (uint32_t)REG_SZ;
        expected_current.byte_count =
            (uint32_t)((managed_length + 1u) * sizeof(wchar_t));
        expected_current.value =
            (wchar_t *)malloc(expected_current.byte_count);
        if (!expected_current.value) {
            jvman_installer_metadata_free(&metadata);
            return JVMAN_ENV_NO_MEMORY;
        }
        memcpy(expected_current.value,
               metadata.legacy_java_hklm_managed_value,
               expected_current.byte_count);
    }

    if (metadata.legacy_java_hklm_prior_present &&
        metadata.legacy_java_hklm_prior_value) {
        size_t prior_length =
            wcslen(metadata.legacy_java_hklm_prior_value);
        prior.valid = 1;
        prior.present = 1;
        prior.type = metadata.legacy_java_hklm_prior_type;
        prior.byte_count =
            (uint32_t)((prior_length + 1u) * sizeof(wchar_t));
        prior.value = (wchar_t *)malloc(prior.byte_count);
        if (!prior.value) {
            free(expected_current.value);
            jvman_installer_metadata_free(&metadata);
            return JVMAN_ENV_NO_MEMORY;
        }
        memcpy(prior.value, metadata.legacy_java_hklm_prior_value,
               prior.byte_count);
    } else {
        prior.valid = 1;
        prior.present = 0;
        prior.type = 0u;
        prior.byte_count = 0u;
        prior.value = NULL;
    }

    status = jvman_environment_path_snapshot_restore(
        JVMAN_ENV_SCOPE_MACHINE, &prior, &expected_current, &changed);

    if (status == JVMAN_ENV_OK) {
        metadata.legacy_java_hklm_owned = 0;
        metadata.legacy_java_hklm_prior_present = 0;
        metadata.legacy_java_hklm_prior_type = 0u;
        free(metadata.legacy_java_hklm_prior_value);
        metadata.legacy_java_hklm_prior_value = NULL;
        free(metadata.legacy_java_hklm_managed_value);
        metadata.legacy_java_hklm_managed_value = NULL;
        (void)jvman_installer_metadata_save_scoped(JVMAN_ENV_SCOPE_MACHINE,
                                                    &metadata);
    }

    jvman_environment_path_snapshot_free(&expected_current);
    jvman_environment_path_snapshot_free(&prior);
    jvman_installer_metadata_free(&metadata);
    /* CONFLICT means user edited HKLM Path since our write; leave it alone. */
    if (status == JVMAN_ENV_CONFLICT) return JVMAN_ENV_OK;
    return status;
}

static JvmanEnvironmentStatus installer_restore_legacy_machine_paths(
    const InstallerOptions *options, unsigned int restore_flags) {
    JvmanInstallPaths paths;
    wchar_t java_bin[JVMAN_INSTALL_PATH_CHARS];
    JvmanEnvironmentStatus status;
    int owned = 0;
    int step_changed = 0;
    if (!options || (restore_flags & ~options->legacy_cleanup_flags) != 0u ||
        (restore_flags & ~INSTALLER_LEGACY_PATH_MASK) != 0u) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    status = installer_validate_legacy_paths(
        options, &paths, java_bin,
        sizeof(java_bin) / sizeof(java_bin[0]));
    if (status != JVMAN_ENV_OK) return status;
    if ((restore_flags & INSTALLER_LEGACY_APP_PATH) != 0u) {
        status = jvman_environment_add_path(
            JVMAN_ENV_SCOPE_MACHINE, paths.install_dir, 0, &owned,
            &step_changed, NULL);
        if (status != JVMAN_ENV_OK) return status;
    }
    if ((restore_flags & INSTALLER_LEGACY_JAVA_PATH) != 0u) {
        owned = 0;
        step_changed = 0;
        status = jvman_environment_add_path(
            JVMAN_ENV_SCOPE_MACHINE, java_bin, 0, &owned, &step_changed,
            NULL);
        if (status != JVMAN_ENV_OK) return status;
    }
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus installer_remove_legacy_machine_paths(
    const InstallerOptions *options, unsigned int *removed_flags_out) {
    JvmanInstallPaths paths;
    wchar_t java_bin[JVMAN_INSTALL_PATH_CHARS];
    JvmanEnvironmentStatus status;
    JvmanEnvironmentStatus restore_status;
    int step_changed = 0;
    if (!removed_flags_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *removed_flags_out = 0u;
    status = installer_validate_legacy_paths(
        options, &paths, java_bin,
        sizeof(java_bin) / sizeof(java_bin[0]));
    if (status != JVMAN_ENV_OK) return status;
    if ((options->legacy_cleanup_flags & INSTALLER_LEGACY_APP_PATH) != 0u) {
        status = jvman_environment_remove_path(
            JVMAN_ENV_SCOPE_MACHINE, paths.install_dir, 1, &step_changed,
            NULL);
        if (status != JVMAN_ENV_OK) goto rollback;
        if (step_changed) *removed_flags_out |= INSTALLER_LEGACY_APP_PATH;
    }
    if ((options->legacy_cleanup_flags & INSTALLER_LEGACY_JAVA_PATH) != 0u) {
        step_changed = 0;
        status = jvman_environment_remove_path(
            JVMAN_ENV_SCOPE_MACHINE, java_bin, 1, &step_changed, NULL);
        if (status != JVMAN_ENV_OK) goto rollback;
        if (step_changed) *removed_flags_out |= INSTALLER_LEGACY_JAVA_PATH;
    }
    return JVMAN_ENV_OK;

rollback:
    restore_status = installer_restore_legacy_machine_paths(
        options, *removed_flags_out);
    if (restore_status == JVMAN_ENV_OK) *removed_flags_out = 0u;
    return restore_status == JVMAN_ENV_OK ? status : restore_status;
}

static JvmanEnvironmentStatus installer_clear_legacy_metadata(
    const InstallerOptions *options, JvmanInstallerMetadata *metadata) {
    JvmanInstallPaths legacy_paths;
    JvmanInstallPaths metadata_paths;
    wchar_t java_bin[JVMAN_INSTALL_PATH_CHARS];
    JvmanEnvironmentStatus status;
    if (!options || !metadata || !options->legacy_cleanup_applied ||
        !metadata->install_dir || !metadata->data_home ||
        !metadata->install_id) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    status = installer_validate_legacy_paths(
        options, &legacy_paths, java_bin,
        sizeof(java_bin) / sizeof(java_bin[0]));
    if (status != JVMAN_ENV_OK ||
        jvman_install_paths_init(&metadata_paths, metadata->install_dir,
                                 metadata->data_home) != JVMAN_INSTALL_OK ||
        _wcsicmp(metadata_paths.install_dir, legacy_paths.install_dir) != 0 ||
        _wcsicmp(metadata_paths.data_home, legacy_paths.data_home) != 0 ||
        _wcsicmp(metadata->install_id, options->legacy_install_id) != 0) {
        return status == JVMAN_ENV_OK ? JVMAN_ENV_METADATA_INVALID : status;
    }
    if ((options->legacy_cleanup_flags & INSTALLER_LEGACY_APP_PATH) != 0u) {
        if (!metadata->app_path_owned ||
            metadata->app_path_scope !=
                (uint32_t)JVMAN_ENV_SCOPE_MACHINE) {
            return JVMAN_ENV_METADATA_INVALID;
        }
        metadata->app_path_owned = 0;
        metadata->app_path_scope = (uint32_t)JVMAN_ENV_SCOPE_USER;
    }
    if ((options->legacy_cleanup_flags & INSTALLER_LEGACY_JAVA_PATH) != 0u) {
        if (!metadata->java_path_owned ||
            metadata->java_path_scope !=
                (uint32_t)JVMAN_ENV_SCOPE_MACHINE) {
            return JVMAN_ENV_METADATA_INVALID;
        }
        metadata->java_path_owned = 0;
        metadata->java_path_scope = (uint32_t)JVMAN_ENV_SCOPE_USER;
    }
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus installer_commit_legacy_cleanup(
    const InstallerOptions *options) {
    JvmanInstallerMetadata metadata;
    JvmanEnvironmentStatus status;
    int found = 0;
    if (!options || options->legacy_cleanup_flags == 0u ||
        !options->legacy_cleanup_applied) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    jvman_installer_metadata_init(&metadata);
    status = jvman_installer_metadata_load_scoped(
        JVMAN_ENV_SCOPE_USER, &metadata, &found);
    if (status != JVMAN_ENV_OK || !found) {
        jvman_installer_metadata_free(&metadata);
        return status == JVMAN_ENV_OK ? JVMAN_ENV_METADATA_INVALID : status;
    }
    status = installer_clear_legacy_metadata(options, &metadata);
    if (status == JVMAN_ENV_OK) {
        status = jvman_installer_metadata_save_scoped(
            JVMAN_ENV_SCOPE_USER, &metadata);
    }
    jvman_installer_metadata_free(&metadata);
    return status;
}

static void installer_accept_legacy_cleanup(InstallerOptions *options) {
    if (!options) return;
    options->legacy_cleanup_only = 0;
    options->legacy_cleanup_applied = 1;
}

static int installer_rollback_legacy_cleanup(
    const InstallerOptions *options, unsigned int removed_flags,
    HANDLE *instance_mutex) {
    InstallerOptions restore_options;
    int already_running = 0;
    int cancelled = 0;
    int launched = 0;
    int cleanup_completed = 0;
    unsigned int ignored_removed = 0u;
    int restore_result;
    if (!options || !instance_mutex ||
        (removed_flags & ~INSTALLER_LEGACY_PATH_MASK) != 0u) {
        return -1;
    }
    if (removed_flags == 0u) return 0;
    if (installer_process_is_elevated()) {
        return installer_restore_legacy_machine_paths(
                   options, removed_flags) == JVMAN_ENV_OK
                   ? 0
                   : -1;
    }
    if (!*instance_mutex) {
        *instance_mutex = installer_acquire_single_instance_after_handoff(
            &already_running);
        if (!*instance_mutex) return -1;
    }
    restore_options = *options;
    restore_options.machine_mode = 0;
    restore_options.uninstall = 0;
    restore_options.portable = 0;
    restore_options.remove_data = 0;
    restore_options.remove_jdks = 0;
    restore_options.uninstall_scope_set = 0;
    restore_options.add_path = 1;
    restore_options.path_scope_set = 0;
    restore_options.path_scope = JVMAN_ENV_SCOPE_USER;
    restore_options.configure_java = 0;
    restore_options.replace_java_home = 0;
    restore_options.discover = 0;
    restore_options.elevated_resume = 0;
    restore_options.legacy_cleanup_only = 0;
    restore_options.legacy_restore_only = 1;
    restore_options.legacy_cleanup_applied = 0;
    restore_options.legacy_cleanup_flags = removed_flags;
    restore_options.install_dir_set = 0;
    restore_options.install_dir[0] = L'\0';
    restore_result = installer_run_elevated_machine(
        &restore_options, instance_mutex, &cancelled, &launched,
        &cleanup_completed, &ignored_removed);
    return launched && restore_result == 0 ? 0 : -1;
}

static JvmanEnvironmentStatus installer_save_user_language(void) {
    static const wchar_t key_name[] = L"Software\\jvman\\Preferences";
    static const wchar_t value_name[] = L"Language";
    const wchar_t *language = installer_language_tag();
    HKEY key = NULL;
    LSTATUS result = RegCreateKeyExW(
        HKEY_CURRENT_USER, key_name, 0, NULL, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, NULL, &key, NULL);
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(
            key, value_name, 0, REG_SZ, (const BYTE *)language,
            (DWORD)((wcslen(language) + 1u) * sizeof(language[0])));
        RegCloseKey(key);
    }
    if (result == ERROR_SUCCESS) return JVMAN_ENV_OK;
    if (result == ERROR_ACCESS_DENIED || result == ERROR_PRIVILEGE_NOT_HELD) {
        return JVMAN_ENV_ACCESS_DENIED;
    }
    return JVMAN_ENV_WIN32_ERROR;
}

static int installer_current_module_matches(const wchar_t *path) {
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    DWORD length;
    if (!path) return 0;
    length = GetModuleFileNameW(
        NULL, module, (DWORD)(sizeof(module) / sizeof(module[0])));
    return length != 0u && length < sizeof(module) / sizeof(module[0]) &&
           _wcsicmp(module, path) == 0;
}

static JvmanEnvironmentPathSnapshot *installer_written_path_snapshot(
    JvmanEnvironmentScope scope,
    JvmanEnvironmentPathSnapshot *user_written,
    JvmanEnvironmentPathSnapshot *machine_written) {
    return scope == JVMAN_ENV_SCOPE_MACHINE ? machine_written : user_written;
}

static uint32_t installer_path_scope_bit(JvmanEnvironmentScope scope) {
    return 1u << (uint32_t)scope;
}

static uint32_t installer_environment_path_scope_mask(
    const InstallerOptions *options, const JvmanInstallerMetadata *metadata) {
    uint32_t mask = 0u;
    int manage_java_path;
    if (!options || !metadata) return 0u;
    if (options->add_path) {
        mask |= installer_path_scope_bit(options->path_scope);
        if (metadata->app_path_owned) {
            mask |= installer_path_scope_bit(installer_path_scope_from_metadata(
                metadata->app_path_scope));
        }
    } else if (metadata->app_path_owned) {
        mask |= installer_path_scope_bit(installer_path_scope_from_metadata(
            metadata->app_path_scope));
    }
    manage_java_path = options->add_path &&
                       options->path_scope == JVMAN_ENV_SCOPE_USER;
    if (manage_java_path) {
        mask |= installer_path_scope_bit(options->path_scope);
        if (metadata->java_path_owned) {
            mask |= installer_path_scope_bit(installer_path_scope_from_metadata(
                metadata->java_path_scope));
        }
    } else if (metadata->java_path_owned) {
        mask |= installer_path_scope_bit(installer_path_scope_from_metadata(
            metadata->java_path_scope));
    }
    return mask;
}

static JvmanEnvironmentStatus installer_apply_path_entry(
    const wchar_t *directory, JvmanEnvironmentScope desired_scope, int add,
    int *owned, uint32_t *scope_value, int *changed_out,
    JvmanEnvironmentPathSnapshot *user_written,
    JvmanEnvironmentPathSnapshot *machine_written) {
    JvmanEnvironmentScope current_scope;
    JvmanEnvironmentStatus status;
    int step_changed = 0;
    if (!directory || !owned || !scope_value || !changed_out ||
        !user_written || !machine_written ||
        (add != 0 && add != 1)) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *changed_out = 0;
    current_scope = installer_path_scope_from_metadata(*scope_value);
    if (add) {
        if (*owned && current_scope != desired_scope) {
            status = jvman_environment_remove_path(current_scope, directory, 1,
                &step_changed, installer_written_path_snapshot(
                    current_scope, user_written, machine_written));
            if (status != JVMAN_ENV_OK) return status;
            *owned = 0;
            *changed_out |= step_changed;
        }
        step_changed = 0;
        status = jvman_environment_add_path(desired_scope, directory, *owned,
            owned, &step_changed, installer_written_path_snapshot(
                desired_scope, user_written, machine_written));
        if (status != JVMAN_ENV_OK) return status;
        *scope_value = (uint32_t)desired_scope;
        *changed_out |= step_changed;
    } else if (*owned) {
        status = jvman_environment_remove_path(current_scope, directory, 1,
            &step_changed, installer_written_path_snapshot(
                current_scope, user_written, machine_written));
        if (status != JVMAN_ENV_OK) return status;
        *owned = 0;
        *scope_value = (uint32_t)desired_scope;
        *changed_out |= step_changed;
    } else {
        *scope_value = (uint32_t)desired_scope;
    }
    return JVMAN_ENV_OK;
}

static int installer_apply_environment(const InstallerOptions *options,
                                       const JvmanInstallPaths *paths,
                                       JvmanInstallerMetadata *metadata,
                                       int *changed_out,
                                       int *java_home_changed_out,
                                       JvmanEnvironmentPathSnapshot *user_written,
                                       JvmanEnvironmentPathSnapshot *machine_written,
                                       JvmanEnvironmentValueSnapshot *java_home_written) {
    wchar_t current[JVMAN_INSTALL_PATH_CHARS];
    int changed = 0;
    int step_changed = 0;
    int manage_java_path;
    JvmanEnvironmentStatus status;
    if (!options || !paths || !metadata || !changed_out ||
        !java_home_changed_out || !user_written || !machine_written ||
        !java_home_written) return -1;
    *changed_out = 0;
    *java_home_changed_out = 0;
    manage_java_path = options->add_path &&
                       options->path_scope == JVMAN_ENV_SCOPE_USER;
    if (options->add_path) {
        status = installer_apply_path_entry(
            paths->install_dir, options->path_scope, 1,
            &metadata->app_path_owned, &metadata->app_path_scope,
            &step_changed, user_written, machine_written);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    } else if (metadata->app_path_owned) {
        status = installer_apply_path_entry(
            paths->install_dir, options->path_scope, 0,
            &metadata->app_path_owned, &metadata->app_path_scope,
            &step_changed, user_written, machine_written);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    }

    if (options->configure_java == 1) {
        if (installer_join(current, sizeof(current) / sizeof(*current),
                           paths->data_home, L"current") != 0) return (int)JVMAN_ENV_TOO_LONG;
        status = jvman_environment_configure_java_home(
            current, metadata, options->replace_java_home, &step_changed,
            java_home_written);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
        *java_home_changed_out |= step_changed;
    } else if (options->configure_java == -1 && metadata->java_home_owned) {
        status = jvman_environment_restore_java_home(
            metadata, &step_changed, java_home_written);
        if (status != JVMAN_ENV_OK) return (int)status;
        metadata->java_home_owned = 0;
        changed |= step_changed;
        *java_home_changed_out |= step_changed;
    }

    if (manage_java_path || metadata->java_path_owned) {
        if (installer_join(current, sizeof(current) / sizeof(*current),
                           paths->data_home, L"current\\bin") != 0) {
            return (int)JVMAN_ENV_TOO_LONG;
        }
    }
    if (manage_java_path) {
        status = installer_apply_path_entry(
            current, options->path_scope, 1,
            &metadata->java_path_owned, &metadata->java_path_scope,
            &step_changed, user_written, machine_written);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    } else if (metadata->java_path_owned) {
        status = installer_apply_path_entry(
            current, options->path_scope, 0,
            &metadata->java_path_owned, &metadata->java_path_scope,
            &step_changed, user_written, machine_written);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    }
    *changed_out = changed;
    return 0;
}

static JvmanEnvironmentStatus installer_restore_path_snapshots(
    const JvmanEnvironmentPathSnapshot *user_before,
    const JvmanEnvironmentPathSnapshot *machine_before,
    const JvmanEnvironmentPathSnapshot *user_written,
    const JvmanEnvironmentPathSnapshot *machine_written) {
    JvmanEnvironmentStatus first_error = JVMAN_ENV_OK;
    JvmanEnvironmentStatus status;
    int changed;
    if (user_written->valid) {
        status = jvman_environment_path_snapshot_restore(
            JVMAN_ENV_SCOPE_USER, user_before, user_written, &changed);
        if (status != JVMAN_ENV_OK) first_error = status;
    }
    if (machine_written->valid) {
        status = jvman_environment_path_snapshot_restore(
            JVMAN_ENV_SCOPE_MACHINE, machine_before, machine_written, &changed);
        if (status != JVMAN_ENV_OK && first_error == JVMAN_ENV_OK) {
            first_error = status;
        }
    }
    return first_error;
}

static JvmanEnvironmentStatus installer_rollback_environment(
    const JvmanEnvironmentPathSnapshot *user_before,
    const JvmanEnvironmentPathSnapshot *machine_before,
    const JvmanEnvironmentPathSnapshot *user_written,
    const JvmanEnvironmentPathSnapshot *machine_written,
    const JvmanEnvironmentValueSnapshot *java_home_before,
    const JvmanEnvironmentValueSnapshot *java_home_written) {
    JvmanEnvironmentStatus first_error;
    JvmanEnvironmentStatus status;
    int changed;
    first_error = installer_restore_path_snapshots(
        user_before, machine_before, user_written, machine_written);
    if (java_home_written->valid) {
        status = jvman_environment_java_home_snapshot_restore(
            java_home_before, java_home_written, &changed);
        if (status != JVMAN_ENV_OK && first_error == JVMAN_ENV_OK) {
            first_error = status;
        }
    }
    return first_error;
}

static int installer_run_discover(const JvmanInstallPaths *paths) {
    wchar_t command[JVMAN_INSTALL_PATH_CHARS + 64];
    wchar_t previous_home[JVMAN_INSTALL_PATH_CHARS];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;
    DWORD previous_length;
    DWORD previous_error;
    int had_previous;
    int restore_ok;
    if (!paths || installer_format(command, sizeof(command) / sizeof(*command),
                                   L"\"%s\" discover --register",
                                   paths->jvman_path) != 0) return -1;
    previous_home[0] = L'\0';
    SetLastError(ERROR_SUCCESS);
    previous_length = GetEnvironmentVariableW(
        L"JVMAN_HOME", previous_home,
        (DWORD)(sizeof(previous_home) / sizeof(previous_home[0])));
    previous_error = GetLastError();
    if (previous_length >= sizeof(previous_home) / sizeof(previous_home[0])) {
        return -1;
    }
    if (previous_length == 0 && previous_error != ERROR_SUCCESS &&
        previous_error != ERROR_ENVVAR_NOT_FOUND) {
        return -1;
    }
    had_previous = previous_length != 0 || previous_error != ERROR_ENVVAR_NOT_FOUND;
    if (!SetEnvironmentVariableW(L"JVMAN_HOME", paths->data_home)) return -1;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(paths->jvman_path, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL, &startup, &process)) {
        (void)SetEnvironmentVariableW(
            L"JVMAN_HOME", had_previous ? previous_home : NULL);
        return -1;
    }
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(process.hProcess, &exit_code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    restore_ok = SetEnvironmentVariableW(
        L"JVMAN_HOME", had_previous ? previous_home : NULL);
    return restore_ok && exit_code == 0 ? 0 : -1;
}

static int installer_install(const InstallerOptions *options) {
    JvmanInstallPaths paths;
    JvmanSetupPayload payload;
    JvmanInstallerMetadata metadata;
    JvmanEnvironmentPathSnapshot user_path_snapshot;
    JvmanEnvironmentPathSnapshot machine_path_snapshot;
    JvmanEnvironmentPathSnapshot user_path_written;
    JvmanEnvironmentPathSnapshot machine_path_written;
    JvmanEnvironmentValueSnapshot java_home_snapshot;
    JvmanEnvironmentValueSnapshot java_home_written;
    wchar_t default_install[JVMAN_INSTALL_PATH_CHARS];
    wchar_t default_data[JVMAN_INSTALL_PATH_CHARS];
    wchar_t marker_id[JVMAN_INSTALL_MARKER_ID_CHARS];
    int found = 0;
    int backup_created = 0;
    int changed = 0;
    int java_home_changed = 0;
    uint32_t path_scope_mask = 0u;
    int result = 1;
    JvmanEnvironmentStatus env_status;
    JvmanEnvironmentStatus rollback_status;
    JvmanInstallStatus install_status;
    JvmanEnvironmentScope metadata_scope;
    if (!options) return 1;
    metadata_scope = options->machine_mode ? JVMAN_ENV_SCOPE_MACHINE
                                           : JVMAN_ENV_SCOPE_USER;
    jvman_installer_metadata_init(&metadata);
    jvman_environment_path_snapshot_init(&user_path_snapshot);
    jvman_environment_path_snapshot_init(&machine_path_snapshot);
    jvman_environment_path_snapshot_init(&user_path_written);
    jvman_environment_path_snapshot_init(&machine_path_written);
    jvman_environment_value_snapshot_init(&java_home_snapshot);
    jvman_environment_value_snapshot_init(&java_home_written);
    memset(&payload, 0, sizeof(payload));
    install_status = options->machine_mode
                         ? jvman_install_paths_machine_default(&paths)
                         : jvman_install_paths_default(&paths);
    if (install_status != JVMAN_INSTALL_OK) {
        return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_DETERMINE_PATHS),
                                 MB_OK | MB_ICONERROR, options->silent);
    }
    installer_copy(default_install, sizeof(default_install) / sizeof(*default_install),
                   paths.install_dir);
    installer_copy(default_data, sizeof(default_data) / sizeof(*default_data),
                   paths.data_home);
    if (!options->portable) {
        env_status = jvman_installer_metadata_load_scoped(
            metadata_scope, &metadata, &found);
        if (env_status != JVMAN_ENV_OK && env_status != JVMAN_ENV_NOT_FOUND) {
            jvman_installer_metadata_free(&metadata);
            return installer_report_status(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_READ_STATE),
                                           jvman_lang_environment_status(env_status),
                                           options->silent);
        }
        if (found) {
            JvmanInstallPaths recorded_paths;
            JvmanInstallStatus recorded_status;
            recorded_status = (!metadata.install_dir || !metadata.data_home)
                                  ? JVMAN_INSTALL_INVALID_ARGUMENT
                                  : jvman_install_paths_init(
                                        &recorded_paths, metadata.install_dir,
                                        metadata.data_home);
            if (options->machine_mode &&
                (recorded_status != JVMAN_INSTALL_OK ||
                 _wcsicmp(recorded_paths.install_dir, default_install) != 0 ||
                 _wcsicmp(recorded_paths.data_home, default_data) != 0 ||
                 metadata.java_path_owned || metadata.java_home_owned ||
                 metadata.app_path_scope !=
                     (uint32_t)JVMAN_ENV_SCOPE_MACHINE)) {
                recorded_status = JVMAN_INSTALL_INVALID_ARGUMENT;
            }
            if (recorded_status != JVMAN_INSTALL_OK) {
                jvman_installer_metadata_free(&metadata);
                return installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_EXISTING_PATHS_INVALID),
                    jvman_lang_install_status(recorded_status), options->silent);
            }
            /* A repeat install follows the recorded paths, including a custom
             * data home.  An explicit directory is accepted only when it
             * resolves to the same recorded installation. */
            if (options->install_dir_set) {
                installer_copy(default_install,
                               sizeof(default_install) / sizeof(*default_install),
                               options->install_dir);
                if (jvman_install_paths_init(&paths, default_install,
                                             recorded_paths.data_home) !=
                        JVMAN_INSTALL_OK ||
                    _wcsicmp(paths.install_dir, recorded_paths.install_dir) != 0) {
                    jvman_installer_metadata_free(&metadata);
                    return installer_report(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_DIR_MISMATCH),
                        MB_OK | MB_ICONERROR, options->silent);
                }
            } else {
                paths = recorded_paths;
            }
            if (!installer_metadata_matches(&metadata, &paths) ||
                jvman_install_marker_read(&paths, marker_id,
                    sizeof(marker_id) / sizeof(*marker_id)) != JVMAN_INSTALL_OK ||
                _wcsicmp(marker_id, metadata.install_id) != 0) {
                jvman_installer_metadata_free(&metadata);
                return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_STATE_MISMATCH),
                    MB_OK | MB_ICONERROR, options->silent);
            }
            if (!options->machine_mode && options->legacy_cleanup_applied) {
                env_status = installer_clear_legacy_metadata(options, &metadata);
                if (env_status != JVMAN_ENV_OK) {
                    jvman_installer_metadata_free(&metadata);
                    return installer_report_status(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_CANNOT_READ_STATE),
                        jvman_lang_environment_status(env_status),
                        options->silent);
                }
            }
        } else {
            if (options->install_dir_set) {
                installer_copy(default_install,
                               sizeof(default_install) / sizeof(*default_install),
                               options->install_dir);
                if (jvman_install_paths_init(&paths, default_install, default_data) !=
                    JVMAN_INSTALL_OK) {
                    jvman_installer_metadata_free(&metadata);
                    return installer_report(
                        jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_DIR_NOT_SAFE),
                        MB_OK | MB_ICONERROR, options->silent);
                }
            }
            if (installer_set_metadata_defaults(&metadata, &paths, 0) != 0) {
                jvman_installer_metadata_free(&metadata);
                return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_NO_MEMORY),
                                         MB_OK | MB_ICONERROR, options->silent);
            }
        }
    } else if (options->install_dir_set) {
        installer_copy(default_install, sizeof(default_install) / sizeof(*default_install),
                       options->install_dir);
        if (jvman_install_paths_init(&paths, default_install, default_data) !=
            JVMAN_INSTALL_OK) {
            jvman_installer_metadata_free(&metadata);
            return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_DIR_NOT_SAFE),
                                     MB_OK | MB_ICONERROR, options->silent);
        }
    }
    install_status = jvman_install_paths_create(&paths);
    if (install_status != JVMAN_INSTALL_OK) {
        jvman_installer_metadata_free(&metadata);
        return installer_report_status(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_CREATE_DIR),
                                       jvman_lang_install_status(install_status),
                                       options->silent);
    }
    if (found && !options->portable) {
        install_status = jvman_install_backup_create(&paths);
        if (install_status != JVMAN_INSTALL_OK) goto install_failure;
        backup_created = 1;
    }
    install_status = jvman_setup_payload_open(NULL, &payload);
    if (install_status != JVMAN_INSTALL_OK) goto install_failure;
    install_status = jvman_setup_payload_extract(&payload, paths.jvman_path);
    if (install_status != JVMAN_INSTALL_OK) goto install_failure;
    if (!options->portable) {
        install_status = jvman_install_copy_self_uninstaller(&paths);
        if (install_status != JVMAN_INSTALL_OK ||
            jvman_install_marker_write(&paths, metadata.install_id) != JVMAN_INSTALL_OK) {
            if (install_status == JVMAN_INSTALL_OK) install_status = JVMAN_INSTALL_IO_ERROR;
            goto install_failure;
        }
        jvman_setup_payload_close(&payload);
        path_scope_mask = installer_environment_path_scope_mask(
            options, &metadata);
        if ((path_scope_mask & installer_path_scope_bit(
                 JVMAN_ENV_SCOPE_USER)) != 0u) {
            env_status = jvman_environment_path_snapshot_capture(
                JVMAN_ENV_SCOPE_USER, &user_path_snapshot);
            if (env_status != JVMAN_ENV_OK) goto environment_failure;
        }
        if ((path_scope_mask & installer_path_scope_bit(
                 JVMAN_ENV_SCOPE_MACHINE)) != 0u) {
            env_status = jvman_environment_path_snapshot_capture(
                JVMAN_ENV_SCOPE_MACHINE, &machine_path_snapshot);
            if (env_status != JVMAN_ENV_OK) goto environment_failure;
        }
        if (options->configure_java == 1 ||
            (options->configure_java == -1 && metadata.java_home_owned)) {
            env_status = jvman_environment_java_home_snapshot_capture(
                &java_home_snapshot);
            if (env_status != JVMAN_ENV_OK) goto environment_failure;
        }
        env_status = (JvmanEnvironmentStatus)installer_apply_environment(
            options, &paths, &metadata, &changed, &java_home_changed,
            &user_path_written, &machine_path_written, &java_home_written);
        if (env_status != JVMAN_ENV_OK) {
            rollback_status = installer_rollback_environment(
                &user_path_snapshot, &machine_path_snapshot,
                &user_path_written, &machine_path_written,
                &java_home_snapshot, &java_home_written);
            if (rollback_status != JVMAN_ENV_OK) env_status = rollback_status;
            goto environment_failure;
        }
        if (installer_set_field(&metadata.version, JVMAN_VERSION_W) != 0 ||
            installer_set_field(&metadata.install_dir, paths.install_dir) != 0 ||
            installer_set_field(&metadata.data_home, paths.data_home) != 0 ||
            installer_set_field(&metadata.language,
                                installer_language_tag()) != 0) {
            env_status = JVMAN_ENV_NO_MEMORY;
            rollback_status = installer_rollback_environment(
                &user_path_snapshot, &machine_path_snapshot,
                &user_path_written, &machine_path_written,
                &java_home_snapshot, &java_home_written);
            if (rollback_status != JVMAN_ENV_OK) env_status = rollback_status;
            goto environment_failure;
        }
        env_status = jvman_installer_metadata_save_scoped(metadata_scope,
                                                          &metadata);
        if (env_status != JVMAN_ENV_OK) {
            rollback_status = installer_rollback_environment(
                &user_path_snapshot, &machine_path_snapshot,
                &user_path_written, &machine_path_written,
                &java_home_snapshot, &java_home_written);
            if (rollback_status != JVMAN_ENV_OK) env_status = rollback_status;
            goto environment_failure;
        }
        jvman_environment_path_snapshot_free(&user_path_snapshot);
        jvman_environment_path_snapshot_free(&machine_path_snapshot);
        jvman_environment_path_snapshot_free(&user_path_written);
        jvman_environment_path_snapshot_free(&machine_path_written);
        jvman_environment_value_snapshot_free(&java_home_snapshot);
        jvman_environment_value_snapshot_free(&java_home_written);
        {
            wchar_t uninstall_command[JVMAN_INSTALL_PATH_CHARS + 32];
            if (installer_format(uninstall_command,
                    sizeof(uninstall_command) / sizeof(*uninstall_command),
                    options->machine_mode
                        ? L"\"%s\" /UNINSTALL /MACHINE"
                        : L"\"%s\" /UNINSTALL",
                    paths.uninstall_path) != 0) {
                env_status = JVMAN_ENV_TOO_LONG;
            } else {
                env_status = jvman_arp_write_scoped(
                    metadata_scope, JVMAN_VERSION_W, paths.install_dir,
                    uninstall_command);
            }
            if (env_status != JVMAN_ENV_OK) {
                (void)installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                    jvman_lang_environment_status(env_status),
                    options->silent);
                result = 2;
            }
        }
        if (backup_created) {
            (void)jvman_install_backup_discard(&paths);
            backup_created = 0;
        }
        if (changed) (void)jvman_environment_broadcast_change();
        if (options->discover && installer_run_discover(&paths) != 0) {
            if (!options->silent) {
                installer_message_box(
                    jvman_lang_str(JVMAN_STR_DISCOVER_FAILED_DETAIL),
                    jvman_lang_str(JVMAN_STR_DISCOVER_FAILED_TITLE), MB_OK | MB_ICONWARNING);
            }
            result = 2;
        }
    } else {
        jvman_setup_payload_close(&payload);
    }
    jvman_installer_metadata_free(&metadata);
    if (result == 1) result = 0;
    return result;

environment_failure:
    jvman_environment_path_snapshot_free(&user_path_snapshot);
    jvman_environment_path_snapshot_free(&machine_path_snapshot);
    jvman_environment_path_snapshot_free(&user_path_written);
    jvman_environment_path_snapshot_free(&machine_path_written);
    jvman_environment_value_snapshot_free(&java_home_snapshot);
    jvman_environment_value_snapshot_free(&java_home_written);
    if (backup_created) {
        install_status = jvman_install_backup_restore(&paths);
        if (install_status != JVMAN_INSTALL_OK) {
            jvman_installer_metadata_free(&metadata);
            return installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_INSTALL),
                jvman_lang_install_status(install_status), options->silent);
        }
        backup_created = 0;
    }
    if (!found && !options->portable) {
        (void)jvman_arp_delete_scoped(metadata_scope);
        (void)jvman_installer_metadata_delete_scoped(metadata_scope);
        (void)jvman_install_uninstall(&paths);
    }
    jvman_installer_metadata_free(&metadata);
    return installer_report_status(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                                   jvman_lang_environment_status(env_status),
                                   options->silent);

install_failure:
    jvman_setup_payload_close(&payload);
    if (backup_created) {
        JvmanInstallStatus restore_status =
            jvman_install_backup_restore(&paths);
        if (restore_status != JVMAN_INSTALL_OK) {
            install_status = restore_status;
        }
        backup_created = 0;
    }
    if (!found && !options->portable) {
        (void)jvman_arp_delete_scoped(metadata_scope);
        (void)jvman_installer_metadata_delete_scoped(metadata_scope);
        (void)jvman_install_uninstall(&paths);
    }
    jvman_installer_metadata_free(&metadata);
    return installer_report_status(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_INSTALL),
                                   jvman_lang_install_status(install_status),
                                   options->silent);
}

static int installer_uninstall(InstallerOptions *options) {
    JvmanInstallerMetadata metadata;
    JvmanInstallPaths paths;
    JvmanInstallPaths expected_machine_paths;
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    int found = 0;
    int changed = 0;
    int result = 0;
    int environment_failed = 0;
    int self_cleanup = 0;
    JvmanEnvironmentStatus first_environment_error = JVMAN_ENV_OK;
    int step_changed = 0;
    JvmanEnvironmentStatus env_status;
    JvmanInstallStatus install_status;
    JvmanEnvironmentScope metadata_scope;
    const wchar_t *success_message;
    if (!options) return 1;
    if (options->machine_mode &&
        (options->remove_data || options->remove_jdks)) {
        return installer_report(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_INVALID_ARGS),
            MB_OK | MB_ICONERROR, options->silent);
    }
    /* Machine installations are shared, while CLI data is resolved per
     * invoking user. Never infer a user's removable data tree from HKLM. */
    if (options->machine_mode) options->uninstall_scope_set = 1;
    metadata_scope = options->machine_mode ? JVMAN_ENV_SCOPE_MACHINE
                                           : JVMAN_ENV_SCOPE_USER;
    if (options->machine_mode &&
        jvman_install_paths_machine_default(&expected_machine_paths) !=
            JVMAN_INSTALL_OK) {
        return installer_report(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_RECORD_INVALID),
            MB_OK | MB_ICONERROR, options->silent);
    }
    jvman_installer_metadata_init(&metadata);
    env_status = jvman_installer_metadata_load_scoped(
        metadata_scope, &metadata, &found);
    if (env_status != JVMAN_ENV_OK || !found || !metadata.install_dir ||
        !metadata.data_home || !metadata.install_id ||
        (options->machine_mode &&
         (metadata.java_home_owned || metadata.java_path_owned ||
          metadata.app_path_scope !=
              (uint32_t)JVMAN_ENV_SCOPE_MACHINE)) ||
        jvman_install_paths_init(&paths, metadata.install_dir,
                                 metadata.data_home) != JVMAN_INSTALL_OK ||
        (options->machine_mode &&
         (_wcsicmp(paths.install_dir,
                   expected_machine_paths.install_dir) != 0 ||
          _wcsicmp(paths.data_home, expected_machine_paths.data_home) != 0)) ||
        jvman_install_marker_read(&paths, marker, sizeof(marker) / sizeof(*marker)) !=
            JVMAN_INSTALL_OK || _wcsicmp(marker, metadata.install_id) != 0) {
        jvman_installer_metadata_free(&metadata);
        return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_UNINSTALL_RECORD_INVALID),
                                 MB_OK | MB_ICONERROR, options->silent);
    }
    if (!options->machine_mode && options->legacy_cleanup_applied) {
        env_status = installer_clear_legacy_metadata(options, &metadata);
        if (env_status != JVMAN_ENV_OK) {
            jvman_installer_metadata_free(&metadata);
            return installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_READ_STATE),
                jvman_lang_environment_status(env_status), options->silent);
        }
    }
    if (!options->silent && !options->uninstall_confirmed) {
        int confirm_result = installer_confirm_uninstall(options);
        if (confirm_result != 0) {
            jvman_installer_metadata_free(&metadata);
            if (confirm_result > 0) return 0;
            return installer_report(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_UNINSTALL_SCOPE_FAILED),
                MB_OK | MB_ICONERROR, options->silent);
        }
    }
    if (metadata.java_home_owned) {
        step_changed = 0;
        env_status = jvman_environment_restore_java_home(
            &metadata, &step_changed, NULL);
        if (env_status != JVMAN_ENV_OK) {
            result = 1;
            environment_failed = 1;
            if (first_environment_error == JVMAN_ENV_OK) {
                first_environment_error = env_status;
            }
        } else {
            metadata.java_home_owned = 0;
            changed |= step_changed;
        }
    }
    if (metadata.java_path_owned) {
        wchar_t java_bin[JVMAN_INSTALL_PATH_CHARS];
        if (installer_join(java_bin, sizeof(java_bin) / sizeof(*java_bin),
                           paths.data_home, L"current\\bin") != 0) {
            env_status = JVMAN_ENV_TOO_LONG;
        } else {
            step_changed = 0;
            env_status = jvman_environment_remove_path(
                installer_path_scope_from_metadata(metadata.java_path_scope),
                java_bin, 1, &step_changed, NULL);
        }
        if (env_status != JVMAN_ENV_OK) {
            result = 1;
            environment_failed = 1;
            if (first_environment_error == JVMAN_ENV_OK) {
                first_environment_error = env_status;
            }
        } else {
            metadata.java_path_owned = 0;
            changed |= step_changed;
        }
    }
    if (metadata.app_path_owned) {
        step_changed = 0;
        env_status = jvman_environment_remove_path(
            installer_path_scope_from_metadata(metadata.app_path_scope),
            paths.install_dir, 1, &step_changed, NULL);
        if (env_status != JVMAN_ENV_OK) {
            result = 1;
            environment_failed = 1;
            if (first_environment_error == JVMAN_ENV_OK) {
                first_environment_error = env_status;
            }
        } else {
            metadata.app_path_owned = 0;
            changed |= step_changed;
        }
    }

    /* Do not discard the ownership record when an environment update fails.
     * Keeping the marker, uninstaller, and metadata makes the operation
     * retryable and avoids leaving an untracked PATH/JAVA_HOME entry. */
    if (environment_failed) {
        if (changed) (void)jvman_environment_broadcast_change();
        jvman_installer_metadata_free(&metadata);
        return installer_report_status(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_ENV_FAILED),
            jvman_lang_environment_status(first_environment_error),
            options->silent);
    }

    if (changed) (void)jvman_environment_broadcast_change();
    if (options->remove_data) {
        install_status = jvman_install_remove_data(
            &paths, options->remove_jdks);
        if (install_status != JVMAN_INSTALL_OK &&
            install_status != JVMAN_INSTALL_NOT_FOUND) {
            jvman_installer_metadata_free(&metadata);
            return installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_UNINSTALL_DATA_FAILED),
                jvman_lang_install_status(install_status),
                options->silent);
        }
    }
    /* Remove the functional payload first, but retain the authenticated
     * marker and uninstaller until both registry records are committed. */
    install_status = jvman_install_remove_payload(&paths);
    if (install_status != JVMAN_INSTALL_OK && install_status != JVMAN_INSTALL_NOT_FOUND) {
        jvman_installer_metadata_free(&metadata);
        return installer_report_status(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_FILES_FAILED),
            jvman_lang_install_status(install_status),
            options->silent);
    }
    self_cleanup = installer_current_module_matches(paths.uninstall_path);
    if (self_cleanup &&
        installer_start_self_cleanup(&paths, metadata.install_id,
                                     metadata_scope) != 0) {
        jvman_installer_metadata_free(&metadata);
        return installer_report_status(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_FILES_FAILED),
            jvman_lang_install_status(JVMAN_INSTALL_IO_ERROR),
            options->silent);
    }
    env_status = jvman_arp_delete_scoped(metadata_scope);
    if (env_status != JVMAN_ENV_OK && env_status != JVMAN_ENV_NOT_FOUND) {
        result = 1;
    } else {
        env_status = jvman_installer_metadata_delete_scoped(metadata_scope);
        if (env_status != JVMAN_ENV_OK && env_status != JVMAN_ENV_NOT_FOUND) {
            result = 1;
        }
    }
    if (result == 0 && !self_cleanup) {
        install_status = jvman_install_uninstall(&paths);
        if (install_status != JVMAN_INSTALL_OK &&
            install_status != JVMAN_INSTALL_NOT_FOUND) {
            result = 1;
        }
    }
    jvman_installer_metadata_free(&metadata);
    success_message = options->remove_jdks
                          ? jvman_lang_str(JVMAN_STR_UNINSTALL_SUCCESS_ALL)
                          : options->remove_data
                                ? jvman_lang_str(
                                      JVMAN_STR_UNINSTALL_SUCCESS_DATA)
                                : jvman_lang_str(JVMAN_STR_UNINSTALL_SUCCESS);
    if (!options->silent) {
        installer_message_box(
            result == 0 ? success_message
                        : jvman_lang_str(JVMAN_STR_UNINSTALL_PARTIAL),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_OK | (result == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return result;
}

static int installer_browse_directory(wchar_t *out, size_t capacity) {
    BROWSEINFOW browse;
    LPITEMIDLIST item;
    int result = -1;
    if (!out || capacity == 0) return -1;
    memset(&browse, 0, sizeof(browse));
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    browse.lpszTitle = jvman_lang_str(JVMAN_STR_BROWSE_TITLE);
    item = SHBrowseForFolderW(&browse);
    if (item) {
        if (SHGetPathFromIDListW(item, out) && wcslen(out) < capacity) result = 0;
        CoTaskMemFree(item);
    }
    return result;
}

static int installer_prepare_gui_options(InstallerOptions *options) {
    JvmanInstallPaths paths;
    JvmanInstallPaths recorded_paths;
    JvmanInstallerMetadata metadata;
    int found = 0;
    wchar_t message[INSTALLER_TEXT_MAX];
    int answer;
    if (!options || jvman_install_paths_default(&paths) != JVMAN_INSTALL_OK) return -1;
    jvman_installer_metadata_init(&metadata);
    if (jvman_installer_metadata_load(&metadata, &found) == JVMAN_ENV_OK && found &&
        metadata.install_dir && metadata.data_home &&
        wcslen(metadata.install_dir) < sizeof(options->install_dir) /
                                             sizeof(*options->install_dir)) {
        installer_copy(options->install_dir,
                       sizeof(options->install_dir) / sizeof(*options->install_dir),
                       metadata.install_dir);
        options->install_dir_set = 1;
        if (jvman_install_paths_init(&recorded_paths, metadata.install_dir,
                                     metadata.data_home) == JVMAN_INSTALL_OK) {
            paths = recorded_paths;
        }
    } else {
        installer_copy(options->install_dir,
                       sizeof(options->install_dir) / sizeof(*options->install_dir),
                       paths.install_dir);
    }
    answer = installer_message_box(
        jvman_lang_str(JVMAN_STR_INSTALL_PROMPT),
        jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (answer == IDCANCEL) {
        jvman_installer_metadata_free(&metadata);
        return 1;
    }
    if (answer == IDNO) {
        if (installer_browse_directory(options->install_dir,
                sizeof(options->install_dir) / sizeof(*options->install_dir)) != 0) {
            jvman_installer_metadata_free(&metadata);
            return 1;
        }
        options->install_dir_set = 1;
    }
    answer = installer_message_box(
        jvman_lang_str(JVMAN_STR_ADD_PATH_PROMPT),
        jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    options->add_path = answer == IDYES;
    if (options->add_path) {
        answer = installer_message_box(
            jvman_lang_str(JVMAN_STR_PATH_SCOPE_PROMPT),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer == IDCANCEL) {
            jvman_installer_metadata_free(&metadata);
            return 1;
        }
        options->path_scope = answer == IDYES ? JVMAN_ENV_SCOPE_MACHINE
                                              : JVMAN_ENV_SCOPE_USER;
        options->path_scope_set = 1;
        if (options->path_scope == JVMAN_ENV_SCOPE_MACHINE) {
            if (jvman_install_paths_machine_default(&paths) != JVMAN_INSTALL_OK) {
                jvman_installer_metadata_free(&metadata);
                return -1;
            }
            options->machine_mode = 1;
            options->install_dir_set = 0;
            installer_copy(
                options->install_dir,
                sizeof(options->install_dir) / sizeof(*options->install_dir),
                paths.install_dir);
        }
    }
    if (!options->machine_mode &&
        jvman_install_paths_init(&paths, options->install_dir, paths.data_home) ==
        JVMAN_INSTALL_OK) {
        answer = installer_message_box(
            jvman_lang_str(JVMAN_STR_CONFIGURE_JAVA_PROMPT),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        options->configure_java = answer == IDYES;
        options->replace_java_home = options->configure_java;
    }
    if (!options->machine_mode) {
        answer = installer_message_box(
            jvman_lang_str(JVMAN_STR_DISCOVER_PROMPT),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        options->discover = answer == IDYES;
    }
    jvman_installer_metadata_free(&metadata);
    if (installer_format(message, sizeof(message) / sizeof(*message),
                         jvman_lang_str(JVMAN_STR_INSTALL_LOCATION), options->install_dir) != 0) return -1;
    (void)message;
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    PWSTR command_line, int show_command) {
    int argc;
    wchar_t **argv = NULL;
    InstallerOptions options;
    InstallerSourceLock source_lock;
    HANDLE instance_mutex = NULL;
    HANDLE machine_lock = NULL;
    int already_running = 0;
    int language_result;
    int parse_result;
    int auto_uninstall_result;
    int legacy_prepare_result;
    int legacy_cleanup_completed = 0;
    unsigned int legacy_removed_flags = 0u;
    int operation_committed = 0;
    int result = 2;
    int elevation_cancelled = 0;
    int elevation_launched = 0;
    HRESULT com_status;
    int com_initialized;
    JvmanEnvironmentStatus env_status;
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show_command;
    /* 关闭当前目录参与可执行文件搜索，防御 CreateProcess(NULL, command, ...) 时的 PATH 注入。 */
    (void)SetSearchPathMode(
        BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
    jvman_lang_use_system_default();
    installer_source_lock_init(&source_lock);
    com_status = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    com_initialized = SUCCEEDED(com_status);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        if (com_initialized) CoUninitialize();
        return 2;
    }
    if (argc > 1 && installer_is_switch(argv[1], L"/CLEANUP")) {
        result = installer_cleanup_worker(argc, argv);
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return result;
    }
    parse_result = installer_parse_options(argc, argv, &options);
    if (parse_result == 1) {
        if (options.language_set) (void)jvman_lang_set(options.language);
        if (!options.silent && !options.language_set) {
            language_result = jvman_lang_select_dialog();
            if (language_result != 0) {
                LocalFree(argv);
                if (com_initialized) CoUninitialize();
                return language_result > 0 ? 0 : 1;
            }
        }
        installer_show_usage();
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return 0;
    }
    if (parse_result != 0) {
        installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE),
                         jvman_lang_str(JVMAN_STR_INVALID_ARGS),
                         MB_OK | MB_ICONERROR, options.silent);
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return 2;
    }
    if (options.language_set && jvman_lang_set(options.language) != 0) {
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return 2;
    }
    /* 0.5.0: Independent elevated helper for HKLM legacy Java PATH relocation
     * and its uninstall counterpart. Runs completely in the elevated child
     * process, does not touch any other install state, and exits directly. */
    if (options.java_hklm_relocate_only || options.java_hklm_restore_only) {
        JvmanEnvironmentStatus helper_status;
        if (!installer_process_is_elevated()) {
            installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                jvman_lang_environment_status(JVMAN_ENV_ACCESS_DENIED),
                options.silent);
            LocalFree(argv);
            if (com_initialized) CoUninitialize();
            return 2;
        }
        helper_status = options.java_hklm_relocate_only
                            ? installer_relocate_java_hklm_execute()
                            : installer_restore_java_hklm_execute();
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return helper_status == JVMAN_ENV_OK ? 0 : 2;
    }
    if (options.elevated_resume && !installer_process_is_elevated()) {
        installer_report_status(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
            jvman_lang_environment_status(JVMAN_ENV_ACCESS_DENIED),
            options.silent);
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return 1;
    }
    if (installer_source_lock_acquire(&source_lock) != 0) {
        result = installer_report_status(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_CANNOT_INSTALL),
            jvman_lang_install_status(JVMAN_INSTALL_PATH_REPARSE),
            options.silent);
        goto done;
    }
    /* Cleanup waits for this process, then reacquires the same locks before
     * deleting authenticated install files. */
    instance_mutex = options.elevated_resume
                         ? installer_acquire_single_instance_after_handoff(
                               &already_running)
                         : installer_acquire_single_instance(&already_running);
    if (!instance_mutex) {
        if (already_running) {
            installer_report(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_ALREADY_RUNNING),
                MB_OK | MB_ICONINFORMATION, options.silent);
        } else {
            installer_report(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_CREATE_INSTANCE_LOCK),
                MB_OK | MB_ICONERROR, options.silent);
        }
        result = already_running ? INSTALLER_ALREADY_RUNNING_EXIT : 2;
        goto done;
    }
    if (!options.silent && !options.elevated_resume &&
        !options.language_set) {
        language_result = jvman_lang_select_dialog();
        if (language_result != 0) {
            result = language_result > 0 ? 0 : 1;
            goto done;
        }
    }
    auto_uninstall_result = installer_detect_registered_uninstaller(
        argc, &options);
    if (auto_uninstall_result < 0) {
        installer_report(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_RECORD_INVALID),
            MB_OK | MB_ICONERROR, options.silent);
        result = 1;
        goto done;
    }
    if (!options.silent && !options.elevated_resume && !options.uninstall &&
        !options.portable) {
        parse_result = installer_prepare_gui_options(&options);
        if (parse_result != 0) {
            result = parse_result == 1 ? 0 : 1;
            goto done;
        }
    }
    if (!options.silent && !options.elevated_resume && options.uninstall) {
        int confirm_result = installer_confirm_uninstall(&options);
        if (confirm_result != 0) {
            if (confirm_result < 0) {
                result = installer_report(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_UNINSTALL_SCOPE_FAILED),
                    MB_OK | MB_ICONERROR, options.silent);
            } else {
                result = 0;
            }
            goto done;
        }
    }
    legacy_prepare_result = installer_prepare_legacy_cleanup(&options);
    if (legacy_prepare_result < 0) {
        installer_report(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_RECORD_INVALID),
            MB_OK | MB_ICONERROR, options.silent);
        result = 1;
        goto done;
    }
    if ((options.machine_mode || options.legacy_cleanup_only) &&
        !installer_process_is_elevated()) {
        result = installer_run_elevated_machine(
            &options, &instance_mutex, &elevation_cancelled,
            &elevation_launched, &legacy_cleanup_completed,
            &legacy_removed_flags);
        if (!elevation_launched) {
            if (elevation_cancelled) {
                result = options.silent ? 1 : 0;
            } else {
                result = installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_INSTALL),
                    jvman_lang_environment_status(JVMAN_ENV_ACCESS_DENIED),
                    options.silent);
            }
        }
        if (elevation_launched && options.legacy_cleanup_flags != 0u) {
            if (legacy_cleanup_completed && (result == 0 || result == 2)) {
                installer_accept_legacy_cleanup(&options);
            } else if (result == 0 || result == 2) {
                result = installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                    jvman_lang_environment_status(JVMAN_ENV_WIN32_ERROR),
                    options.silent);
            } else if (!options.machine_mode && legacy_cleanup_completed &&
                       legacy_removed_flags != 0u) {
                if (installer_rollback_legacy_cleanup(
                        &options, legacy_removed_flags,
                        &instance_mutex) == 0) {
                    legacy_removed_flags = 0u;
                    legacy_cleanup_completed = 0;
                } else {
                    (void)installer_report_status(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                        jvman_lang_environment_status(
                            JVMAN_ENV_WIN32_ERROR),
                        options.silent);
                }
            }
        }
        if (options.machine_mode) {
            operation_committed = result == 0 ||
                                  (!options.uninstall && result == 2);
            if (operation_committed && options.legacy_cleanup_flags != 0u &&
                legacy_cleanup_completed) {
                env_status = installer_commit_legacy_cleanup(&options);
                if (env_status != JVMAN_ENV_OK) {
                    result = installer_report_status(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                        jvman_lang_environment_status(env_status),
                        options.silent);
                    if (installer_rollback_legacy_cleanup(
                            &options, legacy_removed_flags,
                            &instance_mutex) == 0) {
                        legacy_removed_flags = 0u;
                        legacy_cleanup_completed = 0;
                    } else {
                        (void)installer_report_status(
                            jvman_lang_str(JVMAN_STR_APP_TITLE),
                            jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                            jvman_lang_environment_status(
                                JVMAN_ENV_WIN32_ERROR),
                            options.silent);
                    }
                }
            }
            if (!operation_committed && !options.uninstall &&
                legacy_cleanup_completed && legacy_removed_flags != 0u) {
                if (installer_rollback_legacy_cleanup(
                        &options, legacy_removed_flags, &instance_mutex) == 0) {
                    legacy_removed_flags = 0u;
                    legacy_cleanup_completed = 0;
                } else {
                    (void)installer_report_status(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                        jvman_lang_environment_status(
                            JVMAN_ENV_WIN32_ERROR),
                        options.silent);
                }
            }
            if (elevation_launched && operation_committed &&
                !options.uninstall) {
                env_status = installer_save_user_language();
                if (env_status != JVMAN_ENV_OK) {
                    result = installer_report_status(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                        jvman_lang_environment_status(env_status),
                        options.silent);
                } else if (!options.silent && result == 0) {
                    installer_message_box(
                        options.add_path
                            ? jvman_lang_str(JVMAN_STR_INSTALL_SUCCESS_PATH)
                            : jvman_lang_str(JVMAN_STR_INSTALL_SUCCESS),
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        MB_OK | MB_ICONINFORMATION);
                }
            }
            goto done;
        }
        if (!elevation_launched || result != 0 ||
            !legacy_cleanup_completed) {
            goto done;
        }
        instance_mutex = installer_acquire_single_instance_after_handoff(
            &already_running);
        if (!instance_mutex) {
            if (already_running) {
                installer_report(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_ALREADY_RUNNING),
                    MB_OK | MB_ICONINFORMATION, options.silent);
            } else {
                installer_report(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_CREATE_INSTANCE_LOCK),
                    MB_OK | MB_ICONERROR, options.silent);
            }
            result = already_running ? INSTALLER_ALREADY_RUNNING_EXIT : 2;
            if (installer_rollback_legacy_cleanup(
                    &options, legacy_removed_flags, &instance_mutex) != 0) {
                (void)installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                    jvman_lang_environment_status(JVMAN_ENV_WIN32_ERROR),
                    options.silent);
            } else {
                legacy_removed_flags = 0u;
                legacy_cleanup_completed = 0;
            }
            goto done;
        }
    }
    if (options.machine_mode ||
        (options.legacy_cleanup_flags != 0u &&
         !options.legacy_cleanup_applied)) {
        machine_lock = installer_acquire_machine_instance(
            options.elevated_resume, &already_running);
        if (!machine_lock) {
            if (already_running) {
                installer_report(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_ALREADY_RUNNING),
                    MB_OK | MB_ICONINFORMATION, options.silent);
            } else {
                installer_report(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_CREATE_INSTANCE_LOCK),
                    MB_OK | MB_ICONERROR, options.silent);
            }
            result = already_running ? INSTALLER_ALREADY_RUNNING_EXIT : 2;
            goto done;
        }
    }
    if (options.legacy_restore_only) {
        env_status = installer_restore_legacy_machine_paths(
            &options, options.legacy_cleanup_flags);
        if (env_status != JVMAN_ENV_OK) {
            result = installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                jvman_lang_environment_status(env_status), options.silent);
        } else {
            result = 0;
        }
        goto done;
    }
    if (options.legacy_cleanup_flags != 0u &&
        !options.legacy_cleanup_applied) {
        env_status = installer_remove_legacy_machine_paths(
            &options, &legacy_removed_flags);
        if (env_status != JVMAN_ENV_OK) {
            if (legacy_removed_flags != 0u &&
                installer_rollback_legacy_cleanup(
                    &options, legacy_removed_flags, &instance_mutex) == 0) {
                legacy_removed_flags = 0u;
            }
            result = installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                jvman_lang_environment_status(env_status),
                options.silent);
            goto done;
        }
        legacy_cleanup_completed = 1;
        if (options.elevated_resume && options.legacy_cleanup_only) {
            result = 0;
            goto done;
        }
        installer_accept_legacy_cleanup(&options);
    }
    if (options.uninstall) {
        result = installer_uninstall(&options);
    } else {
        result = installer_install(&options);
    }
    operation_committed = result == 0 ||
                          (!options.uninstall && result == 2);
    if (options.legacy_cleanup_applied) {
        if (operation_committed && options.machine_mode &&
            !options.elevated_resume) {
            env_status = installer_commit_legacy_cleanup(&options);
            if (env_status != JVMAN_ENV_OK) {
                result = installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                    jvman_lang_environment_status(env_status),
                    options.silent);
                if (installer_rollback_legacy_cleanup(
                        &options, legacy_removed_flags,
                        &instance_mutex) == 0) {
                    legacy_removed_flags = 0u;
                    legacy_cleanup_completed = 0;
                } else {
                    (void)installer_report_status(
                        jvman_lang_str(JVMAN_STR_APP_TITLE),
                        jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                        jvman_lang_environment_status(
                            JVMAN_ENV_WIN32_ERROR),
                        options.silent);
                }
            }
        } else if (!operation_committed && !options.uninstall &&
                   legacy_removed_flags != 0u) {
            if (installer_rollback_legacy_cleanup(
                    &options, legacy_removed_flags, &instance_mutex) != 0) {
                (void)installer_report_status(
                    jvman_lang_str(JVMAN_STR_APP_TITLE),
                    jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                    jvman_lang_environment_status(JVMAN_ENV_WIN32_ERROR),
                    options.silent);
            } else {
                legacy_removed_flags = 0u;
                legacy_cleanup_completed = 0;
            }
        }
    }
    if (!options.uninstall && operation_committed && !options.portable &&
        !options.elevated_resume) {
        env_status = installer_save_user_language();
        if (env_status != JVMAN_ENV_OK) {
            result = installer_report_status(
                jvman_lang_str(JVMAN_STR_APP_TITLE),
                jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                jvman_lang_environment_status(env_status), options.silent);
        }
    }
    /* 0.5.0: If the user asked for legacy Java HKLM PATH relocation, run it
     * now. Two dispatch paths keep this correct across install modes:
     *   - Machine install: we're already inside the elevated child process,
     *     so call the execute function directly. The main (non-elevated)
     *     wrapper of the machine install skips this and lets the elevated
     *     child handle it.
     *   - User-mode install: we're a non-elevated process, so preflight the
     *     HKLM Path (read-only, no admin needed) and only spawn the narrow
     *     UAC helper when there's actually something to move.
     */
    if (!options.uninstall && operation_committed && !options.portable &&
        options.relocate_legacy_java == 1) {
        int is_machine_elevated_child =
            options.machine_mode && options.elevated_resume;
        int is_user_main = !options.machine_mode && !options.elevated_resume;
        if (is_machine_elevated_child || is_user_main) {
            JvmanEnvironmentPathSnapshot machine_path;
            int needs_relocate = 0;
            jvman_environment_path_snapshot_init(&machine_path);
            if (jvman_environment_path_snapshot_capture(
                    JVMAN_ENV_SCOPE_MACHINE, &machine_path) == JVMAN_ENV_OK &&
                machine_path.present && machine_path.value) {
                wchar_t *reformed = NULL;
                int reformed_changed = 0;
                if (jvman_pathlist_move_java_family_to_end(
                        machine_path.value, &reformed, &reformed_changed) ==
                        JVMAN_PATHLIST_OK &&
                    reformed_changed) {
                    needs_relocate = 1;
                }
                free(reformed);
            }
            jvman_environment_path_snapshot_free(&machine_path);
            if (needs_relocate) {
                if (is_machine_elevated_child) {
                    (void)installer_relocate_java_hklm_execute();
                } else {
                    (void)installer_spawn_java_hklm_helper(0, options.silent);
                }
            }
        }
    }
    /* Symmetric restore on uninstall: only when metadata records that we
     * previously relocated. Metadata is HKLM-scoped, readable without admin. */
    if (options.uninstall && operation_committed && !options.portable) {
        int is_machine_elevated_child =
            options.machine_mode && options.elevated_resume;
        int is_user_main = !options.machine_mode && !options.elevated_resume;
        if (is_machine_elevated_child || is_user_main) {
            JvmanInstallerMetadata check_metadata;
            int check_found = 0;
            jvman_installer_metadata_init(&check_metadata);
            if (jvman_installer_metadata_load_scoped(
                    JVMAN_ENV_SCOPE_MACHINE, &check_metadata, &check_found) ==
                    JVMAN_ENV_OK &&
                check_found && check_metadata.legacy_java_hklm_owned) {
                if (is_machine_elevated_child) {
                    (void)installer_restore_java_hklm_execute();
                } else {
                    (void)installer_spawn_java_hklm_helper(1, options.silent);
                }
            }
            jvman_installer_metadata_free(&check_metadata);
        }
    }
    if (!options.uninstall && result == 0 && !options.silent &&
        !options.elevated_resume) {
        installer_message_box(
            options.add_path
                ? jvman_lang_str(JVMAN_STR_INSTALL_SUCCESS_PATH)
                : jvman_lang_str(JVMAN_STR_INSTALL_SUCCESS),
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            MB_OK | MB_ICONINFORMATION);
    }
done:
    if (options.elevated_resume &&
        (legacy_cleanup_completed || legacy_removed_flags != 0u) &&
        result >= 0 &&
        result <= (int)INSTALLER_LEGACY_CLEANUP_RESULT_MASK) {
        result = (int)INSTALLER_LEGACY_CLEANUP_EXIT_BASE +
                 (int)((legacy_removed_flags & INSTALLER_LEGACY_PATH_MASK) <<
                       INSTALLER_LEGACY_CLEANUP_RESULT_BITS) +
                 result;
    }
    if (instance_mutex) {
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
    }
    if (machine_lock) CloseHandle(machine_lock);
    installer_source_lock_release(&source_lock);
    if (argv) LocalFree(argv);
    if (com_initialized) CoUninitialize();
    return result;
}
