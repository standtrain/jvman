#include "platform.h"

#include "sha256.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <winioctl.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <wchar.h>

/* Windows SDK headers hide these names behind a newer target macro. */
typedef struct JvmanFileDispositionInfoEx {
    DWORD flags;
} JvmanFileDispositionInfoEx;
#define JVMAN_FILE_DISPOSITION_INFO_EX_CLASS ((FILE_INFO_BY_HANDLE_CLASS)21)
#define JVMAN_FILE_DISPOSITION_FLAG_DELETE 0x00000001u
#define JVMAN_FILE_DISPOSITION_FLAG_POSIX 0x00000002u
#define JVMAN_FILE_DISPOSITION_FLAG_IGNORE_READONLY 0x00000010u
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

static char platform_error_buffer[512];

static void platform_set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(platform_error_buffer, sizeof(platform_error_buffer), format, args);
    va_end(args);
}

#if defined(_WIN32)
static void platform_set_windows_error(const char *action) {
    DWORD code = GetLastError();
    char message[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, message, (DWORD)sizeof(message), NULL);
    message[strcspn(message, "\r\n")] = '\0';
    platform_set_error("%s (Windows error %lu: %s)", action,
                       (unsigned long)code, message[0] ? message : "unknown error");
}
#endif

const char *platform_last_error(void) {
    return platform_error_buffer[0] ? platform_error_buffer : "unknown platform error";
}

#if defined(_WIN32)
static int platform_wide_to_utf8(const WCHAR *wide, char **utf8_out) {
    int required;
    char *utf8;
    if (!wide || !utf8_out) {
        platform_set_error("invalid UTF-16 conversion input");
        return -1;
    }
    required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                   NULL, 0, NULL, NULL);
    if (required <= 0) {
        platform_set_windows_error("cannot convert registry path to UTF-8");
        return -1;
    }
    utf8 = (char *)malloc((size_t)required);
    if (!utf8) {
        platform_set_error("out of memory converting registry path");
        return -1;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                            utf8, required, NULL, NULL) != required) {
        free(utf8);
        platform_set_windows_error("cannot convert registry path to UTF-8");
        return -1;
    }
    *utf8_out = utf8;
    return 0;
}

static int platform_utf8_to_wide(const char *utf8, WCHAR **wide_out) {
    int required;
    WCHAR *wide;
    if (!utf8 || !wide_out) {
        platform_set_error("invalid UTF-8 conversion input");
        return -1;
    }
    required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                   NULL, 0);
    if (required <= 0 || (size_t)required > JVMAN_PATH_MAX) {
        if (required <= 0) platform_set_windows_error("cannot convert UTF-8 path");
        else platform_set_error("UTF-8 path is too long");
        return -1;
    }
    wide = (WCHAR *)malloc((size_t)required * sizeof(*wide));
    if (!wide) {
        platform_set_error("out of memory converting UTF-8 path");
        return -1;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                            wide, required) != required) {
        free(wide);
        platform_set_windows_error("cannot convert UTF-8 path");
        return -1;
    }
    *wide_out = wide;
    return 0;
}

static int platform_copy_wide_to_utf8(const WCHAR *wide, char *out,
                                      size_t out_size) {
    char *utf8 = NULL;
    size_t length;
    if (!out || out_size == 0 || platform_wide_to_utf8(wide, &utf8) != 0) {
        return -1;
    }
    length = strlen(utf8);
    if (length + 1u > out_size) {
        free(utf8);
        platform_set_error("converted Windows path is too long");
        return -1;
    }
    memcpy(out, utf8, length + 1u);
    free(utf8);
    return 0;
}

static int platform_visit_java_registry_value(HKEY version_key,
                                               PlatformPathVisitor visitor,
                                               void *userdata) {
    DWORD type = 0;
    DWORD byte_count = 0;
    LSTATUS status;
    WCHAR *value = NULL;
    WCHAR *expanded = NULL;
    const WCHAR *path;
    char *utf8 = NULL;
    size_t allocation_size;
    int result;

    status = RegQueryValueExW(version_key, L"JavaHome", NULL, &type, NULL, &byte_count);
    if (status != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || byte_count == 0 ||
        (byte_count % sizeof(WCHAR)) != 0 ||
        (size_t)byte_count > (JVMAN_PATH_MAX - 1u) * sizeof(WCHAR)) {
        return 0;
    }
    allocation_size = (size_t)byte_count + sizeof(WCHAR);
    if (allocation_size < (size_t)byte_count) {
        platform_set_error("registry JavaHome value is too large");
        return -1;
    }
    value = (WCHAR *)calloc(1, allocation_size);
    if (!value) {
        platform_set_error("out of memory reading registry JavaHome");
        return -1;
    }
    status = RegQueryValueExW(version_key, L"JavaHome", NULL, &type,
                              (BYTE *)value, &byte_count);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        free(value);
        return 0;
    }
    {
        size_t character_count = (size_t)byte_count / sizeof(WCHAR);
        size_t path_length = character_count;
        size_t index;
        if (path_length != 0 && value[path_length - 1u] == L'\0') {
            --path_length;
        }
        for (index = 0; index < path_length; ++index) {
            if (value[index] == L'\0') {
                free(value);
                return 0;
            }
        }
        value[path_length] = L'\0';
    }
    path = value;
    if (type == REG_EXPAND_SZ) {
        DWORD char_count = ExpandEnvironmentStringsW(value, NULL, 0);
        size_t expanded_size;
        if (char_count == 0 ||
            (size_t)char_count > JVMAN_PATH_MAX) {
            free(value);
            if (char_count == 0) {
                platform_set_windows_error("cannot expand registry JavaHome");
            } else {
                platform_set_error("expanded registry JavaHome is too large");
            }
            return -1;
        }
        expanded_size = (size_t)char_count * sizeof(WCHAR);
        expanded = (WCHAR *)malloc(expanded_size);
        if (!expanded) {
            free(value);
            platform_set_error("out of memory expanding registry JavaHome");
            return -1;
        }
        if (ExpandEnvironmentStringsW(value, expanded, char_count) != char_count) {
            free(expanded);
            free(value);
            platform_set_windows_error("cannot expand registry JavaHome");
            return -1;
        }
        path = expanded;
    }
    if (*path == L'\0') {
        free(expanded);
        free(value);
        return 0;
    }
    if (platform_wide_to_utf8(path, &utf8) != 0) {
        free(expanded);
        free(value);
        return -1;
    }
    result = visitor(utf8, "oracle", userdata);
    free(utf8);
    free(expanded);
    free(value);
    return result;
}

static int platform_visit_java_registry_product(HKEY hive, REGSAM view,
                                                 const WCHAR *product_key,
                                                 PlatformPathVisitor visitor,
                                                 void *userdata) {
    HKEY key;
    DWORD max_name_length = 0;
    WCHAR *name;
    DWORD index;
    LSTATUS status;
    size_t name_count;
    int result = 0;

    status = RegOpenKeyExW(hive, product_key, 0, KEY_READ | view, &key);
    if (status != ERROR_SUCCESS) return 0;
    status = RegQueryInfoKeyW(key, NULL, NULL, NULL, NULL, &max_name_length,
                              NULL, NULL, NULL, NULL, NULL, NULL);
    if (status != ERROR_SUCCESS) {
        RegCloseKey(key);
        return 0;
    }
    name_count = (size_t)max_name_length + 1;
    if (name_count == 0 ||
        name_count * sizeof(WCHAR) / sizeof(WCHAR) != name_count) {
        RegCloseKey(key);
        platform_set_error("registry Java version name is too large");
        return -1;
    }
    name = (WCHAR *)malloc(name_count * sizeof(WCHAR));
    if (!name) {
        RegCloseKey(key);
        platform_set_error("out of memory enumerating Java registry keys");
        return -1;
    }
    for (index = 0;; ++index) {
        DWORD name_length = max_name_length + 1;
        HKEY version_key;
        status = RegEnumKeyExW(key, index, name, &name_length, NULL, NULL, NULL, NULL);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) break;
        status = RegOpenKeyExW(key, name, 0, KEY_READ, &version_key);
        if (status != ERROR_SUCCESS) continue;
        result = platform_visit_java_registry_value(version_key, visitor, userdata);
        RegCloseKey(version_key);
        if (result != 0) break;
    }
    free(name);
    RegCloseKey(key);
    return result;
}
#endif

int platform_visit_java_registry_homes(PlatformPathVisitor visitor, void *userdata) {
    if (!visitor) {
        platform_set_error("registry path visitor is required");
        return -1;
    }
#if defined(_WIN32)
    static const WCHAR *product_keys[] = {
        L"SOFTWARE\\JavaSoft\\JDK",
        L"SOFTWARE\\JavaSoft\\Java Development Kit",
        L"SOFTWARE\\JavaSoft\\JRE",
        L"SOFTWARE\\JavaSoft\\Java Runtime Environment"
    };
    static const HKEY hives[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    static const REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    size_t hive_index;
    size_t view_index;
    size_t product_index;
    for (hive_index = 0; hive_index < sizeof(hives) / sizeof(hives[0]); ++hive_index) {
        for (view_index = 0; view_index < sizeof(views) / sizeof(views[0]); ++view_index) {
            for (product_index = 0;
                 product_index < sizeof(product_keys) / sizeof(product_keys[0]);
                 ++product_index) {
                int result = platform_visit_java_registry_product(
                    hives[hive_index], views[view_index], product_keys[product_index],
                    visitor, userdata);
                if (result != 0) return result;
            }
        }
    }
#else
    (void)userdata;
#endif
    return 0;
}

static char *platform_duplicate(const char *text) {
    size_t length;
    char *copy;
    if (!text) return NULL;
    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}

int platform_default_root(char *out, size_t out_size) {
    const char *override = getenv("JVMAN_HOME");
    const char *base;
    if (override && *override) {
        if (strlen(override) + 1 > out_size) {
            platform_set_error("JVMAN_HOME is too long");
            return -1;
        }
        strcpy(out, override);
        jvman_strip_trailing_separators(out);
        return 0;
    }
#if defined(_WIN32)
    base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base || jvman_path_join(out, out_size, base, "jvman") != 0) {
        platform_set_error("LOCALAPPDATA and USERPROFILE are not available");
        return -1;
    }
#else
    base = getenv("XDG_DATA_HOME");
    if (base && *base && base[0] == '/') {
        if (jvman_path_join(out, out_size, base, "jvman") != 0) {
            platform_set_error("XDG_DATA_HOME is too long");
            return -1;
        }
    } else {
        base = getenv("HOME");
        if (!base || !*base ||
            jvman_path_join3(out, out_size, base, ".local/share", "jvman") != 0) {
            platform_set_error("HOME is not available");
            return -1;
        }
    }
#endif
    return 0;
}

int platform_absolute_path(const char *path, char *out, size_t out_size) {
    if (!path || !out) return -1;
#if defined(_WIN32)
    if (!_fullpath(out, path, out_size)) {
        platform_set_error("cannot resolve absolute path '%s': %s", path, strerror(errno));
        return -1;
    }
    if (platform_path_exists(out)) {
        HANDLE handle = CreateFileA(out, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            char resolved[JVMAN_PATH_MAX];
            DWORD length = GetFinalPathNameByHandleA(handle, resolved,
                                                     (DWORD)sizeof(resolved),
                                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(handle);
            if (length > 0 && length < sizeof(resolved)) {
                const char *normalized = resolved;
                if (strncmp(resolved, "\\\\?\\UNC\\", 8) == 0) {
                    if (strlen(resolved + 8) + 3 > out_size) return -1;
                    out[0] = '\\';
                    out[1] = '\\';
                    strcpy(out + 2, resolved + 8);
                } else {
                    if (strncmp(resolved, "\\\\?\\", 4) == 0) normalized += 4;
                    if (strlen(normalized) + 1 > out_size) return -1;
                    strcpy(out, normalized);
                }
            }
        }
    }
#else
    char resolved[JVMAN_PATH_MAX];
    if (realpath(path, resolved)) {
        if (strlen(resolved) + 1 > out_size) return -1;
        strcpy(out, resolved);
    } else if (path[0] == '/') {
        if (strlen(path) + 1 > out_size) return -1;
        strcpy(out, path);
    } else {
        char cwd[JVMAN_PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)) || jvman_path_join(out, out_size, cwd, path) != 0) {
            platform_set_error("cannot resolve absolute path '%s': %s", path, strerror(errno));
            return -1;
        }
    }
#endif
    jvman_strip_trailing_separators(out);
    return 0;
}

int platform_is_directory(const char *path) {
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

int platform_is_file(const char *path) {
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
#endif
}

int platform_path_exists(const char *path) {
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return lstat(path, &info) == 0;
#endif
}

static int platform_make_directory(const char *path) {
#if defined(_WIN32)
    if (CreateDirectoryA(path, NULL)) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS && platform_is_directory(path)) return 0;
    platform_set_windows_error("cannot create directory");
#else
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST && platform_is_directory(path)) return 0;
    platform_set_error("cannot create directory '%s': %s", path, strerror(errno));
#endif
    return -1;
}

int platform_mkdirs(const char *path) {
    char buffer[JVMAN_PATH_MAX];
    size_t i;
    size_t length;
    if (!path || strlen(path) >= sizeof(buffer)) {
        platform_set_error("directory path is too long");
        return -1;
    }
    strcpy(buffer, path);
    length = strlen(buffer);
    for (i = 1; i < length; ++i) {
#if defined(_WIN32)
        if (buffer[i] != '/' && buffer[i] != '\\') continue;
        if (i == 2 && buffer[1] == ':') continue;
        if (i < 2 && buffer[0] == '\\') continue;
#else
        if (buffer[i] != '/') continue;
#endif
        buffer[i] = '\0';
        if (*buffer && !platform_is_directory(buffer) && platform_make_directory(buffer) != 0) {
            return -1;
        }
        buffer[i] = JVMAN_DIR_SEP;
    }
    return platform_is_directory(buffer) ? 0 : platform_make_directory(buffer);
}

int platform_remove_file(const char *path) {
    if (!path) return -1;
#if defined(_WIN32)
    WCHAR *wide = NULL;
    DWORD attributes;
    if (platform_utf8_to_wide(path, &wide) != 0) return -1;
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND ||
         GetLastError() == ERROR_PATH_NOT_FOUND)) {
        free(wide);
        return 0;
    }
    if (DeleteFileW(wide)) {
        free(wide);
        return 0;
    }
    free(wide);
    platform_set_windows_error("cannot delete file");
#else
    if (!platform_path_exists(path)) return 0;
    if (unlink(path) == 0 || errno == ENOENT) return 0;
    platform_set_error("cannot delete file '%s': %s", path, strerror(errno));
#endif
    return -1;
}

int platform_copy_file(const char *source, const char *destination) {
#if defined(_WIN32)
    if (CopyFileA(source, destination, TRUE)) return 0;
    platform_set_windows_error("cannot copy file");
    return -1;
#else
    int input = open(source, O_RDONLY);
    int output;
    char buffer[64 * 1024];
    ssize_t count;
    if (input < 0) {
        platform_set_error("cannot open '%s': %s", source, strerror(errno));
        return -1;
    }
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output < 0) {
        platform_set_error("cannot create '%s': %s", destination, strerror(errno));
        close(input);
        return -1;
    }
    while ((count = read(input, buffer, sizeof(buffer))) != 0) {
        ssize_t written = 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            platform_set_error("cannot read '%s': %s", source, strerror(errno));
            goto failure;
        }
        while (written < count) {
            ssize_t result = write(output, buffer + written, (size_t)(count - written));
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                platform_set_error("cannot write '%s': %s", destination, strerror(errno));
                goto failure;
            }
            written += result;
        }
    }
    if (fsync(output) != 0) {
        int saved_error = errno;
        close(output);
        platform_set_error("cannot flush '%s': %s", destination, strerror(saved_error));
        close(input);
        unlink(destination);
        return -1;
    }
    if (close(output) != 0) {
        platform_set_error("cannot flush '%s': %s", destination, strerror(errno));
        close(input);
        unlink(destination);
        return -1;
    }
    close(input);
    return 0;
failure:
    close(input);
    close(output);
    unlink(destination);
    return -1;
#endif
}

#if defined(_WIN32)
static int platform_remove_tree_windows(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    WIN32_FIND_DATAA item;
    HANDLE search;
    char pattern[JVMAN_PATH_MAX];
    char child[JVMAN_PATH_MAX];
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return platform_remove_file(path);
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (RemoveDirectoryA(path)) return 0;
        platform_set_windows_error("cannot remove directory link");
        return -1;
    }
    if (jvman_path_join(pattern, sizeof(pattern), path, "*") != 0) return -1;
    search = FindFirstFileA(pattern, &item);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(item.cFileName, ".") == 0 || strcmp(item.cFileName, "..") == 0) continue;
            if (jvman_path_join(child, sizeof(child), path, item.cFileName) != 0) {
                FindClose(search);
                return -1;
            }
            if (platform_remove_tree_windows(child) != 0) {
                FindClose(search);
                return -1;
            }
        } while (FindNextFileA(search, &item));
        FindClose(search);
    }
    if (RemoveDirectoryA(path)) return 0;
    platform_set_windows_error("cannot remove directory");
    return -1;
}
#else
static int platform_remove_tree_posix(const char *path) {
    struct stat info;
    DIR *directory;
    struct dirent *item;
    char child[JVMAN_PATH_MAX];
    if (lstat(path, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
        if (unlink(path) == 0) return 0;
        platform_set_error("cannot remove '%s': %s", path, strerror(errno));
        return -1;
    }
    directory = opendir(path);
    if (!directory) return -1;
    while ((item = readdir(directory)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
        if (jvman_path_join(child, sizeof(child), path, item->d_name) != 0 ||
            platform_remove_tree_posix(child) != 0) {
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);
    if (rmdir(path) == 0) return 0;
    platform_set_error("cannot remove directory '%s': %s", path, strerror(errno));
    return -1;
}
#endif

int platform_remove_tree(const char *path) {
#if defined(_WIN32)
    return platform_remove_tree_windows(path);
#else
    return platform_remove_tree_posix(path);
#endif
}

int platform_move(const char *source, const char *destination) {
#if defined(_WIN32)
    if (MoveFileExA(source, destination, MOVEFILE_WRITE_THROUGH)) return 0;
    platform_set_windows_error("cannot move path");
#else
    if (rename(source, destination) == 0) return 0;
    platform_set_error("cannot move '%s' to '%s': %s", source, destination, strerror(errno));
#endif
    return -1;
}

int platform_write_text_atomic(const char *path, const char *text) {
    char temporary[JVMAN_PATH_MAX];
    FILE *file;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", path,
                 platform_process_id()) >= (int)sizeof(temporary)) {
        platform_set_error("temporary file path is too long");
        return -1;
    }
#if defined(_WIN32)
    file = fopen(temporary, "wb");
#else
    {
        int descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        file = descriptor >= 0 ? fdopen(descriptor, "wb") : NULL;
        if (!file && descriptor >= 0) close(descriptor);
    }
#endif
    if (!file) {
        platform_set_error("cannot write '%s': %s", temporary, strerror(errno));
        return -1;
    }
    if (fputs(text, file) == EOF || fflush(file) != 0) {
        platform_set_error("cannot flush '%s': %s", temporary, strerror(errno));
        fclose(file);
        platform_remove_file(temporary);
        return -1;
    }
#if defined(_WIN32)
    if (_commit(_fileno(file)) != 0) {
        platform_set_error("cannot flush '%s': %s", temporary, strerror(errno));
        fclose(file);
        platform_remove_file(temporary);
        return -1;
    }
#else
    if (fsync(fileno(file)) != 0) {
        platform_set_error("cannot flush '%s': %s", temporary, strerror(errno));
        fclose(file);
        platform_remove_file(temporary);
        return -1;
    }
#endif
    if (fclose(file) != 0) {
        platform_remove_file(temporary);
        return -1;
    }
#if defined(_WIN32)
    if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        platform_set_windows_error("cannot commit state file");
        platform_remove_file(temporary);
        return -1;
    }
#else
    if (rename(temporary, path) != 0) {
        platform_set_error("cannot commit state file '%s': %s", path, strerror(errno));
        platform_remove_file(temporary);
        return -1;
    }
#endif
    return 0;
}

int platform_read_line(const char *path, char *out, size_t out_size) {
    FILE *file;
    size_t length;
    if (!out || out_size == 0) return -1;
    file = fopen(path, "rb");
    if (!file) return -1;
    if (!fgets(out, (int)out_size, file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    length = strlen(out);
    while (length && (out[length - 1] == '\r' || out[length - 1] == '\n')) {
        out[--length] = '\0';
    }
    return 0;
}

int platform_list_directory(const char *path, char ***names_out, size_t *count_out) {
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    if (!names_out || !count_out) return -1;
#if defined(_WIN32)
    WIN32_FIND_DATAA item;
    HANDLE search;
    char pattern[JVMAN_PATH_MAX];
    if (jvman_path_join(pattern, sizeof(pattern), path, "*") != 0) return -1;
    search = FindFirstFileA(pattern, &item);
    if (search == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            *names_out = NULL; *count_out = 0; return 0;
        }
        platform_set_windows_error("cannot list directory");
        return -1;
    }
    do {
        char *copy;
        if (strcmp(item.cFileName, ".") == 0 || strcmp(item.cFileName, "..") == 0) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            char **grown = (char **)realloc(names, next * sizeof(*names));
            if (!grown) { FindClose(search); platform_free_directory_list(names, count); return -1; }
            names = grown; capacity = next;
        }
        copy = platform_duplicate(item.cFileName);
        if (!copy) { FindClose(search); platform_free_directory_list(names, count); return -1; }
        names[count++] = copy;
    } while (FindNextFileA(search, &item));
    FindClose(search);
#else
    DIR *directory = opendir(path);
    struct dirent *item;
    if (!directory) {
        if (errno == ENOENT) { *names_out = NULL; *count_out = 0; return 0; }
        platform_set_error("cannot list directory '%s': %s", path, strerror(errno));
        return -1;
    }
    while ((item = readdir(directory)) != NULL) {
        char *copy;
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            char **grown = (char **)realloc(names, next * sizeof(*names));
            if (!grown) { closedir(directory); platform_free_directory_list(names, count); return -1; }
            names = grown; capacity = next;
        }
        copy = platform_duplicate(item->d_name);
        if (!copy) { closedir(directory); platform_free_directory_list(names, count); return -1; }
        names[count++] = copy;
    }
    closedir(directory);
#endif
    *names_out = names;
    *count_out = count;
    return 0;
}

void platform_free_directory_list(char **names, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) free(names[i]);
    free(names);
}

#if defined(_WIN32)
typedef struct JvmanMountPointBuffer {
    DWORD reparse_tag;
    WORD reparse_data_length;
    WORD reserved;
    WORD substitute_name_offset;
    WORD substitute_name_length;
    WORD print_name_offset;
    WORD print_name_length;
    WCHAR path_buffer[JVMAN_PATH_MAX * 2];
} JvmanMountPointBuffer;

static int platform_create_junction(const char *link_path, const char *target_path) {
    WCHAR link_w[JVMAN_PATH_MAX];
    WCHAR target_w[JVMAN_PATH_MAX];
    WCHAR substitute[JVMAN_PATH_MAX + 8];
    JvmanMountPointBuffer buffer;
    HANDLE handle;
    DWORD returned;
    size_t substitute_chars;
    size_t print_chars;
    size_t substitute_bytes;
    size_t print_bytes;
    DWORD input_size;
    int target_length;
    int link_length;

    link_length = MultiByteToWideChar(CP_ACP, 0, link_path, -1, link_w, JVMAN_PATH_MAX);
    target_length = MultiByteToWideChar(CP_ACP, 0, target_path, -1, target_w, JVMAN_PATH_MAX);
    if (!link_length || !target_length) {
        platform_set_windows_error("cannot convert junction path");
        return -1;
    }
    if (target_w[0] == L'\\' && target_w[1] == L'\\') {
        if (swprintf(substitute, JVMAN_PATH_MAX + 8, L"\\??\\UNC\\%ls", target_w + 2) < 0) return -1;
    } else {
        if (swprintf(substitute, JVMAN_PATH_MAX + 8, L"\\??\\%ls", target_w) < 0) return -1;
    }
    if (!CreateDirectoryW(link_w, NULL)) {
        platform_set_windows_error("cannot create junction directory");
        return -1;
    }
    handle = CreateFileW(link_w, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        platform_set_windows_error("cannot open junction directory");
        RemoveDirectoryW(link_w);
        return -1;
    }
    memset(&buffer, 0, sizeof(buffer));
    substitute_chars = wcslen(substitute);
    print_chars = wcslen(target_w);
    substitute_bytes = substitute_chars * sizeof(WCHAR);
    print_bytes = print_chars * sizeof(WCHAR);
    if (substitute_bytes + print_bytes + 2 * sizeof(WCHAR) > sizeof(buffer.path_buffer)) {
        CloseHandle(handle);
        RemoveDirectoryW(link_w);
        platform_set_error("junction target is too long");
        return -1;
    }
    buffer.reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.substitute_name_offset = 0;
    buffer.substitute_name_length = (WORD)substitute_bytes;
    buffer.print_name_offset = (WORD)(substitute_bytes + sizeof(WCHAR));
    buffer.print_name_length = (WORD)print_bytes;
    memcpy(buffer.path_buffer, substitute, substitute_bytes);
    memcpy((char *)buffer.path_buffer + buffer.print_name_offset, target_w, print_bytes);
    buffer.reparse_data_length = (WORD)(8 + substitute_bytes + sizeof(WCHAR) +
                                        print_bytes + sizeof(WCHAR));
    input_size = 8 + buffer.reparse_data_length;
    if (!DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, input_size,
                         NULL, 0, &returned, NULL)) {
        platform_set_windows_error("cannot create directory junction");
        CloseHandle(handle);
        RemoveDirectoryW(link_w);
        return -1;
    }
    CloseHandle(handle);
    return 0;
}
#endif

int platform_is_directory_link(const char *path) {
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    HANDLE handle;
    unsigned char buffer[16 * 1024];
    DWORD returned;
    DWORD tag;
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, buffer,
                         (DWORD)sizeof(buffer), &returned, NULL)) {
        CloseHandle(handle);
        return 0;
    }
    CloseHandle(handle);
    memcpy(&tag, buffer, sizeof(tag));
    return tag == IO_REPARSE_TAG_MOUNT_POINT || tag == IO_REPARSE_TAG_SYMLINK;
#else
    struct stat info;
    return lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

int platform_replace_directory_link(const char *link_path, const char *target_path) {
    char temporary[JVMAN_PATH_MAX];
    if (!platform_is_directory(target_path)) {
        platform_set_error("link target is not a directory: %s", target_path);
        return -1;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.new.%lu", link_path,
                 platform_process_id()) >= (int)sizeof(temporary)) return -1;
    if (platform_path_exists(temporary)) {
        if (!platform_is_directory_link(temporary) || platform_remove_tree(temporary) != 0) {
            platform_set_error("stale switch path is not a managed directory link: %s", temporary);
            return -1;
        }
    }
#if defined(_WIN32)
    {
        char backup[JVMAN_PATH_MAX];
        int had_existing = platform_path_exists(link_path);
        if (had_existing && !platform_is_directory_link(link_path)) {
            platform_set_error("refusing to replace non-link path: %s", link_path);
            return -1;
        }
        if (snprintf(backup, sizeof(backup), "%s.old.%lu", link_path,
                     platform_process_id()) >= (int)sizeof(backup)) return -1;
        if (platform_path_exists(backup)) {
            if (!platform_is_directory_link(backup) || platform_remove_tree(backup) != 0) return -1;
        }
        if (platform_create_junction(temporary, target_path) != 0) return -1;
        if (had_existing && !MoveFileExA(link_path, backup, MOVEFILE_WRITE_THROUGH)) {
            platform_set_windows_error("cannot stage current junction");
            platform_remove_tree(temporary);
            return -1;
        }
        if (!MoveFileExA(temporary, link_path, MOVEFILE_WRITE_THROUGH)) {
            platform_set_windows_error("cannot activate current junction");
            if (had_existing) MoveFileExA(backup, link_path, MOVEFILE_WRITE_THROUGH);
            platform_remove_tree(temporary);
            return -1;
        }
        if (had_existing) {
            /* Activation already succeeded; a stale backup is safe to retry later. */
            platform_remove_tree(backup);
        }
    }
#else
    if (symlink(target_path, temporary) != 0) {
        platform_set_error("cannot create symlink '%s': %s", temporary, strerror(errno));
        return -1;
    }
    if (platform_path_exists(link_path) && !platform_is_directory_link(link_path)) {
        unlink(temporary);
        platform_set_error("refusing to replace non-link path: %s", link_path);
        return -1;
    }
    if (rename(temporary, link_path) != 0) {
        platform_set_error("cannot activate current symlink: %s", strerror(errno));
        unlink(temporary);
        return -1;
    }
#endif
    return 0;
}

/* Windows command discovery is separate from command-line quoting. */
#if defined(_WIN32)
static int windows_has_path_component(const char *name) {
    return name && (strchr(name, '\\') || strchr(name, '/') ||
                    (isalpha((unsigned char)name[0]) && name[1] == ':'));
}

static int windows_has_extension(const char *name) {
    const char *base = name;
    const char *cursor;
    for (cursor = name; cursor && *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') base = cursor + 1;
    }
    return base && strrchr(base, '.') != NULL;
}

static int windows_try_candidate(const char *base, const char *extension,
                                 char *out, size_t out_size) {
    char candidate[JVMAN_PATH_MAX];
    DWORD attributes;
    if (extension) {
        if (snprintf(candidate, sizeof(candidate), "%s%s", base, extension) >=
            (int)sizeof(candidate)) return -1;
    } else {
        if (strlen(base) >= sizeof(candidate)) return -1;
        strcpy(candidate, base);
    }
    attributes = GetFileAttributesA(candidate);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return -1;
    }
    return platform_absolute_path(candidate, out, out_size);
}

static int windows_try_pathext_candidate(const char *base, char *out, size_t out_size) {
    const char *extensions = getenv("PATHEXT");
    const char *cursor;
    if (windows_has_extension(base)) {
        return windows_try_candidate(base, NULL, out, out_size);
    }
    if (!extensions || !*extensions) extensions = ".COM;.EXE;.BAT;.CMD";
    cursor = extensions;
    while (*cursor) {
        const char *end = strchr(cursor, ';');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char extension[32];
        size_t i;
        if (length > 1 && length < sizeof(extension)) {
            memcpy(extension, cursor, length);
            extension[length] = '\0';
            for (i = 0; i < length; ++i) {
                if (!(isalnum((unsigned char)extension[i]) || extension[i] == '.')) break;
            }
            if (i == length && extension[0] == '.' &&
                windows_try_candidate(base, extension, out, out_size) == 0) return 0;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return windows_try_candidate(base, NULL, out, out_size);
}

static int windows_resolve_command(const char *name, char *out, size_t out_size) {
    char found[JVMAN_PATH_MAX];
    DWORD length;
    const char *extensions;
    const char *cursor;
    if (!name || !*name) return -1;
    if (windows_has_path_component(name)) {
        return windows_try_pathext_candidate(name, out, out_size);
    }
    if (windows_has_extension(name)) {
        length = SearchPathA(NULL, name, NULL, (DWORD)sizeof(found), found, NULL);
        if (length > 0 && length < sizeof(found)) {
            return platform_absolute_path(found, out, out_size);
        }
        return -1;
    }
    extensions = getenv("PATHEXT");
    if (!extensions || !*extensions) extensions = ".COM;.EXE;.BAT;.CMD";
    cursor = extensions;
    while (*cursor) {
        const char *end = strchr(cursor, ';');
        size_t extension_length = end ? (size_t)(end - cursor) : strlen(cursor);
        char extension[32];
        if (extension_length > 1 && extension_length < sizeof(extension)) {
            memcpy(extension, cursor, extension_length);
            extension[extension_length] = '\0';
            length = SearchPathA(NULL, name, extension, (DWORD)sizeof(found), found, NULL);
            if (length > 0 && length < sizeof(found) &&
                platform_absolute_path(found, out, out_size) == 0) return 0;
        }
        if (!end) break;
        cursor = end + 1;
    }
    length = SearchPathA(NULL, name, NULL, (DWORD)sizeof(found), found, NULL);
    if (length > 0 && length < sizeof(found)) {
        return platform_absolute_path(found, out, out_size);
    }
    return -1;
}

static int windows_resolve_trusted_command(const char *name, char *out, size_t out_size) {
    char directory[JVMAN_PATH_MAX];
    char candidate[JVMAN_PATH_MAX];
    UINT system_length;
    if (!name || !*name || windows_has_path_component(name)) return -1;
    system_length = GetSystemDirectoryA(directory, (UINT)sizeof(directory));
    if (system_length > 0 && system_length < sizeof(directory) &&
        jvman_path_join(candidate, sizeof(candidate), directory, name) == 0 &&
        windows_try_pathext_candidate(candidate, out, out_size) == 0) return 0;
    return -1;
}

static int windows_is_batch_file(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension && (_stricmp(extension, ".cmd") == 0 ||
                         _stricmp(extension, ".bat") == 0);
}
#endif

int platform_spawn_wait(char *const argv[]) {
    if (!argv || !argv[0]) return -1;
#if defined(_WIN32)
    size_t required = 1;
    size_t i;
    size_t argument_count = 0;
    char *command_line;
    char *output;
    char executable[JVMAN_PATH_MAX];
    char command_interpreter[JVMAN_PATH_MAX];
    char **effective_argv;
    int batch_file;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD exit_code;
    while (argv[argument_count]) ++argument_count;
    if (windows_resolve_command(argv[0], executable, sizeof(executable)) != 0) {
        platform_set_error("cannot find command '%s'", argv[0]);
        return -1;
    }
    batch_file = windows_is_batch_file(executable);
    effective_argv = (char **)calloc(argument_count + (batch_file ? 6 : 1), sizeof(char *));
    if (!effective_argv) return -1;
    if (batch_file) {
        if (windows_resolve_trusted_command("cmd.exe", command_interpreter,
                                            sizeof(command_interpreter)) != 0) {
            free(effective_argv);
            platform_set_error("cannot find the Windows command interpreter");
            return -1;
        }
        effective_argv[0] = command_interpreter;
        effective_argv[1] = "/d";
        effective_argv[2] = "/s";
        effective_argv[3] = "/c";
        effective_argv[4] = "call";
        effective_argv[5] = executable;
        for (i = 1; i < argument_count; ++i) effective_argv[i + 5] = argv[i];
    } else {
        effective_argv[0] = executable;
        for (i = 1; i < argument_count; ++i) effective_argv[i] = argv[i];
    }
    for (i = 0; effective_argv[i]; ++i) {
        size_t length = strlen(effective_argv[i]);
        if (length > (SIZE_MAX - required - 4) / 2) {
            platform_set_error("child command line is too long");
            free(effective_argv);
            return -1;
        }
        required += length * 2 + 4;
    }
    if (required > 32767) {
        platform_set_error("child command line is too long");
        free(effective_argv);
        return -1;
    }
    command_line = (char *)malloc(required);
    if (!command_line) { free(effective_argv); return -1; }
    output = command_line;
    for (i = 0; effective_argv[i]; ++i) {
        const char *input = effective_argv[i];
        int needs_quotes = *input == '\0' || strpbrk(input, " \t\n\v\"") != NULL ||
                           (batch_file && i >= 5);
        if (i) *output++ = ' ';
        if (!needs_quotes) {
            size_t length = strlen(input);
            memcpy(output, input, length);
            output += length;
            continue;
        }
        *output++ = '"';
        while (*input) {
            size_t backslashes = 0;
            while (*input == '\\') {
                ++backslashes;
                ++input;
            }
            if (*input == '"') {
                size_t count = backslashes * 2 + 1;
                while (count--) *output++ = '\\';
                *output++ = *input++;
            } else if (*input == '\0') {
                size_t count = backslashes * 2;
                while (count--) *output++ = '\\';
                break;
            } else {
                while (backslashes--) *output++ = '\\';
                *output++ = *input++;
            }
        }
        *output++ = '"';
    }
    *output = '\0';
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(effective_argv[0], command_line, NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process)) {
        platform_set_windows_error("cannot start child process");
        free(command_line);
        free(effective_argv);
        return -1;
    }
    free(command_line);
    free(effective_argv);
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code)) {
        platform_set_windows_error("cannot wait for child process");
        CloseHandle(process.hProcess);
        return -1;
    }
    CloseHandle(process.hProcess);
    return (int)exit_code;
#else
    pid_t child = fork();
    int status;
    if (child < 0) {
        platform_set_error("cannot fork: %s", strerror(errno));
        return -1;
    }
    if (child == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        platform_set_error("cannot wait for child process: %s", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

int platform_find_executable(const char *name, char *out, size_t out_size) {
#if defined(_WIN32)
    return windows_resolve_command(name, out, out_size);
#else
    const char *path = getenv("PATH");
    const char *cursor = path;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, ':');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char directory[JVMAN_PATH_MAX];
        char candidate[JVMAN_PATH_MAX];
        if (length == 0) strcpy(directory, ".");
        else if (length < sizeof(directory)) { memcpy(directory, cursor, length); directory[length] = '\0'; }
        else return -1;
        if (jvman_path_join(candidate, sizeof(candidate), directory, name) == 0 &&
            access(candidate, X_OK) == 0) {
            return platform_absolute_path(candidate, out, out_size);
        }
        if (!end) break;
        cursor = end + 1;
    }
    return -1;
#endif
}

int platform_find_trusted_executable(const char *name, char *out, size_t out_size) {
    if (!name || !*name || !out || out_size == 0) return -1;
#if defined(_WIN32)
    return windows_resolve_trusted_command(name, out, out_size);
#else
    static const char *const directories[] = {
        "/usr/bin", "/bin", "/usr/sbin", "/sbin",
        "/run/current-system/sw/bin"
    };
    size_t index;
    if (strchr(name, '/') != NULL) return -1;
    for (index = 0; index < sizeof(directories) / sizeof(directories[0]);
         ++index) {
        char candidate[JVMAN_PATH_MAX];
        char canonical[JVMAN_PATH_MAX];
        struct stat directory_info;
        struct stat file_info;
        if (stat(directories[index], &directory_info) != 0 ||
            !S_ISDIR(directory_info.st_mode) || directory_info.st_uid != 0 ||
            (directory_info.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
            jvman_path_join(candidate, sizeof(candidate), directories[index],
                            name) != 0 ||
            platform_absolute_path(candidate, canonical,
                                   sizeof(canonical)) != 0 ||
            stat(canonical, &file_info) != 0 || !S_ISREG(file_info.st_mode) ||
            file_info.st_uid != 0 ||
            (file_info.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) != 0 ||
            access(canonical, X_OK) != 0) {
            continue;
        }
        if (strlen(canonical) + 1u > out_size) return -1;
        strcpy(out, canonical);
        return 0;
    }
    return -1;
#endif
}

int platform_set_environment(const char *name, const char *value) {
#if defined(_WIN32)
    if (_putenv_s(name, value ? value : "") == 0) return 0;
#else
    if (value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0) return 0;
#endif
    platform_set_error("cannot set environment variable %s", name);
    return -1;
}

int platform_prepend_path(const char *directory) {
    const char *old_path = getenv("PATH");
    size_t required = strlen(directory) + 1 +
                      (old_path && *old_path ? strlen(old_path) + 1 : 0);
    char *updated = (char *)malloc(required);
    int result;
    if (!updated) return -1;
#if defined(_WIN32)
    if (old_path && *old_path) snprintf(updated, required, "%s;%s", directory, old_path);
    else snprintf(updated, required, "%s", directory);
#else
    if (old_path && *old_path) snprintf(updated, required, "%s:%s", directory, old_path);
    else snprintf(updated, required, "%s", directory);
#endif
    result = platform_set_environment("PATH", updated);
    free(updated);
    return result;
}

const char *platform_os_name(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "mac";
#elif defined(__linux__)
    if (access("/etc/alpine-release", F_OK) == 0) return "alpine-linux";
    return "linux";
#else
    return "unsupported";
#endif
}

const char *platform_arch_name(void) {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x32";
#elif defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return "x64";
#elif defined(__arm__)
    return "arm";
#elif defined(__powerpc64__) && defined(__BYTE_ORDER__) && \
      __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return "ppc64le";
#elif defined(__powerpc64__)
    return "ppc64";
#elif defined(__s390x__)
    return "s390x";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "unsupported";
#endif
}

const char *platform_archive_extension(void) {
#if defined(_WIN32)
    return ".zip";
#else
    return ".tar.gz";
#endif
}

unsigned long platform_process_id(void) {
#if defined(_WIN32)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

int platform_monotonic_millis(uint64_t *value_out) {
    if (!value_out) return -1;
#if defined(_WIN32)
    *value_out = (uint64_t)GetTickCount64();
#else
    {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        *value_out = (uint64_t)now.tv_sec * 1000u +
                     (uint64_t)now.tv_nsec / 1000000u;
    }
#endif
    return 0;
}

int platform_lock_acquire(const char *path, PlatformLock *lock) {
    if (!lock) return -1;
#if defined(_WIN32)
    int attempt;
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (attempt = 0; attempt < 50; ++attempt) {
        handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION) break;
        Sleep(100);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        platform_set_windows_error("cannot acquire state lock");
        return -1;
    }
    lock->handle = handle;
#else
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    int descriptor_flags;
    if (fd < 0) {
        platform_set_error("cannot acquire state lock '%s': %s", path, strerror(errno));
        return -1;
    }
    descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 || fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
        flock(fd, LOCK_EX) != 0) {
        if (fd >= 0) close(fd);
        platform_set_error("cannot acquire state lock '%s': %s", path, strerror(errno));
        return -1;
    }
    lock->fd = fd;
#endif
    return 0;
}

void platform_lock_release(PlatformLock *lock) {
    if (!lock) return;
#if defined(_WIN32)
    if (lock->handle && lock->handle != INVALID_HANDLE_VALUE) CloseHandle((HANDLE)lock->handle);
    lock->handle = NULL;
#else
    if (lock->fd >= 0) { flock(lock->fd, LOCK_UN); close(lock->fd); }
    lock->fd = -1;
#endif
}

#if !defined(_WIN32)
static int posix_secure_temporary_directory(char *out, size_t out_size) {
    const char *requested = getenv("TMPDIR");
    const char *candidates[2];
    size_t index;
    candidates[0] = requested && requested[0] == '/' ? requested : NULL;
    candidates[1] = "/tmp";
    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]);
         ++index) {
        char canonical[JVMAN_PATH_MAX];
        struct stat info;
        size_t length;
        if (!candidates[index] || !realpath(candidates[index], canonical) ||
            stat(canonical, &info) != 0 || !S_ISDIR(info.st_mode) ||
            (info.st_uid != 0 && info.st_uid != geteuid()) ||
            ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0 &&
             (info.st_mode & S_ISVTX) == 0)) {
            continue;
        }
        length = strlen(canonical);
        if (length + 1u > out_size) return -1;
        memcpy(out, canonical, length + 1u);
        return 0;
    }
    return -1;
}
#endif

int platform_create_temporary_file(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        platform_set_error("temporary file output is required");
        return -1;
    }
    out[0] = '\0';
#if defined(_WIN32)
    WCHAR directory[JVMAN_PATH_MAX];
    WCHAR path[JVMAN_PATH_MAX];
    DWORD length;
    unsigned int attempt;
    length = GetTempPathW((DWORD)(sizeof(directory) / sizeof(directory[0])),
                          directory);
    if (length == 0 || length >= sizeof(directory) / sizeof(directory[0])) {
        platform_set_windows_error("cannot locate the temporary directory");
        return -1;
    }
    for (attempt = 0; attempt < 128u; ++attempt) {
        HANDLE file;
        int written = _snwprintf(
            path, sizeof(path) / sizeof(path[0]),
            L"%lsjvman-%08lx-%08lx-%03u.tmp", directory,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetTickCount(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(path) / sizeof(path[0])) {
            platform_set_error("temporary file path is too long");
            return -1;
        }
        file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            if (platform_copy_wide_to_utf8(path, out, out_size) == 0) return 0;
            DeleteFileW(path);
            return -1;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            platform_set_windows_error("cannot create a temporary file");
            return -1;
        }
    }
    platform_set_error("cannot allocate a unique temporary file");
    return -1;
#else
    char directory[JVMAN_PATH_MAX];
    char path[JVMAN_PATH_MAX];
    struct stat info;
    int fd;
    int written;
    if (posix_secure_temporary_directory(directory, sizeof(directory)) != 0) {
        platform_set_error("cannot locate a secure temporary directory");
        return -1;
    }
    written = snprintf(path, sizeof(path), "%s/jvman-XXXXXX", directory);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        platform_set_error("temporary file path is too long");
        return -1;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        platform_set_error("cannot create a temporary file: %s",
                           strerror(errno));
        return -1;
    }
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 ||
        fchmod(fd, 0600) != 0) {
        int saved_error = errno ? errno : EINVAL;
        close(fd);
        unlink(path);
        platform_set_error("cannot secure a temporary file: %s",
                           strerror(saved_error));
        return -1;
    }
    if (close(fd) != 0) {
        int saved_error = errno;
        unlink(path);
        platform_set_error("cannot close a temporary file: %s",
                           strerror(saved_error));
        return -1;
    }
    if (strlen(path) + 1u > out_size) {
        unlink(path);
        platform_set_error("temporary file path is too long");
        return -1;
    }
    strcpy(out, path);
    return 0;
#endif
}

int platform_read_file_limited(const char *path, size_t limit,
                               char **data_out, size_t *size_out) {
    char *data;
    size_t size;
    if (!path || !data_out || !size_out || limit == 0) {
        platform_set_error("invalid limited file read");
        return -1;
    }
    *data_out = NULL;
    *size_out = 0;
#if defined(_WIN32)
    WCHAR *wide = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER file_size;
    size_t offset = 0;
    if (platform_utf8_to_wide(path, &wide) != 0) return -1;
    file = CreateFileW(wide, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                           FILE_FLAG_OPEN_REPARSE_POINT,
                       NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        platform_set_windows_error("cannot open downloaded file");
        return -1;
    }
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0 ||
        (uint64_t)file_size.QuadPart > (uint64_t)limit ||
        (uint64_t)file_size.QuadPart > (uint64_t)(SIZE_MAX - 1u)) {
        CloseHandle(file);
        platform_set_error("downloaded file exceeds the allowed size");
        return -1;
    }
    size = (size_t)file_size.QuadPart;
    data = (char *)malloc(size + 1u);
    if (!data) {
        CloseHandle(file);
        platform_set_error("out of memory reading downloaded file");
        return -1;
    }
    while (offset < size) {
        DWORD request = size - offset > 64u * 1024u
                            ? 64u * 1024u
                            : (DWORD)(size - offset);
        DWORD count = 0;
        if (!ReadFile(file, data + offset, request, &count, NULL) ||
            count == 0) {
            free(data);
            CloseHandle(file);
            platform_set_windows_error("cannot read downloaded file");
            return -1;
        }
        offset += count;
    }
    CloseHandle(file);
#else
    int fd;
    struct stat info;
    size_t offset = 0;
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        info.st_size < 0 || (uintmax_t)info.st_size > (uintmax_t)limit ||
        (uintmax_t)info.st_size > (uintmax_t)(SIZE_MAX - 1u)) {
        if (fd >= 0) close(fd);
        platform_set_error("cannot safely read downloaded file: %s",
                           strerror(errno ? errno : EINVAL));
        return -1;
    }
    size = (size_t)info.st_size;
    data = (char *)malloc(size + 1u);
    if (!data) {
        close(fd);
        platform_set_error("out of memory reading downloaded file");
        return -1;
    }
    while (offset < size) {
        ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(data);
            close(fd);
            platform_set_error("cannot read downloaded file: %s",
                               strerror(errno));
            return -1;
        }
        offset += (size_t)count;
    }
    close(fd);
#endif
    data[size] = '\0';
    *data_out = data;
    *size_out = size;
    return 0;
}

int platform_current_executable(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        platform_set_error("executable path output is required");
        return -1;
    }
#if defined(_WIN32)
    WCHAR path[JVMAN_PATH_MAX];
    DWORD length = GetModuleFileNameW(
        NULL, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= sizeof(path) / sizeof(path[0])) {
        platform_set_windows_error("cannot locate the running executable");
        return -1;
    }
    return platform_copy_wide_to_utf8(path, out, out_size);
#elif defined(__APPLE__)
    uint32_t size = 0;
    char unresolved[JVMAN_PATH_MAX];
    char resolved[JVMAN_PATH_MAX];
    (void)_NSGetExecutablePath(NULL, &size);
    if (size == 0 || size > sizeof(unresolved) ||
        _NSGetExecutablePath(unresolved, &size) != 0 ||
        !realpath(unresolved, resolved)) {
        platform_set_error("cannot locate the running executable: %s",
                           strerror(errno));
        return -1;
    }
    if (strlen(resolved) + 1u > out_size) {
        platform_set_error("running executable path is too long");
        return -1;
    }
    strcpy(out, resolved);
    return 0;
#elif defined(__linux__)
    char path[JVMAN_PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1u);
    if (length <= 0 || (size_t)length >= sizeof(path) - 1u) {
        platform_set_error("cannot locate the running executable: %s",
                           strerror(errno));
        return -1;
    }
    path[length] = '\0';
    if ((size_t)length + 1u > out_size) {
        platform_set_error("running executable path is too long");
        return -1;
    }
    strcpy(out, path);
    return 0;
#else
    (void)out;
    (void)out_size;
    platform_set_error("self-update is unsupported on this operating system");
    return -1;
#endif
}

#if !defined(_WIN32)
static int posix_sha256_fd(int fd, unsigned char digest[32]) {
    unsigned char buffer[64u * 1024u];
    JvmanSha256 context;
    if (fd < 0 || !digest || lseek(fd, 0, SEEK_SET) < 0) return -1;
    jvman_sha256_init(&context);
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (count == 0) break;
        jvman_sha256_update(&context, buffer, (size_t)count);
    }
    jvman_sha256_final(&context, digest);
    return 0;
}

static int posix_lock_update_target(int fd) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return -1;
    deadline.tv_sec += 120;
    for (;;) {
        struct timespec delay;
        struct timespec now;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return 0;
        if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
            return -1;
        }
        for (;;) {
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
                errno = ETIMEDOUT;
                return -1;
            }
            delay.tv_sec = deadline.tv_sec - now.tv_sec;
            delay.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (delay.tv_nsec < 0) {
                --delay.tv_sec;
                delay.tv_nsec += 1000000000L;
            }
            if (delay.tv_sec > 0 || delay.tv_nsec > 100000000L) {
                delay.tv_sec = 0;
                delay.tv_nsec = 100000000L;
            }
            if (nanosleep(&delay, NULL) == 0) break;
            if (errno != EINTR) return -1;
        }
    }
}

static int posix_open_update_directory(const char *target, char *directory,
                                       size_t directory_size,
                                       const char **name_out) {
    const char *separator;
    struct stat info;
    size_t length;
    int flags = O_RDONLY;
    int fd;
    if (!target || !directory || directory_size == 0 || !name_out) {
        errno = EINVAL;
        return -1;
    }
    separator = strrchr(target, '/');
    if (!separator || separator[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    length = separator == target ? 1u : (size_t)(separator - target);
    if (length + 1u > directory_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(directory, target, length);
    directory[length] = '\0';
    *name_out = separator + 1;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(directory, flags);
    if (fd < 0) return -1;
    if (fstat(fd, &info) != 0) {
        int saved_error = errno;
        close(fd);
        errno = saved_error;
        return -1;
    }
    if (!S_ISDIR(info.st_mode) ||
        (info.st_uid != 0 && info.st_uid != geteuid()) ||
        ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0 &&
         (info.st_mode & S_ISVTX) == 0)) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}
#endif

int platform_sha256_file(const char *path, unsigned char digest[32]) {
    if (!path || !digest) {
        platform_set_error("invalid SHA-256 file input");
        return -1;
    }
#if defined(_WIN32)
    WCHAR *wide = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    unsigned char buffer[64u * 1024u];
    JvmanSha256 context;
    if (platform_utf8_to_wide(path, &wide) != 0) return -1;
    file = CreateFileW(wide, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                           FILE_FLAG_OPEN_REPARSE_POINT,
                       NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        platform_set_windows_error("cannot open file for SHA-256");
        return -1;
    }
    jvman_sha256_init(&context);
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer, (DWORD)sizeof(buffer), &count, NULL)) {
            CloseHandle(file);
            platform_set_windows_error("cannot read file for SHA-256");
            return -1;
        }
        if (count == 0) break;
        jvman_sha256_update(&context, buffer, count);
    }
    CloseHandle(file);
    jvman_sha256_final(&context, digest);
    return 0;
#else
    int flags = O_RDONLY;
    int fd;
    struct stat info;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        posix_sha256_fd(fd, digest) != 0) {
        int saved_error = errno ? errno : EINVAL;
        if (fd >= 0) close(fd);
        platform_set_error("cannot calculate SHA-256 for '%s': %s", path,
                           strerror(saved_error));
        return -1;
    }
    close(fd);
    return 0;
#endif
}

#if defined(_WIN32)
static int windows_https_download(const char *url, const char *destination,
                                  size_t limit, unsigned int timeout_seconds) {
    WCHAR *wide_url = NULL;
    WCHAR *wide_destination = NULL;
    URL_COMPONENTSW components;
    WCHAR host[256];
    WCHAR resource[2048];
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    DWORD redirect_policy =
        WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD timeout_millis = timeout_seconds * 1000u;
    DWORD phase_timeout = timeout_millis < 30000u
                              ? timeout_millis : 30000u;
    BY_HANDLE_FILE_INFORMATION output_info;
    size_t resource_length;
    uint64_t total = 0;
    int truncate_existing = 0;
    int result = -1;
    if (!url || strncmp(url, "https://", 8) != 0 ||
        strlen(url) >= JVMAN_PATH_MAX ||
        platform_utf8_to_wide(url, &wide_url) != 0 ||
        platform_utf8_to_wide(destination, &wide_destination) != 0) {
        free(wide_url);
        free(wide_destination);
        platform_set_error("invalid HTTPS download path");
        return -1;
    }
    memset(&components, 0, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide_url, 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0 ||
        components.dwHostNameLength >= sizeof(host) / sizeof(host[0]) ||
        components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0) {
        platform_set_windows_error("cannot parse HTTPS download URL");
        goto done;
    }
    memcpy(host, components.lpszHostName,
           components.dwHostNameLength * sizeof(host[0]));
    host[components.dwHostNameLength] = L'\0';
    resource_length = (size_t)components.dwUrlPathLength +
                      (size_t)components.dwExtraInfoLength;
    if (resource_length == 0 ||
        resource_length >= sizeof(resource) / sizeof(resource[0])) {
        platform_set_error("HTTPS resource path is too long");
        goto done;
    }
    memcpy(resource, components.lpszUrlPath,
           components.dwUrlPathLength * sizeof(resource[0]));
    if (components.dwExtraInfoLength != 0) {
        memcpy(resource + components.dwUrlPathLength, components.lpszExtraInfo,
               components.dwExtraInfoLength * sizeof(resource[0]));
    }
    resource[resource_length] = L'\0';
    output = CreateFileW(wide_destination, GENERIC_WRITE, FILE_SHARE_READ,
                          NULL, CREATE_NEW,
                          FILE_ATTRIBUTE_TEMPORARY |
                              FILE_FLAG_SEQUENTIAL_SCAN |
                              FILE_FLAG_OPEN_REPARSE_POINT,
                          NULL);
    if (output == INVALID_HANDLE_VALUE &&
        (GetLastError() == ERROR_FILE_EXISTS ||
         GetLastError() == ERROR_ALREADY_EXISTS)) {
        truncate_existing = 1;
        output = CreateFileW(wide_destination, GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_TEMPORARY |
                                 FILE_FLAG_SEQUENTIAL_SCAN |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
    }
    if (output == INVALID_HANDLE_VALUE) {
        platform_set_windows_error("cannot open download destination");
        goto done;
    }
    memset(&output_info, 0, sizeof(output_info));
    if (!GetFileInformationByHandle(output, &output_info)) {
        platform_set_windows_error("cannot inspect download destination");
        goto done;
    }
    if ((output_info.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        output_info.nNumberOfLinks != 1u) {
        platform_set_error("download destination failed safety checks");
        goto done;
    }
    if (truncate_existing && !SetEndOfFile(output)) {
        platform_set_windows_error("download destination failed safety checks");
        goto done;
    }
    session = WinHttpOpen(L"jvman/" JVMAN_VERSION_W,
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        platform_set_windows_error("cannot initialize Windows HTTPS");
        goto done;
    }
    if (!WinHttpSetTimeouts(session, phase_timeout, phase_timeout,
                            phase_timeout, timeout_millis)) {
        platform_set_windows_error("cannot set HTTPS timeouts");
        goto done;
    }
    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (!connection) {
        platform_set_windows_error("cannot connect to HTTPS host");
        goto done;
    }
    request = WinHttpOpenRequest(connection, L"GET", resource, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request ||
        !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirect_policy, sizeof(redirect_policy)) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        platform_set_windows_error("HTTPS request failed");
        goto done;
    }
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200u) {
        platform_set_error("HTTPS server returned status %lu",
                           (unsigned long)status);
        goto done;
    }
    for (;;) {
        unsigned char buffer[64u * 1024u];
        DWORD count = 0;
        DWORD written = 0;
        if (!WinHttpReadData(request, buffer, (DWORD)sizeof(buffer), &count)) {
            platform_set_windows_error("cannot read HTTPS response");
            goto done;
        }
        if (count == 0) break;
        if ((uint64_t)count > (uint64_t)limit - total) {
            platform_set_error("HTTPS response exceeds the allowed size");
            goto done;
        }
        if (!WriteFile(output, buffer, count, &written, NULL) ||
            written != count) {
            platform_set_windows_error("cannot write HTTPS response");
            goto done;
        }
        total += count;
    }
    if (total == 0) {
        platform_set_error("HTTPS response is empty");
        goto done;
    }
    if (!FlushFileBuffers(output)) {
        platform_set_windows_error("cannot flush downloaded file");
        goto done;
    }
    result = 0;
done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    free(wide_destination);
    free(wide_url);
    return result;
}
#endif

int platform_https_download_timeout(const char *url, const char *destination,
                                    size_t limit, int show_progress,
                                    unsigned int timeout_seconds) {
    if (!url || !destination || limit == 0 ||
        timeout_seconds == 0 || timeout_seconds > 300u ||
        strncmp(url, "https://", 8) != 0) {
        platform_set_error("invalid HTTPS download request");
        return -1;
    }
#if defined(_WIN32)
    (void)show_progress;
    return windows_https_download(url, destination, limit, timeout_seconds);
#else
    char downloader[JVMAN_PATH_MAX];
    char limit_text[32];
    char timeout_text[16];
    char connect_timeout_text[16];
    char *arguments[32];
    unsigned char buffer[64u * 1024u];
    struct stat path_info;
    struct stat output_info;
    int pipe_fds[2] = {-1, -1};
    int output = -1;
    int output_flags = O_WRONLY;
    int path_existed = 0;
    int index = 0;
    int written;
    int timeout_written;
    int connect_timeout_written;
    int status = 0;
    int child_waited = 0;
    int failed = 0;
    size_t total = 0;
    pid_t child = -1;
    written = snprintf(limit_text, sizeof(limit_text), "%zu", limit);
    timeout_written = snprintf(timeout_text, sizeof(timeout_text), "%u",
                               timeout_seconds);
    connect_timeout_written = snprintf(
        connect_timeout_text, sizeof(connect_timeout_text), "%u",
        timeout_seconds < 30u ? timeout_seconds : 30u);
    if (written < 0 || (size_t)written >= sizeof(limit_text) ||
        timeout_written < 0 ||
        (size_t)timeout_written >= sizeof(timeout_text) ||
        connect_timeout_written < 0 ||
        (size_t)connect_timeout_written >= sizeof(connect_timeout_text) ||
        platform_find_trusted_executable("curl", downloader,
                                         sizeof(downloader)) != 0) {
        platform_set_error("trusted curl executable is not available");
        return -1;
    }
    arguments[index++] = downloader;
    arguments[index++] = "--disable";
    arguments[index++] = "--fail";
    arguments[index++] = "--location";
    arguments[index++] = "--show-error";
    arguments[index++] = "--retry";
    arguments[index++] = "2";
    if (show_progress) arguments[index++] = "--progress-bar";
    else arguments[index++] = "--silent";
    arguments[index++] = "--header";
    arguments[index++] = "User-Agent: jvman/" JVMAN_VERSION;
    arguments[index++] = "--connect-timeout";
    arguments[index++] = connect_timeout_text;
    arguments[index++] = "--max-time";
    arguments[index++] = timeout_text;
    arguments[index++] = "--proto";
    arguments[index++] = "=https";
    arguments[index++] = "--proto-redir";
    arguments[index++] = "=https";
    arguments[index++] = "--max-filesize";
    arguments[index++] = limit_text;
    arguments[index++] = (char *)url;
    arguments[index] = NULL;

    if (lstat(destination, &path_info) == 0) {
        if (!S_ISREG(path_info.st_mode) || path_info.st_nlink != 1 ||
            path_info.st_uid != geteuid()) {
            platform_set_error("download destination failed safety checks");
            return -1;
        }
        path_existed = 1;
    } else if (errno != ENOENT) {
        platform_set_error("cannot inspect download destination: %s",
                           strerror(errno));
        return -1;
    } else {
        output_flags |= O_CREAT | O_EXCL;
    }
#ifdef O_NOFOLLOW
    output_flags |= O_NOFOLLOW;
#endif
    output = open(destination, output_flags, 0600);
    if (output < 0 || fstat(output, &output_info) != 0 ||
        !S_ISREG(output_info.st_mode) || output_info.st_nlink != 1 ||
        output_info.st_uid != geteuid() ||
        (path_existed &&
         (path_info.st_dev != output_info.st_dev ||
          path_info.st_ino != output_info.st_ino)) ||
        fchmod(output, 0600) != 0 ||
        (path_existed && ftruncate(output, 0) != 0)) {
        if (output >= 0) close(output);
        platform_set_error("cannot safely open download destination: %s",
                           strerror(errno));
        return -1;
    }
    if (pipe(pipe_fds) != 0) {
        platform_set_error("cannot create HTTPS response pipe: %s",
                           strerror(errno));
        close(output);
        return -1;
    }
    child = fork();
    if (child < 0) {
        platform_set_error("cannot start HTTPS downloader: %s",
                           strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(output);
        return -1;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipe_fds[1]);
        close(output);
        execv(downloader, arguments);
        _exit(127);
    }
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    for (;;) {
        ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
        size_t offset = 0;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            platform_set_error("cannot read HTTPS response stream: %s",
                               strerror(errno));
            failed = 1;
            break;
        }
        if (count == 0) break;
        if ((size_t)count > limit - total) {
            failed = 1;
            platform_set_error("HTTPS response exceeds the allowed size");
            break;
        }
        while (offset < (size_t)count) {
            ssize_t count_written = write(
                output, buffer + offset, (size_t)count - offset);
            if (count_written < 0 && errno == EINTR) continue;
            if (count_written <= 0) {
                platform_set_error("cannot write HTTPS response: %s",
                                   strerror(errno));
                failed = 1;
                break;
            }
            offset += (size_t)count_written;
        }
        if (failed) break;
        total += (size_t)count;
    }
    close(pipe_fds[0]);
    pipe_fds[0] = -1;
    if (failed) (void)kill(child, SIGKILL);
    for (;;) {
        pid_t waited = waitpid(child, &status, 0);
        if (waited == child) {
            child_waited = 1;
            break;
        }
        if (waited < 0 && errno == EINTR) continue;
        platform_set_error("cannot wait for HTTPS downloader: %s",
                           strerror(errno));
        failed = 1;
        break;
    }
    if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        platform_set_error("HTTPS downloader exited unsuccessfully");
        failed = 1;
    }
    if (!failed && total == 0) {
        platform_set_error("HTTPS response is empty");
        failed = 1;
    }
    if (!failed && fsync(output) != 0) {
        platform_set_error("cannot flush downloaded file: %s", strerror(errno));
        failed = 1;
    }
    if (pipe_fds[0] >= 0) close(pipe_fds[0]);
    if (pipe_fds[1] >= 0) close(pipe_fds[1]);
    if (child > 0 && !child_waited) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (close(output) != 0 && !failed) {
        platform_set_error("cannot close downloaded file: %s", strerror(errno));
        failed = 1;
    }
    return failed ? -1 : 0;
#endif
}

int platform_https_download(const char *url, const char *destination,
                            size_t limit, int show_progress) {
    return platform_https_download_timeout(url, destination, limit,
                                           show_progress, 300u);
}

static int platform_read_exact_at(const char *path, uint64_t offset,
                                  unsigned char *buffer, size_t size) {
    if (!path || !buffer || size == 0) return -1;
#if defined(_WIN32)
    WCHAR *wide = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER position;
    DWORD attributes;
    size_t used = 0;
    if (offset > (uint64_t)INT64_MAX ||
        platform_utf8_to_wide(path, &wide) != 0) return -1;
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        free(wide);
        return -1;
    }
    file = CreateFileW(wide, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                       NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return -1;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN)) {
        CloseHandle(file);
        return -1;
    }
    while (used < size) {
        DWORD request = size - used > UINT32_MAX
                            ? UINT32_MAX
                            : (DWORD)(size - used);
        DWORD count = 0;
        if (!ReadFile(file, buffer + used, request, &count, NULL) ||
            count == 0) {
            CloseHandle(file);
            return -1;
        }
        used += count;
    }
    CloseHandle(file);
    return 0;
#else
    int flags = O_RDONLY;
    int fd;
    struct stat info;
    size_t used = 0;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    while (used < size) {
        ssize_t count = pread(fd, buffer + used, size - used,
                              (off_t)(offset + used));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)count;
    }
    close(fd);
    return 0;
#endif
}

#if defined(_WIN32) || defined(__linux__)
static uint16_t platform_u16_le(const unsigned char *bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}
#endif

#if defined(_WIN32) || defined(__APPLE__)
static uint32_t platform_u32_le(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}
#endif

int platform_validate_executable_image(const char *path) {
    unsigned char header[64];
    if (platform_read_exact_at(path, 0, header, sizeof(header)) != 0) {
        platform_set_error("cannot read executable image header");
        return -1;
    }
#if defined(_WIN32)
    {
        unsigned char pe[6];
        uint32_t pe_offset;
        uint16_t expected_machine;
#if defined(_M_ARM64) || defined(__aarch64__)
        expected_machine = 0xaa64u;
#elif defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
        expected_machine = 0x8664u;
#elif defined(_M_IX86) || defined(__i386__)
        expected_machine = 0x014cu;
#else
        platform_set_error("unsupported Windows update architecture");
        return -1;
#endif
        if (header[0] != 'M' || header[1] != 'Z') {
            platform_set_error("release asset does not contain a DOS/PE header");
            return -1;
        }
        pe_offset = platform_u32_le(header + 60);
        if (pe_offset < 64u || pe_offset > 16u * 1024u * 1024u ||
            platform_read_exact_at(path, pe_offset, pe, sizeof(pe)) != 0 ||
            memcmp(pe, "PE\0\0", 4) != 0 ||
            platform_u16_le(pe + 4) != expected_machine) {
            platform_set_error("release PE image has the wrong format or architecture");
            return -1;
        }
    }
#elif defined(__linux__)
    {
        uint16_t machine;
#if defined(__aarch64__)
        const uint16_t expected_machine = 183u;
#elif defined(__x86_64__) || defined(__amd64__)
        const uint16_t expected_machine = 62u;
#else
        platform_set_error("unsupported Linux update architecture");
        return -1;
#endif
        if (header[0] != 0x7fu || header[1] != 'E' ||
            header[2] != 'L' || header[3] != 'F' ||
            header[4] != 2u || header[5] != 1u) {
            platform_set_error("release asset is not a 64-bit little-endian ELF image");
            return -1;
        }
        machine = platform_u16_le(header + 18);
        if (machine != expected_machine) {
            platform_set_error("release ELF image has the wrong architecture");
            return -1;
        }
    }
#elif defined(__APPLE__)
    {
        uint32_t cpu_type;
#if defined(__aarch64__)
        const uint32_t expected_cpu = 0x0100000cu;
#elif defined(__x86_64__) || defined(__amd64__)
        const uint32_t expected_cpu = 0x01000007u;
#else
        platform_set_error("unsupported macOS update architecture");
        return -1;
#endif
        if (header[0] != 0xcfu || header[1] != 0xfau ||
            header[2] != 0xedu || header[3] != 0xfeu) {
            platform_set_error("release asset is not a 64-bit Mach-O image");
            return -1;
        }
        cpu_type = platform_u32_le(header + 4);
        if (cpu_type != expected_cpu) {
            platform_set_error("release Mach-O image has the wrong architecture");
            return -1;
        }
    }
#else
    platform_set_error("self-update is unsupported on this operating system");
    return -1;
#endif
    return 0;
}

static int platform_sha256_text_valid(const char *text) {
    size_t index;
    if (!text || strlen(text) != 64u) return 0;
    for (index = 0; index < 64u; ++index) {
        if (!isxdigit((unsigned char)text[index])) return 0;
    }
    return 1;
}

#if defined(_WIN32)
static int windows_file_is_regular(const WCHAR *path) {
    DWORD attributes;
    if (!path || !*path) return 0;
    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                          FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

static int windows_get_module_path(WCHAR *out, size_t capacity) {
    DWORD length;
    if (!out || capacity == 0 || capacity > UINT32_MAX) return -1;
    length = GetModuleFileNameW(NULL, out, (DWORD)capacity);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static int windows_process_is_elevated(int *elevated_out) {
    TOKEN_ELEVATION elevation;
    HANDLE token = NULL;
    BOOL query_succeeded;
    DWORD returned = 0;
    if (!elevated_out) return -1;
    *elevated_out = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return -1;
    }
    memset(&elevation, 0, sizeof(elevation));
    query_succeeded = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    if (!query_succeeded || returned != sizeof(elevation)) {
        DWORD error = query_succeeded ? ERROR_INVALID_DATA : GetLastError();
        CloseHandle(token);
        SetLastError(error);
        return -1;
    }
    CloseHandle(token);
    *elevated_out = elevation.TokenIsElevated != 0;
    return 0;
}

static int windows_sha256_handle(HANDLE file,
                                 unsigned char digest[32]);

static int windows_sha256_file_w(const WCHAR *path,
                                 unsigned char digest[32]) {
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE file;
    int result;
    if (!path || !digest) return -1;
    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                           FILE_FLAG_OPEN_REPARSE_POINT,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    memset(&info, 0, sizeof(info));
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks != 1u) {
        CloseHandle(file);
        return -1;
    }
    result = windows_sha256_handle(file, digest);
    CloseHandle(file);
    return result;
}

static int windows_sha256_handle(HANDLE file,
                                 unsigned char digest[32]) {
    unsigned char buffer[64u * 1024u];
    JvmanSha256 context;
    LARGE_INTEGER start;
    if (!file || file == INVALID_HANDLE_VALUE || !digest) return -1;
    start.QuadPart = 0;
    if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN)) return -1;
    jvman_sha256_init(&context);
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer, (DWORD)sizeof(buffer), &count, NULL)) {
            return -1;
        }
        if (count == 0) break;
        jvman_sha256_update(&context, buffer, count);
    }
    jvman_sha256_final(&context, digest);
    return 0;
}

static void windows_digest_to_wide(const unsigned char digest[32],
                                   WCHAR out[65]) {
    static const WCHAR hex[] = L"0123456789abcdef";
    size_t index;
    for (index = 0; index < 32u; ++index) {
        out[index * 2u] = hex[digest[index] >> 4];
        out[index * 2u + 1u] = hex[digest[index] & 0x0fu];
    }
    out[64] = L'\0';
}

static int windows_checksum_w_valid(const WCHAR *text) {
    size_t index;
    if (!text || wcslen(text) != 64u) return 0;
    for (index = 0; index < 64u; ++index) {
        WCHAR ch = text[index];
        if (!((ch >= L'0' && ch <= L'9') ||
              (ch >= L'a' && ch <= L'f') ||
              (ch >= L'A' && ch <= L'F'))) return 0;
    }
    return 1;
}

static int windows_digest_matches_w(const unsigned char digest[32],
                                    const WCHAR *expected) {
    WCHAR actual[65];
    windows_digest_to_wide(digest, actual);
    return windows_checksum_w_valid(expected) &&
           _wcsicmp(actual, expected) == 0;
}

static int windows_update_helper_path_valid(const WCHAR *module);

static HANDLE windows_acquire_update_mutex(const WCHAR *target) {
    WCHAR name[96];
    WCHAR canonical[JVMAN_PATH_MAX];
    WCHAR digest_text[65];
    unsigned char digest[32];
    JvmanSha256 context;
    HANDLE target_file;
    HANDLE mutex;
    DWORD canonical_length;
    DWORD wait_result;
    if (!target || !*target) return NULL;
    target_file = CreateFileW(
        target, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (target_file == INVALID_HANDLE_VALUE) return NULL;
    canonical_length = GetFinalPathNameByHandleW(
        target_file, canonical,
        (DWORD)(sizeof(canonical) / sizeof(canonical[0])),
        FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    CloseHandle(target_file);
    if (canonical_length == 0 ||
        canonical_length >= sizeof(canonical) / sizeof(canonical[0])) {
        return NULL;
    }
    jvman_sha256_init(&context);
    jvman_sha256_update(&context, canonical,
                        (size_t)canonical_length * sizeof(canonical[0]));
    jvman_sha256_final(&context, digest);
    windows_digest_to_wide(digest, digest_text);
    if (_snwprintf(name, sizeof(name) / sizeof(name[0]),
                   L"Global\\jvman-update-%ls", digest_text) < 0) {
        return NULL;
    }
    mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex) return NULL;
    wait_result = WaitForSingleObject(mutex, 120000u);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return NULL;
    }
    return mutex;
}

static void windows_release_update_mutex(HANDLE mutex) {
    if (!mutex) return;
    (void)ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static int windows_delete_file_by_stream_rename(const WCHAR *path) {
    WCHAR stream_name[64];
    FILE_RENAME_INFO *rename_info = NULL;
    FILE_DISPOSITION_INFO disposition;
    HANDLE file = INVALID_HANDLE_VALUE;
    size_t stream_bytes;
    size_t rename_size;
    int written;
    int result = -1;
    if (!path || !*path) return -1;
    written = _snwprintf(stream_name,
                         sizeof(stream_name) / sizeof(stream_name[0]),
                         L":jvman-delete-%08lx",
                         (unsigned long)GetCurrentProcessId());
    if (written <= 0 ||
        (size_t)written >= sizeof(stream_name) / sizeof(stream_name[0])) {
        return -1;
    }
    stream_bytes = (size_t)written * sizeof(*stream_name);
    if (stream_bytes > SIZE_MAX - offsetof(FILE_RENAME_INFO, FileName)) {
        return -1;
    }
    rename_size = offsetof(FILE_RENAME_INFO, FileName) + stream_bytes;
    rename_info = (FILE_RENAME_INFO *)calloc(1u, rename_size);
    if (!rename_info) return -1;
    rename_info->ReplaceIfExists = FALSE;
    rename_info->RootDirectory = NULL;
    rename_info->FileNameLength = (DWORD)stream_bytes;
    memcpy(rename_info->FileName, stream_name, stream_bytes);
    file = CreateFileW(path, DELETE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE ||
        !SetFileInformationByHandle(file, FileRenameInfo, rename_info,
                                    (DWORD)rename_size)) {
        goto done;
    }
    CloseHandle(file);
    file = CreateFileW(path, DELETE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(file, FileDispositionInfo,
                                   &disposition, sizeof(disposition))) {
        result = 0;
    }
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(rename_info);
    return result;
}

static void windows_delete_running_update_helper(void) {
    WCHAR module[JVMAN_PATH_MAX];
    HANDLE file;
    JvmanFileDispositionInfoEx disposition;
    if (windows_get_module_path(module,
                                sizeof(module) / sizeof(module[0])) != 0 ||
        !windows_update_helper_path_valid(module)) {
        return;
    }
    if (windows_delete_file_by_stream_rename(module) == 0) return;
    file = CreateFileW(module, DELETE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        (void)MoveFileExW(module, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        return;
    }
    disposition.flags = JVMAN_FILE_DISPOSITION_FLAG_DELETE |
                        JVMAN_FILE_DISPOSITION_FLAG_POSIX |
                        JVMAN_FILE_DISPOSITION_FLAG_IGNORE_READONLY;
    if (!SetFileInformationByHandle(file,
                                    JVMAN_FILE_DISPOSITION_INFO_EX_CLASS,
                                    &disposition, sizeof(disposition))) {
        (void)MoveFileExW(module, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    CloseHandle(file);
}

static int windows_update_stage_path_syntax_valid(const WCHAR *stage,
                                                  const WCHAR *target) {
    static const WCHAR marker[] = L".jvman-update-";
    const WCHAR *cursor;
    const WCHAR *suffix;
    size_t target_length;
    size_t stage_length;
    if (!stage || !target) return 0;
    target_length = wcslen(target);
    stage_length = wcslen(stage);
    if (target_length == 0 ||
        stage_length <= target_length + (sizeof(marker) / sizeof(marker[0])) ||
        _wcsnicmp(stage, target, target_length) != 0 ||
        _wcsnicmp(stage + target_length, marker,
                  sizeof(marker) / sizeof(marker[0]) - 1u) != 0 ||
        stage_length < 4u ||
        _wcsicmp(stage + stage_length - 4u, L".tmp") != 0) {
        return 0;
    }
    cursor = stage + target_length + sizeof(marker) / sizeof(marker[0]) - 1u;
    suffix = stage + stage_length - 4u;
    if (cursor >= suffix) return 0;
    while (cursor < suffix) {
        if (!((*cursor >= L'0' && *cursor <= L'9') ||
              (*cursor >= L'a' && *cursor <= L'f') ||
              (*cursor >= L'A' && *cursor <= L'F') ||
              *cursor == L'-')) {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

static int windows_update_stage_path_valid(const WCHAR *stage,
                                           const WCHAR *target) {
    return windows_update_stage_path_syntax_valid(stage, target) &&
           windows_file_is_regular(stage) && windows_file_is_regular(target);
}

static int windows_update_helper_path_valid(const WCHAR *module) {
    static const WCHAR prefix[] = L"jvman-update-helper-";
    WCHAR temporary[JVMAN_PATH_MAX];
    DWORD length;
    size_t module_length;
    const WCHAR *filename;
    length = GetTempPathW((DWORD)(sizeof(temporary) / sizeof(temporary[0])),
                          temporary);
    if (!module || length == 0 ||
        length >= sizeof(temporary) / sizeof(temporary[0]) ||
        _wcsnicmp(module, temporary, length) != 0) return 0;
    filename = module + length;
    module_length = wcslen(filename);
    return _wcsnicmp(filename, prefix,
                     sizeof(prefix) / sizeof(prefix[0]) - 1u) == 0 &&
           module_length > 4u &&
           _wcsicmp(filename + module_length - 4u, L".exe") == 0 &&
           wcschr(filename, L'\\') == NULL && wcschr(filename, L'/') == NULL;
}

static int windows_copy_update_helper(const WCHAR *module, WCHAR *helper,
                                      size_t capacity) {
    WCHAR temporary[JVMAN_PATH_MAX];
    DWORD length;
    unsigned int attempt;
    length = GetTempPathW((DWORD)(sizeof(temporary) / sizeof(temporary[0])),
                          temporary);
    if (!module || !helper || capacity == 0 || length == 0 ||
        length >= sizeof(temporary) / sizeof(temporary[0])) return -1;
    for (attempt = 0; attempt < 128u; ++attempt) {
        int written = _snwprintf(
            helper, capacity,
            L"%lsjvman-update-helper-%08lx-%08lx-%03u.exe", temporary,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetTickCount(), attempt);
        if (written < 0 || (size_t)written >= capacity) return -1;
        if (CopyFileW(module, helper, TRUE)) {
            HANDLE file = CreateFileW(
                helper, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT,
                NULL);
            if (file == INVALID_HANDLE_VALUE || !FlushFileBuffers(file)) {
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                DeleteFileW(helper);
                return -1;
            }
            CloseHandle(file);
            return 0;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    }
    return -1;
}

static int windows_start_update_helper(const WCHAR *stage,
                                       const WCHAR *target,
                                       const char *new_checksum,
                                       const char *old_checksum_text) {
    WCHAR module[JVMAN_PATH_MAX];
    WCHAR helper[JVMAN_PATH_MAX];
    WCHAR old_checksum[65];
    WCHAR new_checksum_w[65];
    BY_HANDLE_FILE_INFORMATION helper_info;
    unsigned char helper_digest[32];
    WCHAR *command = NULL;
    unsigned char old_digest[32];
    HANDLE parent = NULL;
    HANDLE helper_file = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security;
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    SIZE_T attributes_size = 0;
    HANDLE inherited_handles[2];
    size_t index;
    int elevated;
    int attributes_initialized = 0;
    int command_length;
    int result = -1;
    if (windows_process_is_elevated(&elevated) != 0) {
        platform_set_windows_error(
            "cannot inspect the Windows process elevation state");
        return -1;
    }
    if (elevated) {
        platform_set_error(
            "Windows self-update must run from a non-elevated terminal");
        return -1;
    }
    if (!stage || !target || !platform_sha256_text_valid(new_checksum) ||
        !platform_sha256_text_valid(old_checksum_text) ||
        windows_get_module_path(module,
                                sizeof(module) / sizeof(module[0])) != 0 ||
        _wcsicmp(module, target) != 0 ||
        !windows_update_stage_path_valid(stage, target) ||
        windows_sha256_file_w(target, old_digest) != 0 ||
        !jvman_hex_equal(old_digest, sizeof(old_digest), old_checksum_text) ||
        windows_copy_update_helper(module, helper,
                                   sizeof(helper) / sizeof(helper[0])) != 0) {
        platform_set_error("cannot prepare the Windows update helper");
        return -1;
    }
    windows_digest_to_wide(old_digest, old_checksum);
    for (index = 0; index < 64u; ++index) {
        new_checksum_w[index] = (WCHAR)(unsigned char)new_checksum[index];
    }
    new_checksum_w[64] = L'\0';
    parent = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    if (!parent ||
        !SetHandleInformation(parent, HANDLE_FLAG_INHERIT,
                              HANDLE_FLAG_INHERIT)) {
        if (parent) CloseHandle(parent);
        DeleteFileW(helper);
        platform_set_windows_error("cannot create an inherited update handle");
        return -1;
    }
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    helper_file = CreateFileW(
        helper, GENERIC_READ,
        FILE_SHARE_READ,
        &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN |
            FILE_FLAG_OPEN_REPARSE_POINT,
        NULL);
    if (helper_file == INVALID_HANDLE_VALUE) {
        CloseHandle(parent);
        DeleteFileW(helper);
        platform_set_windows_error("cannot open the update helper image");
        return -1;
    }
    memset(&helper_info, 0, sizeof(helper_info));
    if (!GetFileInformationByHandle(helper_file, &helper_info)) {
        CloseHandle(helper_file);
        CloseHandle(parent);
        DeleteFileW(helper);
        platform_set_windows_error("cannot inspect the update helper image");
        return -1;
    }
    if ((helper_info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        helper_info.nNumberOfLinks != 1u ||
        windows_sha256_handle(helper_file, helper_digest) != 0 ||
        memcmp(helper_digest, old_digest, sizeof(helper_digest)) != 0) {
        CloseHandle(helper_file);
        CloseHandle(parent);
        DeleteFileW(helper);
        platform_set_error("update helper image failed final safety checks");
        return -1;
    }
    command = (WCHAR *)calloc(32768u, sizeof(*command));
    command_length = command ? _snwprintf(
        command, 32768u,
        L"\"%ls\" --jvman-internal-apply-update-v1 0x%llx 0x%llx "
        L"\"%ls\" \"%ls\" %ls %ls",
        helper, (unsigned long long)(uintptr_t)parent,
        (unsigned long long)(uintptr_t)helper_file, stage, target,
        new_checksum_w, old_checksum) : -1;
    if (!command || command_length < 0 || command_length >= 32768) {
        free(command);
        CloseHandle(helper_file);
        CloseHandle(parent);
        DeleteFileW(helper);
        platform_set_error("Windows update helper command is too long");
        return -1;
    }
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    inherited_handles[0] = parent;
    inherited_handles[1] = helper_file;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    if (attributes_size == 0) {
        platform_set_windows_error("cannot size the update handle list");
        goto helper_done;
    }
    attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attributes_size);
    if (!attributes) {
        platform_set_error("out of memory preparing the update helper");
        goto helper_done;
    }
    if (!InitializeProcThreadAttributeList(attributes, 1, 0,
                                           &attributes_size)) {
        platform_set_windows_error("cannot initialize the update handle list");
        goto helper_done;
    }
    attributes_initialized = 1;
    if (!UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles, sizeof(inherited_handles), NULL, NULL)) {
        platform_set_windows_error("cannot restrict inherited update handles");
        goto helper_done;
    }
    startup.lpAttributeList = attributes;
    if (CreateProcessW(helper, command, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED |
                           CREATE_UNICODE_ENVIRONMENT |
                           EXTENDED_STARTUPINFO_PRESENT,
                       NULL, NULL, &startup.StartupInfo, &process)) {
        DWORD resume_result;
        CloseHandle(helper_file);
        helper_file = INVALID_HANDLE_VALUE;
        CloseHandle(parent);
        parent = NULL;
        resume_result = ResumeThread(process.hThread);
        if (resume_result == (DWORD)-1) {
            DWORD resume_error = GetLastError();
            DWORD stop_error = ERROR_SUCCESS;
            DWORD stop_wait;
            if (!TerminateProcess(process.hProcess, 3u)) {
                stop_error = GetLastError();
            }
            stop_wait = WaitForSingleObject(process.hProcess, 5000u);
            if (stop_wait == WAIT_OBJECT_0) {
                SetLastError(resume_error);
                platform_set_windows_error(
                    "cannot resume the Windows update helper");
            } else {
                if (stop_wait == WAIT_TIMEOUT) stop_error = ERROR_TIMEOUT;
                else if (stop_error == ERROR_SUCCESS) stop_error = GetLastError();
                SetLastError(stop_error);
                platform_set_windows_error(
                    "cannot stop the failed Windows update helper");
            }
        } else {
            result = 0;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        platform_set_windows_error("cannot start the Windows update helper");
    }
helper_done:
    if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    free(command);
    if (helper_file != INVALID_HANDLE_VALUE) CloseHandle(helper_file);
    if (parent) CloseHandle(parent);
    if (result != 0) DeleteFileW(helper);
    return result;
}
#endif

int platform_stage_executable_update(const char *source, const char *target,
                                     char *staged_out, size_t staged_size) {
    if (!source || !target || !staged_out || staged_size == 0) {
        platform_set_error("invalid executable update staging request");
        return -1;
    }
    staged_out[0] = '\0';
#if defined(_WIN32)
    WCHAR *wide_source = NULL;
    WCHAR *wide_target = NULL;
    WCHAR stage[JVMAN_PATH_MAX];
    WCHAR module[JVMAN_PATH_MAX];
    unsigned int attempt;
    if (platform_utf8_to_wide(source, &wide_source) != 0 ||
        platform_utf8_to_wide(target, &wide_target) != 0 ||
        windows_get_module_path(module,
                                sizeof(module) / sizeof(module[0])) != 0 ||
        _wcsicmp(module, wide_target) != 0 ||
        !windows_file_is_regular(wide_source) ||
        !windows_file_is_regular(wide_target)) {
        free(wide_source);
        free(wide_target);
        platform_set_error("update source or target is not a regular executable");
        return -1;
    }
    for (attempt = 0; attempt < 128u; ++attempt) {
        HANDLE file;
        int written = _snwprintf(
            stage, sizeof(stage) / sizeof(stage[0]),
            L"%ls.jvman-update-%08lx-%08lx-%03u.tmp", wide_target,
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetTickCount(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(stage) / sizeof(stage[0])) break;
        if (!CopyFileW(wide_source, stage, TRUE)) {
            if (GetLastError() == ERROR_FILE_EXISTS ||
                GetLastError() == ERROR_ALREADY_EXISTS) continue;
            break;
        }
        file = CreateFileW(stage, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE || !FlushFileBuffers(file)) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            DeleteFileW(stage);
            break;
        }
        CloseHandle(file);
        if (platform_copy_wide_to_utf8(stage, staged_out, staged_size) == 0) {
            free(wide_source);
            free(wide_target);
            return 0;
        }
        DeleteFileW(stage);
        break;
    }
    free(wide_source);
    free(wide_target);
    platform_set_windows_error("cannot create a same-directory update file");
    return -1;
#else
    char current[JVMAN_PATH_MAX];
    char directory[JVMAN_PATH_MAX];
    char stage[JVMAN_PATH_MAX];
    char stage_name[JVMAN_PATH_MAX];
    const char *target_name = NULL;
    struct stat source_info;
    struct stat target_info;
    int directory_fd = -1;
    int input = -1;
    int output = -1;
    int input_flags = O_RDONLY;
    unsigned int attempt;
    int stage_created = 0;
#ifdef O_NOFOLLOW
    input_flags |= O_NOFOLLOW;
#endif
    if (platform_current_executable(current, sizeof(current)) != 0 ||
        strcmp(current, target) != 0 ||
        lstat(source, &source_info) != 0 || !S_ISREG(source_info.st_mode)) {
        platform_set_error("update source or target failed ownership and file checks");
        return -1;
    }
    directory_fd = posix_open_update_directory(
        target, directory, sizeof(directory), &target_name);
    if (directory_fd < 0 ||
        fstatat(directory_fd, target_name, &target_info,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(target_info.st_mode) || target_info.st_uid != geteuid() ||
        target_info.st_nlink != 1 ||
        (target_info.st_mode &
         (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) != 0) {
        if (directory_fd >= 0) close(directory_fd);
        platform_set_error(
            "update target or its directory failed safety checks");
        return -1;
    }
    input = open(source, input_flags);
    if (input < 0) {
        int saved_error = errno;
        close(directory_fd);
        platform_set_error("cannot open staged update source: %s",
                           strerror(saved_error));
        return -1;
    }
    for (attempt = 0; attempt < 128u; ++attempt) {
        int name_written = snprintf(
            stage_name, sizeof(stage_name),
            "%s.jvman-update-%lu-%03u.tmp", target_name,
            platform_process_id(), attempt);
        int path_written;
        if (name_written < 0 ||
            (size_t)name_written >= sizeof(stage_name)) break;
        path_written = strcmp(directory, "/") == 0
                           ? snprintf(stage, sizeof(stage), "%s%s",
                                      directory, stage_name)
                           : snprintf(stage, sizeof(stage), "%s/%s",
                                      directory, stage_name);
        if (path_written < 0 || (size_t)path_written >= sizeof(stage)) break;
        output = openat(directory_fd, stage_name,
                        O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (output >= 0) {
            stage_created = 1;
            break;
        }
        if (errno != EEXIST) break;
    }
    if (output < 0) {
        int saved_error = errno;
        close(input);
        close(directory_fd);
        platform_set_error("cannot create a same-directory update file: %s",
                           strerror(saved_error));
        return -1;
    }
    for (;;) {
        unsigned char buffer[64u * 1024u];
        ssize_t count = read(input, buffer, sizeof(buffer));
        size_t offset = 0;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) goto posix_stage_failure;
        if (count == 0) break;
        while (offset < (size_t)count) {
            ssize_t written = write(output, buffer + offset,
                                    (size_t)count - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto posix_stage_failure;
            offset += (size_t)written;
        }
    }
    if (fchmod(output, target_info.st_mode & 0777) != 0 ||
        fsync(output) != 0) {
        goto posix_stage_failure;
    }
    if (close(output) != 0) {
        output = -1;
        goto posix_stage_failure;
    }
    output = -1;
    close(input);
    if (strlen(stage) + 1u > staged_size) {
        (void)unlinkat(directory_fd, stage_name, 0);
        close(directory_fd);
        platform_set_error("same-directory update path is too long");
        return -1;
    }
    close(directory_fd);
    directory_fd = -1;
    strcpy(staged_out, stage);
    return 0;
posix_stage_failure:
    {
        int saved_error = errno;
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        if (stage_created && directory_fd >= 0) {
            (void)unlinkat(directory_fd, stage_name, 0);
        } else if (stage_created) {
            (void)unlink(stage);
        }
        if (directory_fd >= 0) close(directory_fd);
        platform_set_error("cannot copy executable update: %s",
                           strerror(saved_error));
        return -1;
    }
#endif
}

#if defined(_WIN32)
static int windows_update_worker(int argc, WCHAR **argv) {
    WCHAR module[JVMAN_PATH_MAX];
    WCHAR backup[JVMAN_PATH_MAX];
    BY_HANDLE_FILE_INFORMATION helper_info;
    BY_HANDLE_FILE_INFORMATION module_info;
    unsigned char helper_digest[32];
    unsigned char target_digest[32];
    unsigned char stage_digest[32];
    WCHAR *handle_end = NULL;
    unsigned long long handle_value;
    HANDLE parent;
    HANDLE helper_file;
    HANDLE module_file = INVALID_HANDLE_VALUE;
    HANDLE update_mutex;
    DWORD wait_result;
    DWORD replace_error = ERROR_SUCCESS;
    unsigned int attempt;
    int elevated;
    int replaced = 0;
    if (!argv || argc != 8 ||
        wcscmp(argv[1], L"--jvman-internal-apply-update-v1") != 0) {
        return 2;
    }
    errno = 0;
    handle_value = wcstoull(argv[2], &handle_end, 0);
    if (errno != 0 || !handle_end || *handle_end != L'\0' ||
        handle_value == 0 ||
        handle_value > (unsigned long long)UINTPTR_MAX) {
        return 2;
    }
    parent = (HANDLE)(uintptr_t)handle_value;
    errno = 0;
    handle_end = NULL;
    handle_value = wcstoull(argv[3], &handle_end, 0);
    if (errno != 0 || !handle_end || *handle_end != L'\0' ||
        handle_value == 0 ||
        handle_value > (unsigned long long)UINTPTR_MAX) {
        CloseHandle(parent);
        return 2;
    }
    helper_file = (HANDLE)(uintptr_t)handle_value;
    if (!windows_checksum_w_valid(argv[6]) ||
        !windows_checksum_w_valid(argv[7]) ||
        windows_get_module_path(module,
                                sizeof(module) / sizeof(module[0])) != 0 ||
        !windows_update_helper_path_valid(module) ||
        !windows_update_stage_path_syntax_valid(argv[4], argv[5])) {
        CloseHandle(helper_file);
        CloseHandle(parent);
        return 2;
    }
    if (windows_process_is_elevated(&elevated) != 0 || elevated) {
        CloseHandle(helper_file);
        CloseHandle(parent);
        DeleteFileW(argv[4]);
        return 3;
    }
    module_file = CreateFileW(
        module, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    memset(&helper_info, 0, sizeof(helper_info));
    memset(&module_info, 0, sizeof(module_info));
    if (module_file == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(helper_file, &helper_info) ||
        !GetFileInformationByHandle(module_file, &module_info) ||
        (helper_info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        (module_info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        helper_info.nNumberOfLinks != 1u ||
        module_info.nNumberOfLinks != 1u ||
        helper_info.dwVolumeSerialNumber != module_info.dwVolumeSerialNumber ||
        helper_info.nFileIndexHigh != module_info.nFileIndexHigh ||
        helper_info.nFileIndexLow != module_info.nFileIndexLow) {
        if (module_file != INVALID_HANDLE_VALUE) CloseHandle(module_file);
        CloseHandle(helper_file);
        CloseHandle(parent);
        DeleteFileW(argv[4]);
        return 4;
    }
    CloseHandle(module_file);
    wait_result = WaitForSingleObject(parent, 120000u);
    CloseHandle(parent);
    if (wait_result != WAIT_OBJECT_0) {
        CloseHandle(helper_file);
        DeleteFileW(argv[4]);
        return 3;
    }
    update_mutex = windows_acquire_update_mutex(argv[5]);
    if (!update_mutex) {
        CloseHandle(helper_file);
        DeleteFileW(argv[4]);
        return 3;
    }

    /* The helper is an exact copy of the old target.  Binding both hashes
     * prevents this internal mode from becoming a generic file replacer. */
    if (!windows_update_stage_path_valid(argv[4], argv[5]) ||
        windows_sha256_handle(helper_file, helper_digest) != 0 ||
        windows_sha256_file_w(argv[5], target_digest) != 0 ||
        !windows_digest_matches_w(helper_digest, argv[7]) ||
        !windows_digest_matches_w(target_digest, argv[7]) ||
        windows_sha256_file_w(argv[4], stage_digest) != 0 ||
        !windows_digest_matches_w(stage_digest, argv[6])) {
        CloseHandle(helper_file);
        DeleteFileW(argv[4]);
        windows_release_update_mutex(update_mutex);
        return 4;
    }
    CloseHandle(helper_file);
    for (attempt = 0; attempt < 128u; ++attempt) {
        int written = _snwprintf(
            backup, sizeof(backup) / sizeof(backup[0]),
            L"%ls.jvman-backup-%08lx-%03u.tmp", argv[5],
            (unsigned long)GetCurrentProcessId(), attempt);
        DWORD attributes;
        if (written < 0 ||
            (size_t)written >= sizeof(backup) / sizeof(backup[0])) {
            DeleteFileW(argv[4]);
            windows_release_update_mutex(update_mutex);
            return 5;
        }
        attributes = GetFileAttributesW(backup);
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            (GetLastError() == ERROR_FILE_NOT_FOUND ||
             GetLastError() == ERROR_PATH_NOT_FOUND)) break;
    }
    if (attempt == 128u) {
        DeleteFileW(argv[4]);
        windows_release_update_mutex(update_mutex);
        return 5;
    }
    for (attempt = 0; attempt < 100u; ++attempt) {
        if (ReplaceFileW(argv[5], argv[4], backup,
                         REPLACEFILE_WRITE_THROUGH, NULL, NULL)) {
            replaced = 1;
            break;
        }
        replace_error = GetLastError();
        if (replace_error != ERROR_SHARING_VIOLATION &&
            replace_error != ERROR_ACCESS_DENIED) {
            break;
        }
        Sleep(50);
    }
    if (!replaced) {
        int target_is_old = 0;
        DWORD target_attributes;
        if (windows_sha256_file_w(argv[5], target_digest) == 0) {
            if (windows_digest_matches_w(target_digest, argv[6])) {
                replaced = 1;
            } else if (windows_digest_matches_w(target_digest, argv[7])) {
                target_is_old = 1;
            }
        }
        if (!replaced && !target_is_old) {
            target_attributes = GetFileAttributesW(argv[5]);
            if (target_attributes == INVALID_FILE_ATTRIBUTES &&
                (GetLastError() == ERROR_FILE_NOT_FOUND ||
                 GetLastError() == ERROR_PATH_NOT_FOUND) &&
                windows_sha256_file_w(backup, stage_digest) == 0 &&
                windows_digest_matches_w(stage_digest, argv[7]) &&
                MoveFileExW(backup, argv[5], MOVEFILE_WRITE_THROUGH) &&
                windows_sha256_file_w(argv[5], target_digest) == 0 &&
                windows_digest_matches_w(target_digest, argv[7])) {
                target_is_old = 1;
            }
        }
        if (!replaced) {
            if (target_is_old) {
                DeleteFileW(argv[4]);
                if (windows_sha256_file_w(backup, stage_digest) == 0 &&
                    windows_digest_matches_w(stage_digest, argv[7])) {
                    DeleteFileW(backup);
                }
            }
            SetLastError(replace_error);
            windows_release_update_mutex(update_mutex);
            return 6;
        }
    }
    if (windows_sha256_file_w(argv[5], target_digest) != 0 ||
        !windows_digest_matches_w(target_digest, argv[6])) {
        /* Keep the verified backup unless an atomic same-volume rename can
         * restore it. A failed recovery must never delete the last old copy. */
        if (windows_sha256_file_w(backup, stage_digest) != 0 ||
            !windows_digest_matches_w(stage_digest, argv[7]) ||
            !MoveFileExW(backup, argv[5],
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
            windows_sha256_file_w(argv[5], target_digest) != 0 ||
            !windows_digest_matches_w(target_digest, argv[7])) {
            windows_release_update_mutex(update_mutex);
            return 7;
        }
        windows_release_update_mutex(update_mutex);
        return 7;
    }
    DeleteFileW(backup);
    windows_release_update_mutex(update_mutex);
    return 0;
}
#endif

int platform_publish_executable_update(const char *staged, const char *target,
                                       const char *expected_sha256,
                                       const char *expected_current_sha256,
                                       int *deferred_out) {
    unsigned char digest[32];
    if (!staged || !target || !expected_sha256 ||
        !expected_current_sha256 || !deferred_out ||
        !platform_sha256_text_valid(expected_sha256) ||
        !platform_sha256_text_valid(expected_current_sha256)) {
        platform_set_error("invalid executable update publication request");
        return -1;
    }
    *deferred_out = 0;
    if (platform_sha256_file(staged, digest) != 0 ||
        !jvman_hex_equal(digest, sizeof(digest), expected_sha256)) {
        platform_set_error("staged executable failed final SHA-256 verification");
        return -1;
    }
#if defined(_WIN32)
    {
        WCHAR *wide_stage = NULL;
        WCHAR *wide_target = NULL;
        int result;
        if (platform_utf8_to_wide(staged, &wide_stage) != 0 ||
            platform_utf8_to_wide(target, &wide_target) != 0) {
            free(wide_stage);
            free(wide_target);
            return -1;
        }
        result = windows_start_update_helper(
            wide_stage, wide_target, expected_sha256,
            expected_current_sha256);
        free(wide_stage);
        free(wide_target);
        if (result != 0) return -1;
        *deferred_out = 1;
        return 0;
    }
#else
    {
        char current[JVMAN_PATH_MAX];
        char directory[JVMAN_PATH_MAX];
        const char *stage_name;
        const char *target_name = NULL;
        const char *suffix;
        size_t target_length;
        size_t staged_length;
        struct stat target_info;
        struct stat current_target_info;
        struct stat locked_target_info;
        struct stat stage_info;
        int directory_fd = -1;
        int target_fd;
        int target_flags = O_RDONLY;
#ifdef O_NOFOLLOW
        target_flags |= O_NOFOLLOW;
#endif
        if (platform_current_executable(current, sizeof(current)) != 0 ||
            strcmp(current, target) != 0) {
            platform_set_error("update target does not match this process");
            return -1;
        }
        target_length = strlen(target);
        staged_length = strlen(staged);
        if (staged_length <= target_length) {
            platform_set_error("staged update path does not match its target");
            return -1;
        }
        suffix = staged + target_length;
        if (strncmp(staged, target, target_length) != 0 ||
            strncmp(suffix, ".jvman-update-", 14u) != 0 ||
            !jvman_ends_with(suffix, ".tmp") ||
            strchr(suffix, '/') != NULL) {
            platform_set_error("staged update path does not match its target");
            return -1;
        }
        stage_name = strrchr(staged, '/');
        if (!stage_name || stage_name[1] == '\0') {
            platform_set_error("staged update has no parent directory");
            return -1;
        }
        ++stage_name;
        directory_fd = posix_open_update_directory(
            target, directory, sizeof(directory), &target_name);
        if (directory_fd < 0 ||
            fstatat(directory_fd, target_name, &target_info,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            fstatat(directory_fd, stage_name, &stage_info,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(target_info.st_mode) ||
            target_info.st_uid != geteuid() || target_info.st_nlink != 1 ||
            (target_info.st_mode &
             (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) != 0 ||
            !S_ISREG(stage_info.st_mode) ||
            stage_info.st_uid != geteuid() || stage_info.st_nlink != 1 ||
            (stage_info.st_mode &
             (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) != 0) {
            if (directory_fd >= 0) close(directory_fd);
            platform_set_error("update files failed final safety checks");
            return -1;
        }
        target_fd = openat(directory_fd, target_name, target_flags);
        if (target_fd < 0) {
            close(directory_fd);
            platform_set_error("cannot open the update target: %s",
                               strerror(errno));
            return -1;
        }
        if (posix_lock_update_target(target_fd) != 0) {
            int saved_error = errno;
            close(target_fd);
            close(directory_fd);
            platform_set_error("cannot lock the update target: %s",
                               strerror(saved_error));
            return -1;
        }
        if (fstat(target_fd, &locked_target_info) != 0 ||
            fstatat(directory_fd, target_name, &current_target_info,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(locked_target_info.st_mode) ||
            locked_target_info.st_uid != geteuid() ||
            locked_target_info.st_nlink != 1 ||
            (locked_target_info.st_mode &
             (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) != 0 ||
            locked_target_info.st_dev != current_target_info.st_dev ||
            locked_target_info.st_ino != current_target_info.st_ino ||
            posix_sha256_fd(target_fd, digest) != 0 ||
            !jvman_hex_equal(digest, sizeof(digest),
                             expected_current_sha256)) {
            close(target_fd);
            close(directory_fd);
            platform_set_error(
                "running executable changed while the update was prepared");
            return -1;
        }
        if (renameat(directory_fd, stage_name,
                     directory_fd, target_name) != 0) {
            int saved_error = errno;
            close(target_fd);
            close(directory_fd);
            platform_set_error("cannot atomically publish executable update: %s",
                               strerror(saved_error));
            return -1;
        }
        if (fsync(directory_fd) != 0) {
            int saved_error = errno;
            if (saved_error != EINVAL
#ifdef ENOTSUP
                && saved_error != ENOTSUP
#endif
#ifdef EOPNOTSUPP
                && saved_error != EOPNOTSUPP
#endif
            ) {
                close(target_fd);
                close(directory_fd);
                platform_set_error(
                    "executable was replaced, but directory persistence could not be confirmed: %s",
                    strerror(saved_error));
                return -1;
            }
        }
        close(target_fd);
        close(directory_fd);
        return 0;
    }
#endif
}

int platform_handle_update_helper(int argc, char **argv, int *handled_out) {
    if (!handled_out) return 2;
    *handled_out = 0;
#if defined(_WIN32)
    {
        int wide_argc = 0;
        WCHAR **wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
        int result;
        (void)argc;
        (void)argv;
        if (!wide_argv) {
            platform_set_windows_error("cannot parse the Windows command line");
            return 2;
        }
        if (wide_argc < 2 ||
            wcscmp(wide_argv[1],
                   L"--jvman-internal-apply-update-v1") != 0) {
            LocalFree(wide_argv);
            return 0;
        }
        *handled_out = 1;
        result = windows_update_worker(wide_argc, wide_argv);
        windows_delete_running_update_helper();
        LocalFree(wide_argv);
        return result;
    }
#else
    if (argc >= 2 && argv &&
        strcmp(argv[1], "--jvman-internal-apply-update-v1") == 0) {
        *handled_out = 1;
        return 2;
    }
    return 0;
#endif
}
