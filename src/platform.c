#include "platform.h"

#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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
    if (!platform_path_exists(path)) return 0;
#if defined(_WIN32)
    if (DeleteFileA(path)) return 0;
    platform_set_windows_error("cannot delete file");
#else
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
    file = fopen(temporary, "wb");
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

static int windows_directory_is_absolute(const char *path) {
    return path && ((isalpha((unsigned char)path[0]) && path[1] == ':' &&
                     (path[2] == '\\' || path[2] == '/')) ||
                    (path[0] == '\\' && path[1] == '\\'));
}

static int windows_resolve_trusted_command(const char *name, char *out, size_t out_size) {
    char directory[JVMAN_PATH_MAX];
    char candidate[JVMAN_PATH_MAX];
    UINT system_length;
    const char *path;
    const char *cursor;
    if (!name || !*name) return -1;
    if (windows_has_path_component(name)) {
        if (!windows_directory_is_absolute(name)) return -1;
        return windows_try_pathext_candidate(name, out, out_size);
    }
    system_length = GetSystemDirectoryA(directory, (UINT)sizeof(directory));
    if (system_length > 0 && system_length < sizeof(directory) &&
        jvman_path_join(candidate, sizeof(candidate), directory, name) == 0 &&
        windows_try_pathext_candidate(candidate, out, out_size) == 0) return 0;
    path = getenv("PATH");
    cursor = path;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, ';');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char expanded[JVMAN_PATH_MAX];
        if (length > 0 && length < sizeof(directory)) {
            memcpy(directory, cursor, length);
            directory[length] = '\0';
            if (length >= 2 && directory[0] == '"' && directory[length - 1] == '"') {
                memmove(directory, directory + 1, length - 2);
                directory[length - 2] = '\0';
            }
            {
                DWORD expanded_length = ExpandEnvironmentStringsA(
                    directory, expanded, (DWORD)sizeof(expanded));
                if (expanded_length > 0 && expanded_length < sizeof(expanded) &&
                windows_directory_is_absolute(expanded) &&
                jvman_path_join(candidate, sizeof(candidate), expanded, name) == 0 &&
                    windows_try_pathext_candidate(candidate, out, out_size) == 0) return 0;
            }
        }
        if (!end) break;
        cursor = end + 1;
    }
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
    if (!CreateProcessA(effective_argv[0], command_line, NULL, NULL, TRUE, 0, NULL, NULL,
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
#if defined(_WIN32)
    return windows_resolve_trusted_command(name, out, out_size);
#else
    const char *path = getenv("PATH");
    const char *cursor = path;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, ':');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char directory[JVMAN_PATH_MAX];
        char candidate[JVMAN_PATH_MAX];
        if (length > 0 && length < sizeof(directory)) {
            memcpy(directory, cursor, length);
            directory[length] = '\0';
            if (directory[0] == '/' &&
                jvman_path_join(candidate, sizeof(candidate), directory, name) == 0 &&
                access(candidate, X_OK) == 0) {
                return platform_absolute_path(candidate, out, out_size);
            }
        }
        if (!end) break;
        cursor = end + 1;
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
