#include "files.h"
#include "platform.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

static int join_path(char *out, size_t capacity,
                     const char *left, const char *right) {
    int written;
    if (!out || capacity == 0 || !left || !right) return -1;
    written = snprintf(out, capacity, "%s\\%s", left, right);
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int utf8_to_wide(const char *input, wchar_t *output, size_t capacity) {
    int required;
    if (!input || !output || capacity == 0 || capacity > INT_MAX) return -1;
    required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,
                                   output, (int)capacity);
    return required > 0 ? 0 : -1;
}

static int create_test_root(char *root, size_t capacity) {
    char temporary[JVMAN_PATH_MAX];
    DWORD length;
    unsigned int attempt;
    length = GetTempPathA((DWORD)sizeof(temporary), temporary);
    if (length == 0 || length >= sizeof(temporary)) return -1;
    for (attempt = 0; attempt < 128u; ++attempt) {
        int written = snprintf(
            root, capacity, "%sjvman-data-cleanup-%08lx-%08lx-%03u",
            temporary, (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetTickCount(), attempt);
        if (written < 0 || (size_t)written >= capacity) return -1;
        if (CreateDirectoryA(root, NULL)) return 0;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    }
    return -1;
}

static int require_path(const char *path, int expected, const char *message) {
    int present = platform_path_exists(path);
    if (!!present == !!expected) return 0;
    fprintf(stderr, "%s: %s\n", message, path);
    return -1;
}

int main(void) {
    char root[JVMAN_PATH_MAX] = {0};
    char install[JVMAN_PATH_MAX];
    char data[JVMAN_PATH_MAX];
    char external[JVMAN_PATH_MAX];
    char managed_dir[JVMAN_PATH_MAX];
    char managed_marker[JVMAN_PATH_MAX];
    char cache_dir[JVMAN_PATH_MAX];
    char cache_file[JVMAN_PATH_MAX];
    char source_file[JVMAN_PATH_MAX];
    char current[JVMAN_PATH_MAX];
    char external_marker[JVMAN_PATH_MAX];
    wchar_t install_wide[JVMAN_INSTALL_PATH_CHARS];
    wchar_t data_wide[JVMAN_INSTALL_PATH_CHARS];
    JvmanInstallPaths paths;
    JvmanInstallStatus cleanup_status;
    int result = 1;

    if (create_test_root(root, sizeof(root)) != 0 ||
        join_path(install, sizeof(install), root, "install") != 0 ||
        join_path(data, sizeof(data), root, "data") != 0 ||
        join_path(external, sizeof(external), root, "external-jdk") != 0 ||
        join_path(managed_dir, sizeof(managed_dir), data,
                  "jdks\\managed-test") != 0 ||
        join_path(managed_marker, sizeof(managed_marker), managed_dir,
                  "managed.marker") != 0 ||
        join_path(cache_dir, sizeof(cache_dir), data, "cache\\nested") != 0 ||
        join_path(cache_file, sizeof(cache_file), cache_dir, "cache.tmp") != 0 ||
        join_path(source_file, sizeof(source_file), data, "source.conf") != 0 ||
        join_path(current, sizeof(current), data, "current") != 0 ||
        join_path(external_marker, sizeof(external_marker), external,
                  "external.marker") != 0 ||
        platform_mkdirs(install) != 0 || platform_mkdirs(managed_dir) != 0 ||
        platform_mkdirs(cache_dir) != 0 || platform_mkdirs(external) != 0 ||
        platform_write_text_atomic(managed_marker, "managed\n") != 0 ||
        platform_write_text_atomic(cache_file, "cache\n") != 0 ||
        platform_write_text_atomic(source_file, "source\n") != 0 ||
        platform_write_text_atomic(external_marker, "external\n") != 0 ||
        !SetFileAttributesA(cache_file, FILE_ATTRIBUTE_READONLY) ||
        platform_replace_directory_link(current, external) != 0 ||
        utf8_to_wide(install, install_wide,
                     sizeof(install_wide) / sizeof(install_wide[0])) != 0 ||
        utf8_to_wide(data, data_wide,
                     sizeof(data_wide) / sizeof(data_wide[0])) != 0 ||
        jvman_install_paths_init(&paths, install_wide, data_wide) !=
            JVMAN_INSTALL_OK) {
        fprintf(stderr, "cannot prepare data cleanup fixture: %s\n",
                platform_last_error());
        goto done;
    }

    cleanup_status = jvman_install_remove_data(&paths, 0);
    if (cleanup_status != JVMAN_INSTALL_OK) {
        fwprintf(stderr, L"preserve-JDK cleanup returned %d: %ls\n",
                 (int)cleanup_status,
                 jvman_install_status_message(cleanup_status));
        goto done;
    }
    if (require_path(managed_marker, 1,
                     "managed JDK was not preserved") != 0 ||
        require_path(cache_file, 0, "cache file was not removed") != 0 ||
        require_path(source_file, 0, "source data was not removed") != 0 ||
        require_path(current, 0, "current junction was not removed") != 0 ||
        require_path(external_marker, 1,
                     "current junction target was traversed") != 0 ||
        require_path(data, 1, "data root containing jdks was removed") != 0) {
        fprintf(stderr, "preserve-JDK cleanup result is incomplete\n");
        goto done;
    }

    if (platform_mkdirs(cache_dir) != 0 ||
        platform_write_text_atomic(cache_file, "cache\n") != 0 ||
        jvman_install_remove_data(&paths, 1) != JVMAN_INSTALL_OK ||
        require_path(data, 0, "full data root was not removed") != 0 ||
        require_path(external_marker, 1,
                     "full cleanup removed an external JDK") != 0) {
        fprintf(stderr, "full data cleanup failed\n");
        goto done;
    }

    puts("All installer data cleanup tests passed.");
    result = 0;

done:
    if (cache_file[0] != '\0' && platform_path_exists(cache_file)) {
        (void)SetFileAttributesA(cache_file, FILE_ATTRIBUTE_NORMAL);
    }
    if (root[0] != '\0' && platform_path_exists(root) &&
        platform_remove_tree(root) != 0) {
        fprintf(stderr, "cannot remove data cleanup fixture: %s\n",
                platform_last_error());
        result = 1;
    }
    return result;
}
