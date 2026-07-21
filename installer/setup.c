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

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

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
#define INSTALLER_ALREADY_RUNNING_EXIT 3

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
    int add_path;
    int path_scope_set;
    JvmanEnvironmentScope path_scope;
    int configure_java;
    int replace_java_home;
    int discover;
    int install_dir_set;
    wchar_t install_dir[JVMAN_INSTALL_PATH_CHARS];
} InstallerOptions;

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

static HANDLE installer_acquire_single_instance(int *already_running) {
    HANDLE mutex;
    DWORD error;
    if (!already_running) return NULL;
    *already_running = 0;
    mutex = CreateMutexW(NULL, TRUE, INSTALLER_MUTEX_NAME);
    if (!mutex) return NULL;
    error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        *already_running = 1;
        CloseHandle(mutex);
        return NULL;
    }
    return mutex;
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

static int installer_parse_options(int argc, wchar_t **argv,
                                   InstallerOptions *options) {
    int index;
    if (!options) return -1;
    memset(options, 0, sizeof(*options));
    options->add_path = 1;
    options->path_scope = JVMAN_ENV_SCOPE_USER;
    options->configure_java = 0;
    if (argc < 1 || argc > INSTALLER_MAX_ARGS || !argv) return -1;
    for (index = 1; index < argc; ++index) {
        const wchar_t *argument = argv[index];
        wchar_t value[JVMAN_INSTALL_PATH_CHARS];
        int parsed;
        if (!argument || wcslen(argument) >= JVMAN_INSTALL_PATH_CHARS) return -1;
        if (installer_is_switch(argument, L"/S") ||
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
        } else if (installer_is_switch(argument, L"/HELP") ||
                   installer_is_switch(argument, L"--help") ||
                   installer_is_switch(argument, L"/?")) {
            return 1;
        } else {
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
                              !options->install_dir_set)) {
        return -1;
    }
    if (options->uninstall && (options->portable || options->install_dir_set ||
                               options->add_path == 0 || options->configure_java != 0 ||
                               options->replace_java_home || options->discover ||
                               options->path_scope_set)) {
        return -1;
    }
    if (!options->add_path && options->path_scope_set) return -1;
    if (options->replace_java_home && options->configure_java != 1) return -1;
    return 0;
}

static void installer_show_usage(void) {
    MessageBoxW(NULL,
        jvman_lang_str(JVMAN_STR_USAGE),
        jvman_lang_str(JVMAN_STR_APP_TITLE), MB_OK | MB_ICONINFORMATION);
}

static int installer_current_jdk(const JvmanInstallPaths *paths) {
    wchar_t java_path[JVMAN_INSTALL_PATH_CHARS];
    wchar_t javac_path[JVMAN_INSTALL_PATH_CHARS];
    wchar_t current[JVMAN_INSTALL_PATH_CHARS];
    DWORD java_attributes;
    DWORD javac_attributes;
    if (!paths || installer_join(current, sizeof(current) / sizeof(*current),
                                 paths->data_home, L"current") != 0 ||
        installer_join(java_path, sizeof(java_path) / sizeof(*java_path),
                       current, L"bin\\java.exe") != 0 ||
        installer_join(javac_path, sizeof(javac_path) / sizeof(*javac_path),
                       current, L"bin\\javac.exe") != 0) return 0;
    java_attributes = GetFileAttributesW(java_path);
    javac_attributes = GetFileAttributesW(javac_path);
    return java_attributes != INVALID_FILE_ATTRIBUTES &&
           javac_attributes != INVALID_FILE_ATTRIBUTES &&
           (java_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (javac_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (java_attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           (javac_attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
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
    if (!silent) MessageBoxW(NULL, message, title, type);
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

static int installer_create_cleanup_copy(wchar_t path[], size_t path_capacity) {
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    wchar_t temp_directory[JVMAN_INSTALL_PATH_CHARS];
    wchar_t filename[96];
    HANDLE source = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    uint64_t remaining;
    unsigned char buffer[INSTALLER_COPY_BUFFER_SIZE];
    DWORD length;
    unsigned int attempt;
    if (!path || path_capacity == 0) return -1;
    length = GetModuleFileNameW(NULL, module,
                                (DWORD)(sizeof(module) / sizeof(module[0])));
    if (length == 0 || length >= sizeof(module) / sizeof(module[0])) return -1;
    length = GetTempPathW((DWORD)(sizeof(temp_directory) /
                                  sizeof(temp_directory[0])), temp_directory);
    if (length == 0 || length >= sizeof(temp_directory) /
                              sizeof(temp_directory[0])) return -1;
    source = CreateFileW(module, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
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
            installer_join(path, path_capacity, temp_directory, filename) != 0) {
            CloseHandle(source);
            return -1;
        }
        output = CreateFileW(
            path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, CREATE_NEW,
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
                                        const wchar_t *installation_id) {
    wchar_t helper[JVMAN_INSTALL_PATH_CHARS];
    wchar_t *command = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    int result = -1;
    if (!paths || !installation_id ||
        installer_create_cleanup_copy(helper,
            sizeof(helper) / sizeof(helper[0])) != 0) {
        return -1;
    }
    command = (wchar_t *)malloc(INSTALLER_COMMAND_CHARS * sizeof(*command));
    if (!command ||
        installer_format(command, INSTALLER_COMMAND_CHARS,
                         L"\"%s\" /CLEANUP %lu \"%s\" \"%s\" %s",
                         helper, (unsigned long)GetCurrentProcessId(),
                         paths->install_dir, paths->data_home,
                         installation_id) != 0) {
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

static int installer_mark_cleanup_helper_for_deletion(
    const wchar_t *module_path) {
    /* A running image cannot normally be unlinked on Windows.  Rename its
     * default data stream to a private ADS, then request POSIX deletion of
     * the now-unlinked image.  The strict temp-name check prevents the hidden
     * cleanup mode from being used against an arbitrary executable. */
    static const wchar_t prefix[] = L"jvman-cleanup-";
    wchar_t temp_directory[JVMAN_INSTALL_PATH_CHARS];
    wchar_t stream_name[64];
    const wchar_t *filename;
    size_t temp_length;
    size_t module_length;
    size_t stream_length;
    size_t rename_size;
    FILE_RENAME_INFO *rename_info = NULL;
    FILE_DISPOSITION_INFO disposition;
    JvmanFileDispositionInfoEx disposition_ex;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD length;
    int result = -1;
    if (!module_path) return -1;
    length = GetTempPathW((DWORD)(sizeof(temp_directory) /
                                  sizeof(temp_directory[0])), temp_directory);
    if (length == 0 || length >= sizeof(temp_directory) /
                              sizeof(temp_directory[0])) return -1;
    temp_length = wcslen(temp_directory);
    module_length = wcslen(module_path);
    if (module_length <= temp_length + sizeof(prefix) / sizeof(prefix[0]) ||
        _wcsnicmp(module_path, temp_directory, temp_length) != 0) {
        return -1;
    }
    filename = module_path + temp_length;
    if (_wcsnicmp(filename, prefix,
                  sizeof(prefix) / sizeof(prefix[0]) - 1u) != 0 ||
        wcschr(filename, L'\\') != NULL || wcschr(filename, L'/') != NULL ||
        wcslen(filename) < 4 ||
        _wcsicmp(filename + wcslen(filename) - 4, L".exe") != 0) {
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
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    wchar_t module[JVMAN_INSTALL_PATH_CHARS];
    DWORD parent_id;
    DWORD length;
    HANDLE parent;
    DWORD wait_result;
    DWORD attributes;
    JvmanInstallerMetadata metadata;
    int metadata_found = 0;
    JvmanEnvironmentStatus metadata_status;
    if (argc != 6 || !argv || !installer_is_switch(argv[1], L"/CLEANUP") ||
        installer_parse_process_id(argv[2], &parent_id) != 0 ||
        jvman_install_paths_init(&paths, argv[3], argv[4]) != JVMAN_INSTALL_OK ||
        jvman_install_marker_read(&paths, marker,
                                  sizeof(marker) / sizeof(marker[0])) !=
            JVMAN_INSTALL_OK ||
        _wcsicmp(marker, argv[5]) != 0) {
        return 2;
    }
    length = GetModuleFileNameW(NULL, module,
                                (DWORD)(sizeof(module) / sizeof(module[0])));
    if (length == 0 || length >= sizeof(module) / sizeof(module[0]) ||
        _wcsicmp(module, paths.uninstall_path) == 0) {
        return 2;
    }
    (void)installer_mark_cleanup_helper_for_deletion(module);
    parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
    if (parent) {
        wait_result = WaitForSingleObject(parent, INFINITE);
        CloseHandle(parent);
        if (wait_result != WAIT_OBJECT_0) return 1;
    } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
        return 1;
    }
    /* ERROR_INVALID_PARAMETER means the validated parent PID already exited
     * before this helper could open it. Metadata validation below still gates
     * every deletion, so the cleanup can safely continue in that race. */
    /* The parent removes its registry record only after all environment and
     * ARP operations succeed.  If it crashed or a registry write failed,
     * leave the authenticated files in place for a retry. */
    jvman_installer_metadata_init(&metadata);
    metadata_status = jvman_installer_metadata_load(&metadata, &metadata_found);
    jvman_installer_metadata_free(&metadata);
    if (metadata_status != JVMAN_ENV_OK || metadata_found ||
        installer_delete_file_retry(paths.uninstall_path) != 0 ||
        installer_delete_file_retry(paths.marker_path) != 0) {
        return 1;
    }
    attributes = GetFileAttributesW(paths.install_dir);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        !RemoveDirectoryW(paths.install_dir)) {
        DWORD error = GetLastError();
        if (error != ERROR_DIR_NOT_EMPTY && error != ERROR_FILE_NOT_FOUND &&
            error != ERROR_PATH_NOT_FOUND) return 1;
    }
    return 0;
}

static JvmanEnvironmentScope installer_path_scope_from_metadata(uint32_t scope) {
    return scope == (uint32_t)JVMAN_ENV_SCOPE_MACHINE
               ? JVMAN_ENV_SCOPE_MACHINE
               : JVMAN_ENV_SCOPE_USER;
}

static JvmanEnvironmentStatus installer_apply_path_entry(
    const wchar_t *directory, JvmanEnvironmentScope desired_scope, int add,
    int *owned, uint32_t *scope_value, int *changed_out) {
    JvmanEnvironmentScope current_scope;
    JvmanEnvironmentStatus status;
    int step_changed = 0;
    if (!directory || !owned || !scope_value || !changed_out ||
        (add != 0 && add != 1)) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *changed_out = 0;
    current_scope = installer_path_scope_from_metadata(*scope_value);
    if (add) {
        if (*owned && current_scope != desired_scope) {
            status = jvman_environment_remove_path(current_scope, directory, 1,
                                                   &step_changed);
            if (status != JVMAN_ENV_OK) return status;
            *owned = 0;
            *changed_out |= step_changed;
        }
        step_changed = 0;
        status = jvman_environment_add_path(desired_scope, directory, *owned,
                                            owned, &step_changed);
        if (status != JVMAN_ENV_OK) return status;
        *scope_value = (uint32_t)desired_scope;
        *changed_out |= step_changed;
    } else if (*owned) {
        status = jvman_environment_remove_path(current_scope, directory, 1,
                                               &step_changed);
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
                                       int *changed_out) {
    wchar_t current[JVMAN_INSTALL_PATH_CHARS];
    int changed = 0;
    int step_changed = 0;
    JvmanEnvironmentStatus status;
    if (!options || !paths || !metadata || !changed_out) return -1;
    *changed_out = 0;
    if (options->add_path) {
        status = installer_apply_path_entry(
            paths->install_dir, options->path_scope, 1,
            &metadata->app_path_owned, &metadata->app_path_scope,
            &step_changed);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    } else if (metadata->app_path_owned) {
        status = installer_apply_path_entry(
            paths->install_dir, options->path_scope, 0,
            &metadata->app_path_owned, &metadata->app_path_scope,
            &step_changed);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    }

    if (options->configure_java == 1) {
        if (!installer_current_jdk(paths)) return (int)JVMAN_ENV_CONFLICT;
        if (installer_join(current, sizeof(current) / sizeof(*current),
                           paths->data_home, L"current") != 0) return (int)JVMAN_ENV_TOO_LONG;
        status = jvman_environment_configure_java_home(
            current, metadata, options->replace_java_home, &step_changed);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
        if (installer_join(current, sizeof(current) / sizeof(*current),
                           current, L"bin") != 0) return (int)JVMAN_ENV_TOO_LONG;
        status = installer_apply_path_entry(
            current, options->path_scope, 1,
            &metadata->java_path_owned, &metadata->java_path_scope,
            &step_changed);
        if (status != JVMAN_ENV_OK) return (int)status;
        changed |= step_changed;
    } else {
        if (metadata->java_home_owned) {
            status = jvman_environment_restore_java_home(metadata, &step_changed);
            if (status != JVMAN_ENV_OK) return (int)status;
            metadata->java_home_owned = 0;
            changed |= step_changed;
        }
        if (metadata->java_path_owned) {
            if (installer_join(current, sizeof(current) / sizeof(*current),
                               paths->data_home, L"current\\bin") != 0) {
                return (int)JVMAN_ENV_TOO_LONG;
            }
            status = installer_apply_path_entry(
                current, options->path_scope, 0,
                &metadata->java_path_owned, &metadata->java_path_scope,
                &step_changed);
            if (status != JVMAN_ENV_OK) return (int)status;
            changed |= step_changed;
        }
    }
    *changed_out = changed;
    return 0;
}

static void installer_rollback_environment(const InstallerOptions *options,
                                           const JvmanInstallPaths *paths,
                                           JvmanInstallerMetadata *metadata,
                                           int old_app_owned, int old_java_owned,
                                           uint32_t old_app_scope,
                                           uint32_t old_java_scope,
                                           int old_java_home_owned,
                                           const wchar_t *old_java_home_value) {
    int changed;
    if (!options || !paths || !metadata) return;
    if (metadata->app_path_owned &&
        (!old_app_owned || metadata->app_path_scope != old_app_scope)) {
        (void)jvman_environment_remove_path(
            installer_path_scope_from_metadata(metadata->app_path_scope),
            paths->install_dir, 1, &changed);
        metadata->app_path_owned = 0;
    }
    if (old_app_owned) {
        metadata->app_path_scope = old_app_scope;
        (void)installer_apply_path_entry(
            paths->install_dir, installer_path_scope_from_metadata(old_app_scope),
            1, &metadata->app_path_owned, &metadata->app_path_scope, &changed);
    }
    if (metadata->java_path_owned &&
        (!old_java_owned || metadata->java_path_scope != old_java_scope)) {
        wchar_t java_bin[JVMAN_INSTALL_PATH_CHARS];
        if (installer_join(java_bin, sizeof(java_bin) / sizeof(*java_bin),
                           paths->data_home, L"current\\bin") == 0) {
            (void)jvman_environment_remove_path(
                installer_path_scope_from_metadata(metadata->java_path_scope),
                java_bin, 1, &changed);
        }
        metadata->java_path_owned = 0;
    }
    if (old_java_owned) {
        wchar_t java_bin[JVMAN_INSTALL_PATH_CHARS];
        if (installer_join(java_bin, sizeof(java_bin) / sizeof(*java_bin),
                           paths->data_home, L"current\\bin") == 0) {
            metadata->java_path_scope = old_java_scope;
            (void)installer_apply_path_entry(
                java_bin, installer_path_scope_from_metadata(old_java_scope),
                1, &metadata->java_path_owned, &metadata->java_path_scope,
                &changed);
        }
    }
    if (old_java_home_owned) {
        /* Restore the exact value that was managed before the transaction.
         * The current metadata may already contain a newly configured value,
         * so checking only the ownership bit is insufficient. */
        if (old_java_home_value) {
            (void)jvman_environment_configure_java_home(
                old_java_home_value, metadata, 1, &changed);
        }
    } else if (metadata->java_home_owned) {
        (void)jvman_environment_restore_java_home(metadata, &changed);
    }
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
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE,
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
    wchar_t default_install[JVMAN_INSTALL_PATH_CHARS];
    wchar_t default_data[JVMAN_INSTALL_PATH_CHARS];
    wchar_t marker_id[JVMAN_INSTALL_MARKER_ID_CHARS];
    int found = 0;
    int changed = 0;
    int old_app_owned;
    int old_java_owned;
    uint32_t old_app_scope;
    uint32_t old_java_scope;
    int old_java_home_owned;
    wchar_t *old_java_home_value = NULL;
    int result = 1;
    JvmanEnvironmentStatus env_status;
    JvmanInstallStatus install_status;
    if (!options) return 1;
    jvman_installer_metadata_init(&metadata);
    memset(&payload, 0, sizeof(payload));
    if (jvman_install_paths_default(&paths) != JVMAN_INSTALL_OK) {
        return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_DETERMINE_PATHS),
                                 MB_OK | MB_ICONERROR, options->silent);
    }
    installer_copy(default_install, sizeof(default_install) / sizeof(*default_install),
                   paths.install_dir);
    installer_copy(default_data, sizeof(default_data) / sizeof(*default_data),
                   paths.data_home);
    if (!options->portable) {
        env_status = jvman_installer_metadata_load(&metadata, &found);
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
    install_status = jvman_setup_payload_open(NULL, &payload);
    if (install_status != JVMAN_INSTALL_OK) goto install_failure;
    install_status = jvman_setup_payload_extract(&payload, paths.jvman_path);
    jvman_setup_payload_close(&payload);
    if (install_status != JVMAN_INSTALL_OK) goto install_failure;
    if (!options->portable) {
        install_status = jvman_install_copy_self_uninstaller(&paths);
        if (install_status != JVMAN_INSTALL_OK ||
            jvman_install_marker_write(&paths, metadata.install_id) != JVMAN_INSTALL_OK) {
            if (install_status == JVMAN_INSTALL_OK) install_status = JVMAN_INSTALL_IO_ERROR;
            goto install_failure;
        }
        old_app_owned = metadata.app_path_owned;
        old_java_owned = metadata.java_path_owned;
        old_app_scope = metadata.app_path_scope;
        old_java_scope = metadata.java_path_scope;
        old_java_home_owned = metadata.java_home_owned;
        if (old_java_home_owned) {
            if (!metadata.java_home_managed_value ||
                installer_set_field(&old_java_home_value,
                                    metadata.java_home_managed_value) != 0) {
                env_status = JVMAN_ENV_NO_MEMORY;
                goto environment_failure;
            }
        }
        env_status = (JvmanEnvironmentStatus)installer_apply_environment(
            options, &paths, &metadata, &changed);
        if (env_status != JVMAN_ENV_OK) {
            installer_rollback_environment(options, &paths, &metadata,
                                           old_app_owned, old_java_owned,
                                           old_app_scope, old_java_scope,
                                           old_java_home_owned,
                                           old_java_home_value);
            goto environment_failure;
        }
        if (installer_set_field(&metadata.version, JVMAN_VERSION_W) != 0 ||
            installer_set_field(&metadata.install_dir, paths.install_dir) != 0 ||
            installer_set_field(&metadata.data_home, paths.data_home) != 0) {
            installer_rollback_environment(options, &paths, &metadata,
                                           old_app_owned, old_java_owned,
                                           old_app_scope, old_java_scope,
                                           old_java_home_owned,
                                           old_java_home_value);
            env_status = JVMAN_ENV_NO_MEMORY;
            free(old_java_home_value);
            old_java_home_value = NULL;
            goto environment_failure;
        }
        env_status = jvman_installer_metadata_save(&metadata);
        if (env_status != JVMAN_ENV_OK) {
            installer_rollback_environment(options, &paths, &metadata,
                                           old_app_owned, old_java_owned,
                                           old_app_scope, old_java_scope,
                                           old_java_home_owned,
                                           old_java_home_value);
            free(old_java_home_value);
            old_java_home_value = NULL;
            goto environment_failure;
        }
        free(old_java_home_value);
        old_java_home_value = NULL;
        {
            wchar_t uninstall_command[JVMAN_INSTALL_PATH_CHARS + 32];
            if (installer_format(uninstall_command,
                    sizeof(uninstall_command) / sizeof(*uninstall_command),
                    L"\"%s\" /UNINSTALL", paths.uninstall_path) == 0) {
                (void)jvman_arp_write(JVMAN_VERSION_W, paths.install_dir,
                                      uninstall_command);
            }
        }
        if (changed) (void)jvman_environment_broadcast_change();
        if (options->discover && installer_run_discover(&paths) != 0) {
            if (!options->silent) {
                MessageBoxW(NULL,
                    jvman_lang_str(JVMAN_STR_DISCOVER_FAILED_DETAIL),
                    jvman_lang_str(JVMAN_STR_DISCOVER_FAILED_TITLE), MB_OK | MB_ICONWARNING);
            }
            result = 2;
        }
    }
    jvman_installer_metadata_free(&metadata);
    if (result == 1) result = 0;
    return result;

environment_failure:
    free(old_java_home_value);
    jvman_installer_metadata_free(&metadata);
    return installer_report_status(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_UPDATE_ENV),
                                   jvman_lang_environment_status(env_status),
                                   options->silent);

install_failure:
    jvman_setup_payload_close(&payload);
    jvman_installer_metadata_free(&metadata);
    return installer_report_status(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_CANNOT_INSTALL),
                                   jvman_lang_install_status(install_status),
                                   options->silent);
}

static int installer_uninstall(const InstallerOptions *options) {
    JvmanInstallerMetadata metadata;
    JvmanInstallPaths paths;
    wchar_t marker[JVMAN_INSTALL_MARKER_ID_CHARS];
    int found = 0;
    int changed = 0;
    int result = 0;
    int environment_failed = 0;
    JvmanEnvironmentStatus first_environment_error = JVMAN_ENV_OK;
    int step_changed = 0;
    JvmanEnvironmentStatus env_status;
    JvmanInstallStatus install_status;
    if (!options) return 1;
    jvman_installer_metadata_init(&metadata);
    env_status = jvman_installer_metadata_load(&metadata, &found);
    if (env_status != JVMAN_ENV_OK || !found || !metadata.install_dir ||
        !metadata.data_home || !metadata.install_id ||
        jvman_install_paths_init(&paths, metadata.install_dir,
                                 metadata.data_home) != JVMAN_INSTALL_OK ||
        jvman_install_marker_read(&paths, marker, sizeof(marker) / sizeof(*marker)) !=
            JVMAN_INSTALL_OK || _wcsicmp(marker, metadata.install_id) != 0) {
        jvman_installer_metadata_free(&metadata);
        return installer_report(jvman_lang_str(JVMAN_STR_APP_TITLE), jvman_lang_str(JVMAN_STR_UNINSTALL_RECORD_INVALID),
                                 MB_OK | MB_ICONERROR, options->silent);
    }
    if (!options->silent && MessageBoxW(NULL,
            jvman_lang_str(JVMAN_STR_UNINSTALL_CONFIRM),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        jvman_installer_metadata_free(&metadata);
        return 0;
    }
    if (metadata.java_home_owned) {
        step_changed = 0;
        env_status = jvman_environment_restore_java_home(&metadata, &step_changed);
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
                java_bin, 1, &step_changed);
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
            paths.install_dir, 1, &step_changed);
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
    install_status = jvman_install_uninstall(&paths);
    if (install_status == JVMAN_INSTALL_SELF_CLEANUP_REQUIRED) {
        if (installer_start_self_cleanup(&paths, metadata.install_id) == 0) {
            install_status = JVMAN_INSTALL_OK;
        } else {
            install_status = JVMAN_INSTALL_IO_ERROR;
        }
    }
    if (install_status != JVMAN_INSTALL_OK && install_status != JVMAN_INSTALL_NOT_FOUND) {
        jvman_installer_metadata_free(&metadata);
        return installer_report_status(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_FILES_FAILED),
            jvman_lang_install_status(install_status),
            options->silent);
    }
    env_status = jvman_arp_delete();
    if (env_status != JVMAN_ENV_OK && env_status != JVMAN_ENV_NOT_FOUND) {
        result = 1;
    } else {
        env_status = jvman_installer_metadata_delete();
        if (env_status != JVMAN_ENV_OK && env_status != JVMAN_ENV_NOT_FOUND) {
            result = 1;
        }
    }
    jvman_installer_metadata_free(&metadata);
    if (!options->silent) {
        MessageBoxW(NULL,
            result == 0 ? jvman_lang_str(JVMAN_STR_UNINSTALL_SUCCESS)
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
    answer = MessageBoxW(NULL,
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
    answer = MessageBoxW(NULL,
        jvman_lang_str(JVMAN_STR_ADD_PATH_PROMPT),
        jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    options->add_path = answer == IDYES;
    if (options->add_path) {
        answer = MessageBoxW(NULL,
            jvman_lang_str(JVMAN_STR_PATH_SCOPE_PROMPT),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (answer == IDCANCEL) {
            jvman_installer_metadata_free(&metadata);
            return 1;
        }
        options->path_scope = answer == IDYES ? JVMAN_ENV_SCOPE_MACHINE
                                              : JVMAN_ENV_SCOPE_USER;
        options->path_scope_set = 1;
    }
    if (jvman_install_paths_init(&paths, options->install_dir, paths.data_home) ==
        JVMAN_INSTALL_OK && installer_current_jdk(&paths)) {
        answer = MessageBoxW(NULL,
            jvman_lang_str(JVMAN_STR_CONFIGURE_JAVA_PROMPT),
            jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        options->configure_java = answer == IDYES;
        options->replace_java_home = options->configure_java;
    }
    answer = MessageBoxW(NULL,
        jvman_lang_str(JVMAN_STR_DISCOVER_PROMPT),
        jvman_lang_str(JVMAN_STR_APP_TITLE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    options->discover = answer == IDYES;
    jvman_installer_metadata_free(&metadata);
    if (installer_format(message, sizeof(message) / sizeof(*message),
                         jvman_lang_str(JVMAN_STR_INSTALL_LOCATION), options->install_dir) != 0) return -1;
    (void)message;
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    PWSTR command_line, int show_command) {
    int argc;
    wchar_t **argv;
    InstallerOptions options;
    HANDLE instance_mutex;
    int already_running;
    int language_result;
    int parse_result;
    int auto_uninstall_result;
    int result;
    HRESULT com_status;
    int com_initialized;
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show_command;
    jvman_lang_use_system_default();
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
        if (!options.silent) {
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
                         MB_OK | MB_ICONERROR, 0);
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return 2;
    }
    /* Serialize file and registry changes. The cleanup worker is excluded
     * because uninstall starts it while this process still owns the mutex. */
    instance_mutex = installer_acquire_single_instance(&already_running);
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
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return already_running ? INSTALLER_ALREADY_RUNNING_EXIT : 2;
    }
    if (!options.silent) {
        language_result = jvman_lang_select_dialog();
        if (language_result != 0) {
            ReleaseMutex(instance_mutex);
            CloseHandle(instance_mutex);
            LocalFree(argv);
            if (com_initialized) CoUninitialize();
            return language_result > 0 ? 0 : 1;
        }
    }
    auto_uninstall_result = installer_detect_registered_uninstaller(
        argc, &options);
    if (auto_uninstall_result < 0) {
        installer_report(
            jvman_lang_str(JVMAN_STR_APP_TITLE),
            jvman_lang_str(JVMAN_STR_UNINSTALL_RECORD_INVALID),
            MB_OK | MB_ICONERROR, options.silent);
        ReleaseMutex(instance_mutex);
        CloseHandle(instance_mutex);
        LocalFree(argv);
        if (com_initialized) CoUninitialize();
        return 1;
    }
    if (!options.silent && !options.uninstall && !options.portable) {
        parse_result = installer_prepare_gui_options(&options);
        if (parse_result != 0) {
            ReleaseMutex(instance_mutex);
            CloseHandle(instance_mutex);
            LocalFree(argv);
            if (com_initialized) CoUninitialize();
            return parse_result == 1 ? 0 : 1;
        }
    }
    if (options.uninstall) {
        result = installer_uninstall(&options);
    } else {
        result = installer_install(&options);
        if (result == 0 && !options.silent) {
            MessageBoxW(NULL,
                options.add_path
                    ? jvman_lang_str(JVMAN_STR_INSTALL_SUCCESS_PATH)
                    : jvman_lang_str(JVMAN_STR_INSTALL_SUCCESS),
                jvman_lang_str(JVMAN_STR_APP_TITLE), MB_OK | MB_ICONINFORMATION);
        }
    }
    ReleaseMutex(instance_mutex);
    CloseHandle(instance_mutex);
    LocalFree(argv);
    if (com_initialized) CoUninitialize();
    return result;
}
