#ifndef JVMAN_INSTALLER_PATHLIST_H
#define JVMAN_INSTALLER_PATHLIST_H

#include <stddef.h>
#include <wchar.h>

/* Windows limits an environment variable value to 32767 WCHARs, including NUL. */
#define JVMAN_PATHLIST_MAX_CHARS ((size_t)32767)

typedef enum JvmanPathListStatus {
    JVMAN_PATHLIST_OK = 0,
    JVMAN_PATHLIST_INVALID_ARGUMENT,
    JVMAN_PATHLIST_INVALID_TARGET,
    JVMAN_PATHLIST_TOO_LONG,
    JVMAN_PATHLIST_NO_MEMORY,
    JVMAN_PATHLIST_WIN32_ERROR
} JvmanPathListStatus;

/*
 * PATH values and targets are UTF-16. Comparison trims each item, removes a
 * pair of surrounding quotes, expands environment variables, normalizes slash
 * direction and trailing slashes, and uses ordinal case-insensitive matching.
 * Targets must be unquoted absolute paths; semicolons and control characters
 * are rejected before environment expansion and after normalization.
 */
JvmanPathListStatus jvman_pathlist_contains(const wchar_t *path_value,
                                            const wchar_t *target,
                                            int *contains_out);

/*
 * add/remove always return a malloc-allocated string in result_out on success;
 * the caller owns it and must release it with free().
 */
JvmanPathListStatus jvman_pathlist_add(const wchar_t *path_value,
                                       const wchar_t *target,
                                       wchar_t **result_out,
                                       int *changed_out);
JvmanPathListStatus jvman_pathlist_remove(const wchar_t *path_value,
                                          const wchar_t *target,
                                          wchar_t **result_out,
                                          int *changed_out);

const wchar_t *jvman_pathlist_status_message(JvmanPathListStatus status);

#endif
