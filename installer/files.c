#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include "files.h"

#include "../src/sha256.h"

#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Keep every temporary buffer bounded.  The package itself is streamed. */
#define JVMAN_FILE_COPY_BUFFER_SIZE (64u * 1024u)
#define JVMAN_SETUP_PREFIX_LIMIT (16ULL * 1024ULL * 1024ULL)
#define JVMAN_PE_MAX_SECTIONS 96u
#define JVMAN_PE_MAX_OPTIONAL_HEADER 4096u
#define JVMAN_TEMP_ATTEMPTS 128u
#define JVMAN_DATA_DELETE_BUFFER_SIZE (64u * 1024u)
#define JVMAN_DATA_DELETE_DEPTH_LIMIT 128u
#define JVMAN_DATA_DELETE_ENTRY_LIMIT 1000000u

static size_t bounded_wcslen(const wchar_t *value, size_t limit) {
    size_t length = 0;
    if (!value) return 0;
    while (length < limit && value[length] != L'\0') ++length;
    return length;
}

static int copy_wstr(wchar_t *destination, size_t capacity,
                     const wchar_t *source) {
    size_t length;
    if (!destination || capacity == 0 || !source) return 0;
    length = bounded_wcslen(source, capacity);
    if (length >= capacity) return 0;
    if (length != 0) memcpy(destination, source, length * sizeof(*source));
    destination[length] = L'\0';
    return 1;
}

static JvmanInstallStatus status_from_error(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_DIRECTORY:
            return JVMAN_INSTALL_NOT_FOUND;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return JVMAN_INSTALL_NO_MEMORY;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return JVMAN_INSTALL_INVALID_PATH;
        case ERROR_CANT_RESOLVE_FILENAME:
        case ERROR_REPARSE_TAG_INVALID:
            return JVMAN_INSTALL_PATH_REPARSE;
        default:
            return JVMAN_INSTALL_IO_ERROR;
    }
}

const wchar_t *jvman_install_status_message(JvmanInstallStatus status) {
    switch (status) {
        case JVMAN_INSTALL_OK: return L"ok";
        case JVMAN_INSTALL_INVALID_ARGUMENT: return L"invalid argument";
        case JVMAN_INSTALL_INVALID_PATH: return L"invalid path";
        case JVMAN_INSTALL_PATH_TOO_LONG: return L"path too long";
        case JVMAN_INSTALL_PATH_NOT_LOCAL: return L"path is not on a local disk";
        case JVMAN_INSTALL_PATH_REPARSE: return L"reparse point is not allowed";
        case JVMAN_INSTALL_PATH_NOT_FOUND: return L"path not found";
        case JVMAN_INSTALL_IO_ERROR: return L"Windows I/O error";
        case JVMAN_INSTALL_FORMAT_ERROR: return L"invalid setup format";
        case JVMAN_INSTALL_HASH_MISMATCH: return L"payload hash mismatch";
        case JVMAN_INSTALL_MARKER_INVALID: return L"invalid installation marker";
        case JVMAN_INSTALL_NOT_FOUND: return L"not found";
        case JVMAN_INSTALL_NO_MEMORY: return L"out of memory";
        case JVMAN_INSTALL_SELF_CLEANUP_REQUIRED:
            return L"out-of-process self-cleanup is required";
        default: return L"unknown installer error";
    }
}

static int path_prefix(const wchar_t *prefix, const wchar_t *value) {
    size_t prefix_length;
    if (!prefix || !value) return 0;
    prefix_length = wcslen(prefix);
    if (_wcsnicmp(prefix, value, prefix_length) != 0) return 0;
    return value[prefix_length] == L'\0' || value[prefix_length] == L'\\';
}

static int path_equal(const wchar_t *left, const wchar_t *right) {
    return left && right && _wcsicmp(left, right) == 0;
}

static int is_forbidden_path_char(wchar_t character) {
    /* Slash is intentionally rejected: install paths are canonicalized to
     * one Windows spelling and are never interpreted as URI-like strings. */
    return character < 0x20 || character == L'%' || character == L';' ||
           character == L'"' || character == L'\'' || character == L'/' ||
           character == L'<' || character == L'>' || character == L'|' ||
           character == L'*' || character == L'?';
}

static int path_has_bad_component(const wchar_t *path, size_t length) {
    size_t start = 3;
    size_t index;
    if (length < 3 || path[1] != L':' || path[2] != L'\\') return 1;
    for (index = 0; index < length; ++index) {
        if (is_forbidden_path_char(path[index])) return 1;
        if (path[index] == L':' && index != 1) return 1; /* ADS/device path */
    }
    if (length > 3 && path[length - 1] == L'\\') --length;
    while (start < length) {
        size_t end = start;
        while (end < length && path[end] != L'\\') ++end;
        if (end == start || (end - start == 1 && path[start] == L'.') ||
            (end - start == 2 && path[start] == L'.' &&
             path[start + 1] == L'.')) return 1;
        if (path[end - 1] == L'.' || path[end - 1] == L' ') return 1;
        start = end + 1;
    }
    return 0;
}

static int get_full_path(const wchar_t *input, wchar_t output[],
                         size_t output_capacity) {
    wchar_t input_copy[JVMAN_INSTALL_PATH_CHARS];
    DWORD result;
    if (!input || !output || output_capacity == 0 ||
        output_capacity > (DWORD)-1) return 0;
    /* Keep input and output separate; the Win32 API does not promise that
     * aliasing them is safe. */
    if (!copy_wstr(input_copy, sizeof(input_copy) / sizeof(input_copy[0]),
                   input)) return 0;
    result = GetFullPathNameW(input_copy, (DWORD)output_capacity, output, NULL);
    return result != 0 && result < output_capacity;
}

static void trim_path_tail(wchar_t *path) {
    size_t length;
    if (!path) return;
    length = wcslen(path);
    while (length > 3 && path[length - 1] == L'\\') {
        path[--length] = L'\0';
    }
}

static int path_is_windows_directory(const wchar_t *path) {
    wchar_t windows_directory[JVMAN_INSTALL_PATH_CHARS];
    wchar_t windows_directory_full[JVMAN_INSTALL_PATH_CHARS];
    wchar_t system_root[JVMAN_INSTALL_PATH_CHARS];
    wchar_t system_root_full[JVMAN_INSTALL_PATH_CHARS];
    DWORD length;
    length = GetWindowsDirectoryW(windows_directory,
                                  (DWORD)(sizeof(windows_directory) /
                                          sizeof(windows_directory[0])));
    if (length == 0 || length >= sizeof(windows_directory) /
                       sizeof(windows_directory[0]) ||
        !get_full_path(windows_directory, windows_directory_full,
                       sizeof(windows_directory_full) /
                       sizeof(windows_directory_full[0]))) {
        windows_directory[0] = L'\0';
    } else if (!copy_wstr(windows_directory,
                          sizeof(windows_directory) / sizeof(windows_directory[0]),
                          windows_directory_full)) {
        windows_directory[0] = L'\0';
    }
    length = GetEnvironmentVariableW(L"SystemRoot", system_root,
                                     (DWORD)(sizeof(system_root) /
                                             sizeof(system_root[0])));
    if (length == 0 || length >= sizeof(system_root) / sizeof(system_root[0]) ||
        !get_full_path(system_root, system_root_full,
                       sizeof(system_root_full) / sizeof(system_root_full[0]))) {
        system_root[0] = L'\0';
    } else if (!copy_wstr(system_root,
                          sizeof(system_root) / sizeof(system_root[0]),
                          system_root_full)) {
        system_root[0] = L'\0';
    }
    return (windows_directory[0] != L'\0' &&
            path_prefix(windows_directory, path)) ||
           (system_root[0] != L'\0' && path_prefix(system_root, path));
}

static JvmanInstallStatus check_existing_components(const wchar_t *path) {
    wchar_t component[JVMAN_INSTALL_PATH_CHARS];
    size_t length;
    size_t index;
    if (!path) return JVMAN_INSTALL_INVALID_ARGUMENT;
    length = wcslen(path);
    if (length >= sizeof(component) / sizeof(component[0])) {
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    component[0] = path[0];
    component[1] = path[1];
    component[2] = path[2];
    component[3] = L'\0';
    index = 3;
    while (index <= length) {
        size_t end = index;
        while (end < length && path[end] != L'\\') ++end;
        if (end > index) {
            size_t component_length = end;
            if (component_length >= sizeof(component) / sizeof(component[0])) {
                return JVMAN_INSTALL_PATH_TOO_LONG;
            }
            memcpy(component + 3, path + 3,
                   (end - 3) * sizeof(*component));
            component[component_length] = L'\0';
            {
                DWORD attributes = GetFileAttributesW(component);
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    DWORD error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND &&
                        error != ERROR_PATH_NOT_FOUND) {
                        return status_from_error(error);
                    }
                } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    return JVMAN_INSTALL_PATH_REPARSE;
                }
            }
        }
        if (end == length) break;
        index = end + 1;
    }
    return JVMAN_INSTALL_OK;
}

static JvmanInstallStatus validate_one_path(const wchar_t *input,
                                            wchar_t normalized[]) {
    wchar_t full[JVMAN_INSTALL_PATH_CHARS];
    size_t length;
    UINT drive_type;
    wchar_t root[4];
    if (!input || !normalized) return JVMAN_INSTALL_INVALID_ARGUMENT;
    length = bounded_wcslen(input, JVMAN_INSTALL_PATH_CHARS);
    if (length == 0) return JVMAN_INSTALL_INVALID_PATH;
    if (length >= JVMAN_INSTALL_PATH_CHARS) return JVMAN_INSTALL_PATH_TOO_LONG;
    if (length < 3 ||
        !((input[0] >= L'A' && input[0] <= L'Z') ||
          (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || input[2] != L'\\') {
        return JVMAN_INSTALL_PATH_NOT_LOCAL;
    }
    if (length >= 2 && input[0] == L'\\' && input[1] == L'\\') {
        return JVMAN_INSTALL_PATH_NOT_LOCAL;
    }
    if (path_has_bad_component(input, length)) {
        return JVMAN_INSTALL_INVALID_PATH;
    }
    if (!get_full_path(input, full, sizeof(full) / sizeof(full[0]))) {
        return status_from_error(GetLastError());
    }
    trim_path_tail(full);
    length = wcslen(full);
    if (length <= 3) return JVMAN_INSTALL_INVALID_PATH; /* never install at root */
    root[0] = full[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
    drive_type = GetDriveTypeW(root);
    if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE) {
        return JVMAN_INSTALL_PATH_NOT_LOCAL;
    }
    if (path_is_windows_directory(full)) return JVMAN_INSTALL_INVALID_PATH;
    if (!copy_wstr(normalized, JVMAN_INSTALL_PATH_CHARS, full)) {
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    return check_existing_components(normalized);
}

static int join_path(wchar_t output[], size_t capacity,
                     const wchar_t *directory, const wchar_t *name) {
    int written;
    if (!output || !directory || !name) return 0;
    written = _snwprintf(output, capacity, L"%ls\\%ls", directory, name);
    return written >= 0 && (size_t)written < capacity;
}

static JvmanInstallStatus validate_paths(const JvmanInstallPaths *paths,
                                         JvmanInstallPaths *normalized) {
    JvmanInstallPaths temporary;
    JvmanInstallStatus status;
    if (!paths) return JVMAN_INSTALL_INVALID_ARGUMENT;
    status = jvman_install_paths_init(&temporary, paths->install_dir,
                                      paths->data_home);
    if (status != JVMAN_INSTALL_OK) return status;
    if (normalized) *normalized = temporary;
    return JVMAN_INSTALL_OK;
}

JvmanInstallStatus jvman_install_paths_init(
    JvmanInstallPaths *paths,
    const wchar_t *install_dir,
    const wchar_t *data_home) {
    wchar_t install_normalized[JVMAN_INSTALL_PATH_CHARS];
    wchar_t data_normalized[JVMAN_INSTALL_PATH_CHARS];
    JvmanInstallStatus status;
    if (!paths || !install_dir || !data_home) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    memset(paths, 0, sizeof(*paths));
    status = validate_one_path(install_dir, install_normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    status = validate_one_path(data_home, data_normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    if (path_prefix(install_normalized, data_normalized) ||
        path_prefix(data_normalized, install_normalized)) {
        return JVMAN_INSTALL_INVALID_PATH;
    }
    if (!copy_wstr(paths->install_dir, JVMAN_INSTALL_PATH_CHARS,
                   install_normalized) ||
        !copy_wstr(paths->data_home, JVMAN_INSTALL_PATH_CHARS,
                   data_normalized) ||
        !join_path(paths->jvman_path, JVMAN_INSTALL_PATH_CHARS,
                   install_normalized, JVMAN_INSTALL_EXECUTABLE_NAME) ||
        !join_path(paths->marker_path, JVMAN_INSTALL_PATH_CHARS,
                   install_normalized, JVMAN_INSTALL_MARKER_NAME) ||
        !join_path(paths->uninstall_path, JVMAN_INSTALL_PATH_CHARS,
                   install_normalized, JVMAN_INSTALL_UNINSTALL_NAME)) {
        memset(paths, 0, sizeof(*paths));
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    return JVMAN_INSTALL_OK;
}

static int get_environment_value(const wchar_t *name, wchar_t value[],
                                 size_t capacity) {
    DWORD length;
    if (!name || !value || capacity == 0 || capacity > (DWORD)-1) return 0;
    SetLastError(ERROR_SUCCESS);
    length = GetEnvironmentVariableW(name, value, (DWORD)capacity);
    if (length == 0 || length >= capacity) {
        value[0] = L'\0';
        return 0;
    }
    return 1;
}

JvmanInstallStatus jvman_install_paths_default(JvmanInstallPaths *paths) {
    wchar_t local_appdata[JVMAN_INSTALL_PATH_CHARS];
    wchar_t install_dir[JVMAN_INSTALL_PATH_CHARS];
    wchar_t data_home[JVMAN_INSTALL_PATH_CHARS];
    wchar_t environment_home[JVMAN_INSTALL_PATH_CHARS];
    JvmanInstallStatus status;
    if (!paths) return JVMAN_INSTALL_INVALID_ARGUMENT;
    if (!get_environment_value(L"LOCALAPPDATA", local_appdata,
                               sizeof(local_appdata) / sizeof(local_appdata[0]))) {
        return JVMAN_INSTALL_PATH_NOT_FOUND;
    }
    if (!join_path(install_dir, sizeof(install_dir) / sizeof(install_dir[0]),
                   local_appdata, L"Programs\\jvman") ||
        !join_path(data_home, sizeof(data_home) / sizeof(data_home[0]),
                   local_appdata, L"jvman")) {
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    if (get_environment_value(L"JVMAN_HOME", environment_home,
                              sizeof(environment_home) /
                              sizeof(environment_home[0]))) {
        status = jvman_install_paths_init(paths, install_dir, environment_home);
        if (status == JVMAN_INSTALL_OK) return status;
    }
    return jvman_install_paths_init(paths, install_dir, data_home);
}

static JvmanInstallStatus ensure_directory_tree(const wchar_t *path) {
    size_t length;
    size_t index;
    wchar_t component[JVMAN_INSTALL_PATH_CHARS];
    if (!path) return JVMAN_INSTALL_INVALID_ARGUMENT;
    length = wcslen(path);
    if (length < 4 || length >= sizeof(component) / sizeof(component[0])) {
        return JVMAN_INSTALL_INVALID_PATH;
    }
    index = 3;
    while (index <= length) {
        size_t end = index;
        while (end < length && path[end] != L'\\') ++end;
        if (end > index) {
            DWORD attributes;
            memcpy(component, path, end * sizeof(*component));
            component[end] = L'\0';
            attributes = GetFileAttributesW(component);
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                    return status_from_error(error);
                }
                if (!CreateDirectoryW(component, NULL)) {
                    error = GetLastError();
                    if (error != ERROR_ALREADY_EXISTS) return status_from_error(error);
                }
                attributes = GetFileAttributesW(component);
            }
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                return status_from_error(GetLastError());
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return JVMAN_INSTALL_PATH_REPARSE;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return JVMAN_INSTALL_INVALID_PATH;
            }
        }
        if (end == length) break;
        index = end + 1;
    }
    return JVMAN_INSTALL_OK;
}

JvmanInstallStatus jvman_install_paths_create(
    const JvmanInstallPaths *paths) {
    JvmanInstallPaths normalized;
    JvmanInstallStatus status = validate_paths(paths, &normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    return ensure_directory_tree(normalized.install_dir);
}

static int read_exact(HANDLE handle, void *buffer, DWORD size) {
    unsigned char *bytes = (unsigned char *)buffer;
    DWORD done;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE ||
        (size != 0 && !buffer)) return 0;
    while (size != 0) {
        done = 0;
        if (!ReadFile(handle, bytes, size, &done, NULL) || done == 0) return 0;
        bytes += done;
        size -= done;
    }
    return 1;
}

static int read_at(HANDLE handle, uint64_t offset, void *buffer, DWORD size) {
    LARGE_INTEGER position;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE ||
        (size != 0 && !buffer) || offset > (uint64_t)LLONG_MAX) {
        return 0;
    }
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN)) {
        return 0;
    }
    return read_exact(handle, buffer, size);
}

static int get_size(HANDLE handle, uint64_t *size_out) {
    LARGE_INTEGER size;
    if (!size_out || !GetFileSizeEx(handle, &size) || size.QuadPart < 0) return 0;
    *size_out = (uint64_t)size.QuadPart;
    return 1;
}

static int validate_pe_prefix(HANDLE handle, uint64_t file_size,
                              uint64_t payload_offset) {
    unsigned char dos_header[64];
    unsigned char nt_prefix[24];
    unsigned char *optional = NULL;
    IMAGE_FILE_HEADER file_header;
    uint32_t pe_offset;
    uint16_t optional_size;
    uint16_t sections;
    uint64_t section_offset;
    uint64_t pe_end;
    uint64_t index;
    int valid = 0;
    if (file_size < sizeof(dos_header) || payload_offset > file_size ||
        !read_at(handle, 0, dos_header, sizeof(dos_header)) ||
        dos_header[0] != 'M' || dos_header[1] != 'Z') return 0;
    pe_offset = (uint32_t)dos_header[0x3c] |
                ((uint32_t)dos_header[0x3d] << 8) |
                ((uint32_t)dos_header[0x3e] << 16) |
                ((uint32_t)dos_header[0x3f] << 24);
    if (pe_offset < sizeof(dos_header) || pe_offset > file_size - 24u ||
        !read_at(handle, pe_offset, nt_prefix, sizeof(nt_prefix)) ||
        nt_prefix[0] != 'P' || nt_prefix[1] != 'E' || nt_prefix[2] != 0 ||
        nt_prefix[3] != 0) return 0;
    memcpy(&file_header, nt_prefix + 4, sizeof(file_header));
    sections = file_header.NumberOfSections;
    optional_size = file_header.SizeOfOptionalHeader;
    if (sections == 0 || sections > JVMAN_PE_MAX_SECTIONS ||
        optional_size == 0 || optional_size > JVMAN_PE_MAX_OPTIONAL_HEADER) {
        return 0;
    }
    section_offset = (uint64_t)pe_offset + 4u + sizeof(IMAGE_FILE_HEADER) +
                     optional_size;
    if (section_offset > payload_offset ||
        (uint64_t)sections * sizeof(IMAGE_SECTION_HEADER) >
            payload_offset - section_offset) return 0;
    optional = (unsigned char *)malloc(optional_size);
    if (!optional) return 0;
    if (!read_at(handle, (uint64_t)pe_offset + 4u + sizeof(IMAGE_FILE_HEADER),
                 optional, optional_size)) goto done;
    if (optional_size >= 64) {
        uint32_t headers_size = (uint32_t)optional[60] |
                                ((uint32_t)optional[61] << 8) |
                                ((uint32_t)optional[62] << 16) |
                                ((uint32_t)optional[63] << 24);
        if (headers_size != 0 && headers_size > payload_offset) goto done;
    }
    pe_end = section_offset + (uint64_t)sections * sizeof(IMAGE_SECTION_HEADER);
    for (index = 0; index < sections; ++index) {
        IMAGE_SECTION_HEADER section;
        uint64_t section_position = section_offset +
                                     index * sizeof(IMAGE_SECTION_HEADER);
        uint64_t raw_end;
        if (!read_at(handle, section_position, &section, sizeof(section))) goto done;
        if (section.SizeOfRawData == 0) continue;
        raw_end = (uint64_t)section.PointerToRawData + section.SizeOfRawData;
        if (raw_end > payload_offset || raw_end > file_size) goto done;
        if (raw_end > pe_end) pe_end = raw_end;
    }
    valid = pe_end <= payload_offset;
done:
    free(optional);
    return valid;
}

JvmanInstallStatus jvman_setup_payload_open(
    const wchar_t *setup_path,
    JvmanSetupPayload *payload) {
    wchar_t current_path[JVMAN_INSTALL_PATH_CHARS];
    unsigned char footer[JVMAN_SETUP_FOOTER_SIZE];
    unsigned char payload_magic[2];
    uint64_t file_size;
    uint64_t payload_size;
    uint64_t payload_offset;
    HANDLE handle;
    if (!payload) return JVMAN_INSTALL_INVALID_ARGUMENT;
    memset(payload, 0, sizeof(*payload));
    payload->setup_handle = INVALID_HANDLE_VALUE;
    if (!setup_path) {
        DWORD length = GetModuleFileNameW(NULL, current_path,
                                          (DWORD)(sizeof(current_path) /
                                                  sizeof(current_path[0])));
        if (length == 0 || length >= sizeof(current_path) / sizeof(current_path[0])) {
            return JVMAN_INSTALL_IO_ERROR;
        }
        setup_path = current_path;
    }
    if (bounded_wcslen(setup_path, JVMAN_INSTALL_PATH_CHARS) >=
        JVMAN_INSTALL_PATH_CHARS) return JVMAN_INSTALL_PATH_TOO_LONG;
    {
        DWORD attributes = GetFileAttributesW(setup_path);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return status_from_error(GetLastError());
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return JVMAN_INSTALL_PATH_REPARSE;
        }
    }
    handle = CreateFileW(setup_path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return status_from_error(GetLastError());
    if (!get_size(handle, &file_size) || file_size < JVMAN_SETUP_FOOTER_SIZE ||
        !read_at(handle, file_size - JVMAN_SETUP_FOOTER_SIZE,
                 footer, sizeof(footer)) ||
        jvman_setup_footer_decode(footer, &payload_size, payload->digest) != 0 ||
        payload_size > file_size - JVMAN_SETUP_FOOTER_SIZE) {
        CloseHandle(handle);
        return JVMAN_INSTALL_FORMAT_ERROR;
    }
    payload_offset = file_size - JVMAN_SETUP_FOOTER_SIZE - payload_size;
    /* The setup stub is expected to be a small native launcher.  Bounding
     * the prefix prevents a crafted file with a tiny valid payload but an
     * arbitrarily large leading blob from being accepted. */
    if (payload_offset > JVMAN_SETUP_PREFIX_LIMIT) {
        CloseHandle(handle);
        return JVMAN_INSTALL_FORMAT_ERROR;
    }
    if (!validate_pe_prefix(handle, file_size, payload_offset) ||
        !read_at(handle, payload_offset, payload_magic, sizeof(payload_magic)) ||
        payload_magic[0] != 'M' || payload_magic[1] != 'Z') {
        CloseHandle(handle);
        return JVMAN_INSTALL_FORMAT_ERROR;
    }
    if (!copy_wstr(payload->setup_path, JVMAN_INSTALL_PATH_CHARS, setup_path)) {
        CloseHandle(handle);
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    payload->setup_handle = handle;
    payload->payload_offset = payload_offset;
    payload->payload_size = payload_size;
    return JVMAN_INSTALL_OK;
}

void jvman_setup_payload_close(JvmanSetupPayload *payload) {
    if (!payload) return;
    if (payload->setup_handle != NULL &&
        payload->setup_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(payload->setup_handle);
    }
    memset(payload, 0, sizeof(*payload));
    payload->setup_handle = INVALID_HANDLE_VALUE;
}

static JvmanInstallStatus check_target_file(const wchar_t *target_path) {
    DWORD attributes;
    if (!target_path) return JVMAN_INSTALL_INVALID_ARGUMENT;
    attributes = GetFileAttributesW(target_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return JVMAN_INSTALL_OK;
        }
        return status_from_error(error);
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return JVMAN_INSTALL_PATH_REPARSE;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return JVMAN_INSTALL_INVALID_PATH;
    }
    return JVMAN_INSTALL_OK;
}

static JvmanInstallStatus validate_target_path(const wchar_t *target_path) {
    wchar_t parent[JVMAN_INSTALL_PATH_CHARS];
    wchar_t normalized_parent[JVMAN_INSTALL_PATH_CHARS];
    const wchar_t *slash;
    size_t parent_length;
    size_t name_length;
    size_t index;
    JvmanInstallStatus status;
    if (!target_path) return JVMAN_INSTALL_INVALID_ARGUMENT;
    if (bounded_wcslen(target_path, JVMAN_INSTALL_PATH_CHARS) >=
        JVMAN_INSTALL_PATH_CHARS) return JVMAN_INSTALL_PATH_TOO_LONG;
    slash = wcsrchr(target_path, L'\\');
    if (!slash || slash == target_path) return JVMAN_INSTALL_INVALID_PATH;
    parent_length = (size_t)(slash - target_path);
    if (parent_length >= sizeof(parent) / sizeof(parent[0])) {
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    memcpy(parent, target_path, parent_length * sizeof(*parent));
    parent[parent_length] = L'\0';
    status = validate_one_path(parent, normalized_parent);
    if (status != JVMAN_INSTALL_OK) return status;
    (void)normalized_parent;
    name_length = wcslen(slash + 1);
    if (name_length == 0 || name_length > 255) return JVMAN_INSTALL_INVALID_PATH;
    for (index = 0; index < name_length; ++index) {
        wchar_t value = slash[1 + index];
        if (is_forbidden_path_char(value) || value == L'\\' || value == L':') {
            return JVMAN_INSTALL_INVALID_PATH;
        }
    }
    if (slash[1 + name_length - 1] == L'.' ||
        slash[1 + name_length - 1] == L' ') return JVMAN_INSTALL_INVALID_PATH;
    return JVMAN_INSTALL_OK;
}

static int make_temp_file(const wchar_t *target_path, wchar_t temp_path[],
                          size_t temp_capacity, HANDLE *handle_out) {
    wchar_t directory[JVMAN_INSTALL_PATH_CHARS];
    const wchar_t *slash;
    unsigned long seed;
    unsigned int attempt;
    if (!target_path || !temp_path || !handle_out) return 0;
    slash = wcsrchr(target_path, L'\\');
    if (!slash || slash == target_path) return 0;
    if ((size_t)(slash - target_path) >= sizeof(directory) / sizeof(directory[0])) {
        return 0;
    }
    memcpy(directory, target_path, (size_t)(slash - target_path) * sizeof(*directory));
    directory[slash - target_path] = L'\0';
    seed = GetCurrentProcessId() ^ GetTickCount();
    for (attempt = 0; attempt < JVMAN_TEMP_ATTEMPTS; ++attempt) {
        int written = _snwprintf(temp_path, temp_capacity,
                                 L"%ls\\.jvman-%08lx-%08x.tmp",
                                 directory, seed, attempt);
        HANDLE handle;
        if (written < 0 || (size_t)written >= temp_capacity) return 0;
        handle = CreateFileW(temp_path, GENERIC_WRITE | GENERIC_READ,
                             FILE_SHARE_READ, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY,
                             NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            *handle_out = handle;
            return 1;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        ++seed;
    }
    return 0;
}

static JvmanInstallStatus publish_temp(HANDLE temporary,
                                       const wchar_t *temp_path,
                                       const wchar_t *target_path) {
    if (!FlushFileBuffers(temporary)) {
        JvmanInstallStatus status = status_from_error(GetLastError());
        CloseHandle(temporary);
        return status;
    }
    if (!CloseHandle(temporary)) return status_from_error(GetLastError());
    if (!MoveFileExW(temp_path, target_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return status_from_error(GetLastError());
    }
    return JVMAN_INSTALL_OK;
}

JvmanInstallStatus jvman_setup_payload_extract(
    const JvmanSetupPayload *payload,
    const wchar_t *target_path) {
    wchar_t temporary_path[JVMAN_INSTALL_PATH_CHARS];
    unsigned char buffer[JVMAN_FILE_COPY_BUFFER_SIZE];
    JvmanSha256 sha;
    HANDLE temporary = INVALID_HANDLE_VALUE;
    uint64_t remaining;
    JvmanInstallStatus status;
    LARGE_INTEGER payload_position;
    if (!payload || payload->setup_handle == NULL ||
        payload->setup_handle == INVALID_HANDLE_VALUE || !target_path) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    if (bounded_wcslen(target_path, JVMAN_INSTALL_PATH_CHARS) >=
        JVMAN_INSTALL_PATH_CHARS) return JVMAN_INSTALL_PATH_TOO_LONG;
    status = validate_target_path(target_path);
    if (status != JVMAN_INSTALL_OK) return status;
    status = check_target_file(target_path);
    if (status != JVMAN_INSTALL_OK) return status;
    if (!make_temp_file(target_path, temporary_path,
                        sizeof(temporary_path) / sizeof(temporary_path[0]),
                        &temporary)) return status_from_error(GetLastError());
    if (payload->payload_offset > (uint64_t)LLONG_MAX) {
        CloseHandle(temporary);
        DeleteFileW(temporary_path);
        return JVMAN_INSTALL_FORMAT_ERROR;
    }
    payload_position.QuadPart = (LONGLONG)payload->payload_offset;
    if (!SetFilePointerEx(payload->setup_handle, payload_position, NULL,
                          FILE_BEGIN)) {
        status = status_from_error(GetLastError());
        CloseHandle(temporary);
        DeleteFileW(temporary_path);
        return status;
    }
    jvman_sha256_init(&sha);
    remaining = payload->payload_size;
    while (remaining != 0) {
        DWORD requested = remaining > sizeof(buffer) ? (DWORD)sizeof(buffer) :
                                                        (DWORD)remaining;
        DWORD read_count = 0;
        DWORD written = 0;
        if (!ReadFile(payload->setup_handle, buffer, requested, &read_count, NULL) ||
            read_count == 0 ||
            !WriteFile(temporary, buffer, read_count, &written, NULL) ||
            written != read_count) {
            status = status_from_error(GetLastError());
            CloseHandle(temporary);
            DeleteFileW(temporary_path);
            return status;
        }
        jvman_sha256_update(&sha, buffer, read_count);
        remaining -= read_count;
    }
    {
        unsigned char digest[32];
        unsigned int index;
        jvman_sha256_final(&sha, digest);
        status = JVMAN_INSTALL_OK;
        for (index = 0; index < sizeof(digest); ++index) {
            if (digest[index] != payload->digest[index]) status = JVMAN_INSTALL_HASH_MISMATCH;
        }
    }
    if (status != JVMAN_INSTALL_OK) {
        CloseHandle(temporary);
        DeleteFileW(temporary_path);
        return status;
    }
    status = check_target_file(target_path);
    if (status == JVMAN_INSTALL_OK) {
        status = publish_temp(temporary, temporary_path, target_path);
        temporary = INVALID_HANDLE_VALUE;
    }
    if (temporary != INVALID_HANDLE_VALUE) {
        CloseHandle(temporary);
        DeleteFileW(temporary_path);
    }
    return status;
}

static JvmanInstallStatus copy_file_atomically(const wchar_t *source_path,
                                               const wchar_t *target_path) {
    HANDLE source = INVALID_HANDLE_VALUE;
    HANDLE temporary = INVALID_HANDLE_VALUE;
    wchar_t temporary_path[JVMAN_INSTALL_PATH_CHARS];
    unsigned char buffer[JVMAN_FILE_COPY_BUFFER_SIZE];
    uint64_t source_size;
    uint64_t remaining;
    JvmanInstallStatus status;
    DWORD attributes;
    if (!source_path || !target_path) return JVMAN_INSTALL_INVALID_ARGUMENT;
    attributes = GetFileAttributesW(source_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return status_from_error(GetLastError());
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return JVMAN_INSTALL_PATH_REPARSE;
    status = check_target_file(target_path);
    if (status != JVMAN_INSTALL_OK) return status;
    source = CreateFileW(source_path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (source == INVALID_HANDLE_VALUE) return status_from_error(GetLastError());
    if (!get_size(source, &source_size) ||
        source_size > JVMAN_SETUP_PAYLOAD_LIMIT + JVMAN_SETUP_PREFIX_LIMIT) {
        CloseHandle(source);
        return JVMAN_INSTALL_FORMAT_ERROR;
    }
    if (!make_temp_file(target_path, temporary_path,
                        sizeof(temporary_path) / sizeof(temporary_path[0]),
                        &temporary)) {
        status = status_from_error(GetLastError());
        CloseHandle(source);
        return status;
    }
    remaining = source_size;
    while (remaining != 0) {
        DWORD requested = remaining > sizeof(buffer) ? (DWORD)sizeof(buffer) :
                                                        (DWORD)remaining;
        DWORD read_count = 0;
        DWORD written = 0;
        if (!ReadFile(source, buffer, requested, &read_count, NULL) ||
            read_count == 0 || !WriteFile(temporary, buffer, read_count, &written, NULL) ||
            written != read_count) {
            status = status_from_error(GetLastError());
            CloseHandle(source);
            CloseHandle(temporary);
            DeleteFileW(temporary_path);
            return status;
        }
        remaining -= read_count;
    }
    CloseHandle(source);
    status = publish_temp(temporary, temporary_path, target_path);
    temporary = INVALID_HANDLE_VALUE;
    if (temporary != INVALID_HANDLE_VALUE) CloseHandle(temporary);
    if (status != JVMAN_INSTALL_OK) DeleteFileW(temporary_path);
    return status;
}

JvmanInstallStatus jvman_install_copy_self_uninstaller(
    const JvmanInstallPaths *paths) {
    JvmanInstallPaths normalized;
    wchar_t source_path[JVMAN_INSTALL_PATH_CHARS];
    DWORD length;
    JvmanInstallStatus status = validate_paths(paths, &normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    length = GetModuleFileNameW(NULL, source_path,
                                (DWORD)(sizeof(source_path) /
                                        sizeof(source_path[0])));
    if (length == 0 || length >= sizeof(source_path) / sizeof(source_path[0])) {
        return JVMAN_INSTALL_IO_ERROR;
    }
    if (path_equal(source_path, normalized.uninstall_path)) {
        return JVMAN_INSTALL_INVALID_PATH;
    }
    return copy_file_atomically(source_path, normalized.uninstall_path);
}

static int marker_id_valid(const wchar_t *installation_id, size_t *length_out) {
    size_t length;
    size_t index;
    if (!installation_id) return 0;
    length = bounded_wcslen(installation_id, JVMAN_INSTALL_MARKER_ID_CHARS);
    if (length == 0 || length >= JVMAN_INSTALL_MARKER_ID_CHARS) return 0;
    for (index = 0; index < length; ++index) {
        wchar_t value = installation_id[index];
        if (!((value >= L'A' && value <= L'Z') ||
              (value >= L'a' && value <= L'z') ||
              (value >= L'0' && value <= L'9') || value == L'.' ||
              value == L'_' || value == L'-')) return 0;
    }
    if (length_out) *length_out = length;
    return 1;
}

JvmanInstallStatus jvman_install_marker_write(
    const JvmanInstallPaths *paths,
    const wchar_t *installation_id) {
    JvmanInstallPaths normalized;
    wchar_t temporary_path[JVMAN_INSTALL_PATH_CHARS];
    HANDLE temporary = INVALID_HANDLE_VALUE;
    size_t length;
    DWORD written;
    JvmanInstallStatus status;
    if (!marker_id_valid(installation_id, &length)) {
        return JVMAN_INSTALL_MARKER_INVALID;
    }
    status = validate_paths(paths, &normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    status = check_target_file(normalized.marker_path);
    if (status != JVMAN_INSTALL_OK) return status;
    if (!make_temp_file(normalized.marker_path, temporary_path,
                        sizeof(temporary_path) / sizeof(temporary_path[0]),
                        &temporary)) return status_from_error(GetLastError());
    if (!WriteFile(temporary, installation_id, (DWORD)(length * sizeof(wchar_t)),
                   &written, NULL) || written != length * sizeof(wchar_t) ||
        !FlushFileBuffers(temporary)) {
        status = status_from_error(GetLastError());
        CloseHandle(temporary);
        DeleteFileW(temporary_path);
        return status;
    }
    status = publish_temp(temporary, temporary_path, normalized.marker_path);
    temporary = INVALID_HANDLE_VALUE;
    if (status != JVMAN_INSTALL_OK) DeleteFileW(temporary_path);
    return status;
}

JvmanInstallStatus jvman_install_marker_read(
    const JvmanInstallPaths *paths,
    wchar_t *installation_id,
    size_t installation_id_capacity) {
    JvmanInstallPaths normalized;
    HANDLE file;
    unsigned char bytes[JVMAN_INSTALL_MARKER_ID_CHARS * sizeof(wchar_t)];
    DWORD read_count = 0;
    size_t index;
    size_t characters;
    JvmanInstallStatus status;
    if (!installation_id || installation_id_capacity == 0) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    installation_id[0] = L'\0';
    status = validate_paths(paths, &normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    file = CreateFileW(normalized.marker_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return JVMAN_INSTALL_NOT_FOUND;
        }
        return status_from_error(error);
    }
    if (!ReadFile(file, bytes, sizeof(bytes), &read_count, NULL)) {
        status = status_from_error(GetLastError());
        CloseHandle(file);
        return status;
    }
    {
        unsigned char extra;
        DWORD extra_read = 0;
        if (read_count == sizeof(bytes) && ReadFile(file, &extra, 1, &extra_read, NULL) &&
            extra_read != 0) {
            CloseHandle(file);
            return JVMAN_INSTALL_MARKER_INVALID;
        }
    }
    CloseHandle(file);
    if (read_count == 0 || (read_count % sizeof(wchar_t)) != 0) {
        return JVMAN_INSTALL_MARKER_INVALID;
    }
    characters = read_count / sizeof(wchar_t);
    if (characters >= installation_id_capacity || characters >= JVMAN_INSTALL_MARKER_ID_CHARS) {
        return JVMAN_INSTALL_MARKER_INVALID;
    }
    for (index = 0; index < characters; ++index) {
        wchar_t value;
        memcpy(&value, bytes + index * sizeof(wchar_t), sizeof(value));
        /* The marker is a fixed-width UTF-16 payload without a terminator.
         * An embedded NUL must not be silently truncated by the subsequent
         * bounded validation/comparison. */
        if (value == L'\0') {
            return JVMAN_INSTALL_MARKER_INVALID;
        }
        installation_id[index] = value;
    }
    installation_id[characters] = L'\0';
    if (!marker_id_valid(installation_id, NULL)) {
        installation_id[0] = L'\0';
        return JVMAN_INSTALL_MARKER_INVALID;
    }
    return JVMAN_INSTALL_OK;
}

static int same_as_current_module(const wchar_t *path) {
    wchar_t current[JVMAN_INSTALL_PATH_CHARS];
    DWORD length = GetModuleFileNameW(NULL, current,
                                      (DWORD)(sizeof(current) / sizeof(current[0])));
    return length != 0 && length < sizeof(current) / sizeof(current[0]) &&
           path_equal(current, path);
}

static JvmanInstallStatus remove_whitelisted_file(const wchar_t *path) {
    DWORD attributes;
    if (!path) return JVMAN_INSTALL_INVALID_ARGUMENT;
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return JVMAN_INSTALL_OK;
        }
        return status_from_error(error);
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return JVMAN_INSTALL_PATH_REPARSE;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return JVMAN_INSTALL_INVALID_PATH;
    }
    if (!DeleteFileW(path)) {
        DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) {
            if (MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT) != 0) {
                return JVMAN_INSTALL_OK;
            }
        }
        return status_from_error(error);
    }
    return JVMAN_INSTALL_OK;
}

static int data_entry_name_equal(const wchar_t *name, size_t length,
                                 const wchar_t *expected) {
    size_t expected_length;
    if (!name || !expected) return 0;
    expected_length = wcslen(expected);
    return length == expected_length &&
           _wcsnicmp(name, expected, length) == 0;
}

static int data_entry_name_valid(const wchar_t *name, size_t length) {
    size_t index;
    if (!name || length == 0 || length >= JVMAN_INSTALL_PATH_CHARS) return 0;
    for (index = 0; index < length; ++index) {
        wchar_t value = name[index];
        if (value == L'\0' || value == L'\\' || value == L'/' ||
            value == L':') {
            return 0;
        }
    }
    return 1;
}

static JvmanInstallStatus data_next_child(
    HANDLE directory, const wchar_t *skip_name,
    wchar_t child_name[], size_t child_capacity, int *found_out) {
    unsigned char buffer[JVMAN_DATA_DELETE_BUFFER_SIZE];
    FILE_INFO_BY_HANDLE_CLASS info_class = FileIdBothDirectoryRestartInfo;
    if (directory == NULL || directory == INVALID_HANDLE_VALUE ||
        !child_name || child_capacity == 0 || !found_out) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    *found_out = 0;
    child_name[0] = L'\0';
    for (;;) {
        size_t offset = 0;
        if (!GetFileInformationByHandleEx(
                directory, info_class, buffer, (DWORD)sizeof(buffer))) {
            DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) {
                return JVMAN_INSTALL_OK;
            }
            return status_from_error(error);
        }
        info_class = FileIdBothDirectoryInfo;
        for (;;) {
            FILE_ID_BOTH_DIR_INFO *entry;
            size_t fixed_size = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            size_t remaining;
            size_t name_length;
            if (offset > sizeof(buffer) ||
                sizeof(buffer) - offset < fixed_size) {
                return JVMAN_INSTALL_FORMAT_ERROR;
            }
            remaining = sizeof(buffer) - offset;
            entry = (FILE_ID_BOTH_DIR_INFO *)(void *)(buffer + offset);
            if ((entry->FileNameLength % sizeof(wchar_t)) != 0 ||
                entry->FileNameLength > remaining - fixed_size) {
                return JVMAN_INSTALL_FORMAT_ERROR;
            }
            name_length = entry->FileNameLength / sizeof(wchar_t);
            if (!data_entry_name_valid(entry->FileName, name_length)) {
                return JVMAN_INSTALL_INVALID_PATH;
            }
            if (!data_entry_name_equal(entry->FileName, name_length, L".") &&
                !data_entry_name_equal(entry->FileName, name_length, L"..") &&
                (!skip_name ||
                 !data_entry_name_equal(entry->FileName, name_length,
                                        skip_name))) {
                if (name_length >= child_capacity) {
                    return JVMAN_INSTALL_PATH_TOO_LONG;
                }
                memcpy(child_name, entry->FileName,
                       name_length * sizeof(*child_name));
                child_name[name_length] = L'\0';
                *found_out = 1;
                return JVMAN_INSTALL_OK;
            }
            if (entry->NextEntryOffset == 0) break;
            if (entry->NextEntryOffset < fixed_size ||
                entry->NextEntryOffset > remaining) {
                return JVMAN_INSTALL_FORMAT_ERROR;
            }
            offset += entry->NextEntryOffset;
        }
    }
}

static JvmanInstallStatus data_handle_final_path(
    HANDLE handle, wchar_t path[], size_t path_capacity) {
    DWORD length;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE || !path ||
        path_capacity == 0 || path_capacity > UINT32_MAX) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    length = GetFinalPathNameByHandleW(
        handle, path, (DWORD)path_capacity,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) return status_from_error(GetLastError());
    if (length >= path_capacity) return JVMAN_INSTALL_PATH_TOO_LONG;
    return JVMAN_INSTALL_OK;
}

static JvmanInstallStatus data_mark_handle_for_deletion(
    HANDLE handle, DWORD attributes) {
    FILE_DISPOSITION_INFO disposition;
    DWORD error;
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(
            handle, FileDispositionInfo, &disposition,
            (DWORD)sizeof(disposition))) {
        return JVMAN_INSTALL_OK;
    }
    error = GetLastError();
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        FILE_BASIC_INFO basic;
        if (GetFileInformationByHandleEx(
                handle, FileBasicInfo, &basic, (DWORD)sizeof(basic))) {
            basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (basic.FileAttributes == 0) {
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            if (SetFileInformationByHandle(
                    handle, FileBasicInfo, &basic, (DWORD)sizeof(basic)) &&
                SetFileInformationByHandle(
                    handle, FileDispositionInfo, &disposition,
                    (DWORD)sizeof(disposition))) {
                return JVMAN_INSTALL_OK;
            }
            error = GetLastError();
        }
    }
    return status_from_error(error);
}

static JvmanInstallStatus data_open_handle(
    const wchar_t *path, HANDLE *handle_out, DWORD *attributes_out) {
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION information;
    if (!path || !handle_out || !attributes_out) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    *handle_out = INVALID_HANDLE_VALUE;
    *attributes_out = 0;
    handle = CreateFileW(
        path, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                  FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return JVMAN_INSTALL_NOT_FOUND;
        }
        return status_from_error(error);
    }
    if (!GetFileInformationByHandle(handle, &information)) {
        JvmanInstallStatus status = status_from_error(GetLastError());
        CloseHandle(handle);
        return status;
    }
    *handle_out = handle;
    *attributes_out = information.dwFileAttributes;
    return JVMAN_INSTALL_OK;
}

static JvmanInstallStatus data_remove_node(
    const wchar_t *path, unsigned int depth, size_t *entry_count) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD attributes = 0;
    JvmanInstallStatus status;
    if (!path || !entry_count) return JVMAN_INSTALL_INVALID_ARGUMENT;
    if (depth > JVMAN_DATA_DELETE_DEPTH_LIMIT) {
        return JVMAN_INSTALL_PATH_TOO_LONG;
    }
    if (*entry_count >= JVMAN_DATA_DELETE_ENTRY_LIMIT) {
        return JVMAN_INSTALL_IO_ERROR;
    }
    ++*entry_count;
    status = data_open_handle(path, &handle, &attributes);
    if (status == JVMAN_INSTALL_NOT_FOUND) return JVMAN_INSTALL_OK;
    if (status != JVMAN_INSTALL_OK) return status;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        wchar_t directory[JVMAN_INSTALL_PATH_CHARS];
        status = data_handle_final_path(
            handle, directory, sizeof(directory) / sizeof(directory[0]));
        while (status == JVMAN_INSTALL_OK) {
            wchar_t name[JVMAN_INSTALL_PATH_CHARS];
            wchar_t child[JVMAN_INSTALL_PATH_CHARS];
            int found = 0;
            status = data_next_child(
                handle, NULL, name, sizeof(name) / sizeof(name[0]), &found);
            if (status != JVMAN_INSTALL_OK || !found) break;
            if (!join_path(child, sizeof(child) / sizeof(child[0]),
                           directory, name)) {
                status = JVMAN_INSTALL_PATH_TOO_LONG;
                break;
            }
            status = data_remove_node(child, depth + 1u, entry_count);
        }
    }
    if (status == JVMAN_INSTALL_OK) {
        status = data_mark_handle_for_deletion(handle, attributes);
    }
    CloseHandle(handle);
    return status;
}

JvmanInstallStatus jvman_install_remove_data(
    const JvmanInstallPaths *paths, int remove_managed_jdks) {
    JvmanInstallPaths normalized;
    HANDLE root = INVALID_HANDLE_VALUE;
    DWORD attributes = 0;
    wchar_t directory[JVMAN_INSTALL_PATH_CHARS];
    size_t entry_count = 0;
    JvmanInstallStatus status;
    if (remove_managed_jdks != 0 && remove_managed_jdks != 1) {
        return JVMAN_INSTALL_INVALID_ARGUMENT;
    }
    status = validate_paths(paths, &normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    status = data_open_handle(normalized.data_home, &root, &attributes);
    if (status == JVMAN_INSTALL_NOT_FOUND) return JVMAN_INSTALL_OK;
    if (status != JVMAN_INSTALL_OK) return status;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        CloseHandle(root);
        return JVMAN_INSTALL_INVALID_PATH;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(root);
        return JVMAN_INSTALL_PATH_REPARSE;
    }
    status = data_handle_final_path(
        root, directory, sizeof(directory) / sizeof(directory[0]));
    while (status == JVMAN_INSTALL_OK) {
        wchar_t name[JVMAN_INSTALL_PATH_CHARS];
        wchar_t child[JVMAN_INSTALL_PATH_CHARS];
        int found = 0;
        status = data_next_child(
            root, remove_managed_jdks ? NULL : L"jdks", name,
            sizeof(name) / sizeof(name[0]), &found);
        if (status != JVMAN_INSTALL_OK || !found) break;
        if (!join_path(child, sizeof(child) / sizeof(child[0]),
                       directory, name)) {
            status = JVMAN_INSTALL_PATH_TOO_LONG;
            break;
        }
        status = data_remove_node(child, 1u, &entry_count);
    }
    if (status == JVMAN_INSTALL_OK && remove_managed_jdks) {
        status = data_mark_handle_for_deletion(root, attributes);
    }
    CloseHandle(root);
    return status;
}

JvmanInstallStatus jvman_install_uninstall(
    const JvmanInstallPaths *paths) {
    JvmanInstallPaths normalized;
    JvmanInstallStatus status;
    DWORD attributes;
    status = validate_paths(paths, &normalized);
    if (status != JVMAN_INSTALL_OK) return status;
    status = remove_whitelisted_file(normalized.jvman_path);
    if (status != JVMAN_INSTALL_OK) return status;
    if (same_as_current_module(normalized.uninstall_path)) {
        return JVMAN_INSTALL_SELF_CLEANUP_REQUIRED;
    }
    /* Keep the marker until every other owned file has been removed.  If a
     * sharing violation prevents the final step, a subsequent invocation can
     * still authenticate the installation and retry cleanup. */
    status = remove_whitelisted_file(normalized.uninstall_path);
    if (status != JVMAN_INSTALL_OK) return status;
    status = remove_whitelisted_file(normalized.marker_path);
    if (status != JVMAN_INSTALL_OK) return status;
    attributes = GetFileAttributesW(normalized.install_dir);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        /* RemoveDirectoryW is intentionally used once: user-created files or
         * subdirectories are left intact rather than recursively removed. */
        if (!RemoveDirectoryW(normalized.install_dir)) {
            DWORD error = GetLastError();
            if (error != ERROR_DIR_NOT_EMPTY && error != ERROR_PATH_NOT_FOUND &&
                error != ERROR_FILE_NOT_FOUND) return status_from_error(error);
        }
    } else if (attributes != INVALID_FILE_ATTRIBUTES) {
        return JVMAN_INSTALL_PATH_REPARSE;
    }
    return JVMAN_INSTALL_OK;
}
