#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "pathlist.h"

#include <windows.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define JVMAN_PATHLIST_MAX_LENGTH (JVMAN_PATHLIST_MAX_CHARS - 1u)

static JvmanPathListStatus bounded_length(const wchar_t *value,
                                          size_t *length_out) {
    size_t index;
    if (!value || !length_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    for (index = 0; index < JVMAN_PATHLIST_MAX_CHARS; ++index) {
        if (value[index] == L'\0') {
            *length_out = index;
            return JVMAN_PATHLIST_OK;
        }
    }
    return JVMAN_PATHLIST_TOO_LONG;
}

static int allocation_size(size_t chars, size_t *bytes_out) {
    if (!bytes_out || chars > SIZE_MAX / sizeof(wchar_t)) return 0;
    *bytes_out = chars * sizeof(wchar_t);
    return 1;
}

static wchar_t *duplicate_range(const wchar_t *start, size_t length) {
    wchar_t *copy;
    size_t bytes;
    if (!start || !allocation_size(length + 1u, &bytes)) return NULL;
    copy = (wchar_t *)malloc(bytes);
    if (!copy) return NULL;
    if (length != 0) memcpy(copy, start, length * sizeof(wchar_t));
    copy[length] = L'\0';
    return copy;
}

static int is_trim_space(wchar_t value) {
    WORD character_type = 0;
    if (GetStringTypeW(CT_CTYPE1, &value, 1, &character_type)) {
        return (character_type & C1_SPACE) != 0;
    }
    return value == L' ' || value == L'\t' || value == L'\r' ||
           value == L'\n' || value == L'\v' || value == L'\f';
}

static int is_separator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

static void trim_and_unquote(const wchar_t **start_in_out,
                             size_t *length_in_out) {
    const wchar_t *start = *start_in_out;
    size_t length = *length_in_out;
    int unquoted = 0;

    do {
        while (length != 0 && is_trim_space(*start)) {
            ++start;
            --length;
        }
        while (length != 0 && is_trim_space(start[length - 1u])) --length;
        unquoted = length >= 2u && start[0] == L'"' &&
                   start[length - 1u] == L'"';
        if (unquoted) {
            ++start;
            length -= 2u;
        }
    } while (unquoted);

    *start_in_out = start;
    *length_in_out = length;
}

static int has_forbidden_target_character(const wchar_t *value) {
    size_t index;
    for (index = 0; value[index] != L'\0'; ++index) {
        unsigned int character = (unsigned int)value[index];
        if (value[index] == L';' || value[index] == L'"' ||
            character < 0x20u || character == 0x7fu) {
            return 1;
        }
    }
    return 0;
}

static int is_drive_absolute(const wchar_t *value, size_t length) {
    wchar_t drive;
    if (length < 3u || value[1] != L':' || !is_separator(value[2])) return 0;
    drive = value[0];
    return (drive >= L'A' && drive <= L'Z') ||
           (drive >= L'a' && drive <= L'z');
}

static int is_unc_absolute(const wchar_t *value, size_t length) {
    size_t index;
    if (length < 5u || !is_separator(value[0]) ||
        !is_separator(value[1]) || is_separator(value[2])) {
        return 0;
    }
    index = 2u;
    while (index < length && !is_separator(value[index])) ++index;
    if (index == 2u || index == length) return 0;
    while (index < length && is_separator(value[index])) ++index;
    return index < length;
}

static int is_absolute_path(const wchar_t *value, size_t length) {
    return is_drive_absolute(value, length) || is_unc_absolute(value, length);
}

static int is_drive_root(const wchar_t *value, size_t length) {
    return length == 3u && value[1] == L':' && value[2] == L'\\';
}

static JvmanPathListStatus normalize_item(const wchar_t *item,
                                          size_t item_length,
                                          wchar_t **normalized_out) {
    const wchar_t *start = item;
    wchar_t *trimmed = NULL;
    wchar_t *expanded = NULL;
    wchar_t *normalized = NULL;
    DWORD required;
    DWORD written;
    size_t length;
    size_t bytes;
    size_t index;

    if (!item || !normalized_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    *normalized_out = NULL;
    trim_and_unquote(&start, &item_length);
    trimmed = duplicate_range(start, item_length);
    if (!trimmed) return JVMAN_PATHLIST_NO_MEMORY;

    required = ExpandEnvironmentStringsW(trimmed, NULL, 0);
    if (required == 0) {
        free(trimmed);
        return JVMAN_PATHLIST_WIN32_ERROR;
    }
    if ((size_t)required > JVMAN_PATHLIST_MAX_CHARS) {
        free(trimmed);
        return JVMAN_PATHLIST_TOO_LONG;
    }
    if (!allocation_size((size_t)required, &bytes)) {
        free(trimmed);
        return JVMAN_PATHLIST_TOO_LONG;
    }
    expanded = (wchar_t *)malloc(bytes);
    if (!expanded) {
        free(trimmed);
        return JVMAN_PATHLIST_NO_MEMORY;
    }
    written = ExpandEnvironmentStringsW(trimmed, expanded, required);
    free(trimmed);
    if (written == 0) {
        free(expanded);
        return JVMAN_PATHLIST_WIN32_ERROR;
    }
    if (written > required) {
        free(expanded);
        return JVMAN_PATHLIST_TOO_LONG;
    }

    start = expanded;
    length = (size_t)written - 1u;
    trim_and_unquote(&start, &length);
    normalized = duplicate_range(start, length);
    free(expanded);
    if (!normalized) return JVMAN_PATHLIST_NO_MEMORY;

    for (index = 0; index < length; ++index) {
        if (normalized[index] == L'/') normalized[index] = L'\\';
    }
    while (length > 0u && normalized[length - 1u] == L'\\' &&
           !is_drive_root(normalized, length)) {
        normalized[--length] = L'\0';
    }
    *normalized_out = normalized;
    return JVMAN_PATHLIST_OK;
}

static JvmanPathListStatus prepare_target(const wchar_t *target,
                                          wchar_t **normalized_out,
                                          size_t *length_out) {
    JvmanPathListStatus status;
    size_t raw_length;
    size_t normalized_length;
    wchar_t *normalized = NULL;

    if (!target || !normalized_out || !length_out) {
        return JVMAN_PATHLIST_INVALID_ARGUMENT;
    }
    status = bounded_length(target, &raw_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    if (raw_length == 0u || has_forbidden_target_character(target)) {
        return JVMAN_PATHLIST_INVALID_TARGET;
    }
    status = normalize_item(target, raw_length, &normalized);
    if (status != JVMAN_PATHLIST_OK) return status;
    status = bounded_length(normalized, &normalized_length);
    if (status != JVMAN_PATHLIST_OK) {
        free(normalized);
        return status;
    }
    if (normalized_length == 0u ||
        has_forbidden_target_character(normalized) ||
        !is_absolute_path(normalized, normalized_length)) {
        free(normalized);
        return JVMAN_PATHLIST_INVALID_TARGET;
    }
    *normalized_out = normalized;
    *length_out = normalized_length;
    return JVMAN_PATHLIST_OK;
}

static JvmanPathListStatus items_equal(const wchar_t *item,
                                       size_t item_length,
                                       const wchar_t *target,
                                       int *equal_out) {
    wchar_t *normalized = NULL;
    JvmanPathListStatus status;
    int comparison;

    if (!target || !equal_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    *equal_out = 0;
    status = normalize_item(item, item_length, &normalized);
    if (status != JVMAN_PATHLIST_OK) return status;
    if (wcslen(normalized) <= (size_t)INT_MAX &&
        wcslen(target) <= (size_t)INT_MAX) {
        comparison = CompareStringOrdinal(normalized, -1, target, -1, TRUE);
        if (comparison == 0) {
            free(normalized);
            return JVMAN_PATHLIST_WIN32_ERROR;
        }
        *equal_out = comparison == CSTR_EQUAL;
    }
    free(normalized);
    return JVMAN_PATHLIST_OK;
}

static JvmanPathListStatus validate_path_value(const wchar_t *path_value,
                                               size_t *length_out) {
    if (!path_value || !length_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    return bounded_length(path_value, length_out);
}

JvmanPathListStatus jvman_pathlist_contains(const wchar_t *path_value,
                                            const wchar_t *target,
                                            int *contains_out) {
    wchar_t *normalized_target = NULL;
    size_t target_length;
    size_t path_length;
    size_t start;
    size_t end;
    JvmanPathListStatus status;

    if (!contains_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    *contains_out = 0;
    status = validate_path_value(path_value, &path_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    status = prepare_target(target, &normalized_target, &target_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    (void)target_length;

    start = 0u;
    for (;;) {
        int equal;
        end = start;
        while (end < path_length && path_value[end] != L';') ++end;
        status = items_equal(path_value + start, end - start,
                             normalized_target, &equal);
        if (status != JVMAN_PATHLIST_OK) {
            free(normalized_target);
            return status;
        }
        if (equal) {
            *contains_out = 1;
            free(normalized_target);
            return JVMAN_PATHLIST_OK;
        }
        if (end == path_length) break;
        start = end + 1u;
    }
    free(normalized_target);
    return JVMAN_PATHLIST_OK;
}

JvmanPathListStatus jvman_pathlist_add(const wchar_t *path_value,
                                       const wchar_t *target,
                                       wchar_t **result_out,
                                       int *changed_out) {
    wchar_t *normalized_target = NULL;
    wchar_t *result = NULL;
    size_t path_length;
    size_t target_length;
    size_t result_length;
    size_t bytes;
    int contains;
    JvmanPathListStatus status;

    if (!result_out || !changed_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    *result_out = NULL;
    *changed_out = 0;
    status = validate_path_value(path_value, &path_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    status = prepare_target(target, &normalized_target, &target_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    status = jvman_pathlist_contains(path_value, normalized_target, &contains);
    if (status != JVMAN_PATHLIST_OK) {
        free(normalized_target);
        return status;
    }
    if (contains) {
        result = duplicate_range(path_value, path_length);
        free(normalized_target);
        if (!result) return JVMAN_PATHLIST_NO_MEMORY;
        *result_out = result;
        return JVMAN_PATHLIST_OK;
    }

    if (path_length > JVMAN_PATHLIST_MAX_LENGTH - target_length ||
        (path_length != 0u &&
         path_length + target_length >= JVMAN_PATHLIST_MAX_LENGTH)) {
        free(normalized_target);
        return JVMAN_PATHLIST_TOO_LONG;
    }
    result_length = path_length + target_length + (path_length != 0u ? 1u : 0u);
    if (result_length > JVMAN_PATHLIST_MAX_LENGTH ||
        !allocation_size(result_length + 1u, &bytes)) {
        free(normalized_target);
        return JVMAN_PATHLIST_TOO_LONG;
    }
    result = (wchar_t *)malloc(bytes);
    if (!result) {
        free(normalized_target);
        return JVMAN_PATHLIST_NO_MEMORY;
    }
    memcpy(result, normalized_target, target_length * sizeof(wchar_t));
    if (path_length != 0u) {
        result[target_length] = L';';
        memcpy(result + target_length + 1u, path_value,
               path_length * sizeof(wchar_t));
    }
    result[result_length] = L'\0';
    free(normalized_target);
    *result_out = result;
    *changed_out = 1;
    return JVMAN_PATHLIST_OK;
}

JvmanPathListStatus jvman_pathlist_remove(const wchar_t *path_value,
                                          const wchar_t *target,
                                          wchar_t **result_out,
                                          int *changed_out) {
    wchar_t *normalized_target = NULL;
    wchar_t *result = NULL;
    size_t path_length;
    size_t target_length;
    size_t start;
    size_t end;
    size_t output_length = 0u;
    size_t kept_items = 0u;
    size_t bytes;
    JvmanPathListStatus status;

    if (!result_out || !changed_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    *result_out = NULL;
    *changed_out = 0;
    status = validate_path_value(path_value, &path_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    status = prepare_target(target, &normalized_target, &target_length);
    if (status != JVMAN_PATHLIST_OK) return status;
    (void)target_length;
    if (!allocation_size(path_length + 1u, &bytes)) {
        free(normalized_target);
        return JVMAN_PATHLIST_TOO_LONG;
    }
    result = (wchar_t *)malloc(bytes);
    if (!result) {
        free(normalized_target);
        return JVMAN_PATHLIST_NO_MEMORY;
    }

    start = 0u;
    for (;;) {
        int equal;
        size_t item_length;
        end = start;
        while (end < path_length && path_value[end] != L';') ++end;
        item_length = end - start;
        status = items_equal(path_value + start, item_length,
                             normalized_target, &equal);
        if (status != JVMAN_PATHLIST_OK) {
            free(normalized_target);
            free(result);
            return status;
        }
        if (equal) {
            *changed_out = 1;
        } else {
            if (kept_items != 0u) result[output_length++] = L';';
            if (item_length != 0u) {
                memcpy(result + output_length, path_value + start,
                       item_length * sizeof(wchar_t));
                output_length += item_length;
            }
            ++kept_items;
        }
        if (end == path_length) break;
        start = end + 1u;
    }
    result[output_length] = L'\0';
    free(normalized_target);
    *result_out = result;
    return JVMAN_PATHLIST_OK;
}

JvmanPathListStatus jvman_pathlist_prepend(const wchar_t *path_value,
                                           const wchar_t *target,
                                           wchar_t **result_out,
                                           int *changed_out) {
    wchar_t *without_target = NULL;
    wchar_t *result = NULL;
    int removed = 0;
    int added = 0;
    JvmanPathListStatus status;
    if (!result_out || !changed_out) return JVMAN_PATHLIST_INVALID_ARGUMENT;
    *result_out = NULL;
    *changed_out = 0;
    status = jvman_pathlist_remove(path_value, target, &without_target, &removed);
    if (status != JVMAN_PATHLIST_OK) return status;
    status = jvman_pathlist_add(without_target, target, &result, &added);
    free(without_target);
    if (status != JVMAN_PATHLIST_OK) {
        free(result);
        return status;
    }
    (void)removed;
    (void)added;
    *changed_out = wcscmp(path_value, result) != 0;
    *result_out = result;
    return JVMAN_PATHLIST_OK;
}

const wchar_t *jvman_pathlist_status_message(JvmanPathListStatus status) {
    switch (status) {
        case JVMAN_PATHLIST_OK: return L"ok";
        case JVMAN_PATHLIST_INVALID_ARGUMENT: return L"invalid argument";
        case JVMAN_PATHLIST_INVALID_TARGET: return L"invalid target path";
        case JVMAN_PATHLIST_TOO_LONG: return L"PATH value is too long";
        case JVMAN_PATHLIST_NO_MEMORY: return L"out of memory";
        case JVMAN_PATHLIST_WIN32_ERROR: return L"Windows API failure";
        default: return L"unknown path-list error";
    }
}
