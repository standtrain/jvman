#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "../installer/pathlist.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fwprintf(stderr, L"FAIL %S:%d: %S\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static void check_add(const wchar_t *before, const wchar_t *target,
                      const wchar_t *expected, int expected_changed) {
    wchar_t *result = NULL;
    int changed = -1;
    CHECK(jvman_pathlist_add(before, target, &result, &changed) ==
          JVMAN_PATHLIST_OK);
    CHECK(result != NULL);
    if (result) CHECK(wcscmp(result, expected) == 0);
    CHECK(changed == expected_changed);
    free(result);
}

static void check_remove(const wchar_t *before, const wchar_t *target,
                         const wchar_t *expected, int expected_changed) {
    wchar_t *result = NULL;
    int changed = -1;
    CHECK(jvman_pathlist_remove(before, target, &result, &changed) ==
          JVMAN_PATHLIST_OK);
    CHECK(result != NULL);
    if (result) CHECK(wcscmp(result, expected) == 0);
    CHECK(changed == expected_changed);
    free(result);
}

static void check_contains(const wchar_t *path_value, const wchar_t *target,
                           int expected) {
    int found = -1;
    CHECK(jvman_pathlist_contains(path_value, target, &found) ==
          JVMAN_PATHLIST_OK);
    CHECK(found == expected);
}

static void test_add_and_contains(void) {
    check_add(L"", L"C:\\jvman\\bin", L"C:\\jvman\\bin", 1);
    check_add(L"C:\\Windows", L"C:\\jvman\\bin",
              L"C:\\Windows;C:\\jvman\\bin", 1);
    check_contains(L"C:\\jvman\\bin;C:\\Windows", L"C:\\jvman\\bin", 1);
    check_contains(L"C:\\Windows;C:\\jvman\\bin;C:\\Tools",
                   L"C:\\jvman\\bin", 1);
    check_contains(L"C:\\Windows;C:\\jvman\\bin", L"C:\\jvman\\bin", 1);
    check_contains(L"C:\\jvman\\binary", L"C:\\jvman\\bin", 0);

    check_add(L"C:/JVMAN/BIN/", L"c:\\jvman\\bin",
              L"C:/JVMAN/BIN/", 0);
    check_add(L"  \"C:\\Program Files\\jvman\\bin\\\"  ",
              L"C:\\Program Files\\jvman\\bin",
              L"  \"C:\\Program Files\\jvman\\bin\\\"  ", 0);
    check_add(L"C:\\Windows;", L"C:\\jvman\\bin",
              L"C:\\Windows;;C:\\jvman\\bin", 1);
    check_add(L"  \\\\server/share\\jvman\\bin\\  ",
              L"\\\\SERVER\\SHARE\\JVMAN\\BIN",
              L"  \\\\server/share\\jvman\\bin\\  ", 0);
    check_contains(L"\u3000C:\\jvman\\bin\u3000", L"C:\\JVMAN\\BIN", 1);
}

static void test_remove(void) {
    check_remove(L"C:\\jvman\\bin;C:\\Windows", L"C:\\jvman\\bin",
                 L"C:\\Windows", 1);
    check_remove(L"C:\\Windows; \"c:/JVMAN/bin/\" ;C:\\Tools",
                 L"C:\\jvman\\bin", L"C:\\Windows;C:\\Tools", 1);
    check_remove(L"C:\\Windows;C:\\jvman\\bin", L"C:\\jvman\\bin",
                 L"C:\\Windows", 1);
    check_remove(L"C:\\jvman\\bin;C:\\JVMAN\\BIN\\;C:\\Tools",
                 L"C:\\jvman\\bin", L"C:\\Tools", 1);
    check_remove(L"C:\\jvman\\binary;C:\\Tools", L"C:\\jvman\\bin",
                 L"C:\\jvman\\binary;C:\\Tools", 0);
    check_remove(L"A;;C:\\jvman\\bin;B", L"C:\\jvman\\bin",
                 L"A;;B", 1);
}

static void test_environment_and_unicode(void) {
    wchar_t old_value[32767];
    DWORD old_length;
    int had_old;

    SetLastError(ERROR_SUCCESS);
    old_length = GetEnvironmentVariableW(L"JVMAN_PATHLIST_TEST_ROOT",
                                          old_value, 32767);
    had_old = old_length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    CHECK(SetEnvironmentVariableW(L"JVMAN_PATHLIST_TEST_ROOT",
                                  L"C:\\工具\\JDK") != 0);
    check_contains(L"%JVMAN_PATHLIST_TEST_ROOT%\\bin",
                   L"c:/工具/jdk/bin/", 1);
    check_add(L"%JVMAN_PATHLIST_TEST_ROOT%\\bin", L"C:\\工具\\JDK\\bin",
              L"%JVMAN_PATHLIST_TEST_ROOT%\\bin", 0);
    check_remove(L"C:\\Windows;%JVMAN_PATHLIST_TEST_ROOT%\\bin;C:\\Tools",
                 L"C:\\工具\\JDK\\bin", L"C:\\Windows;C:\\Tools", 1);
    check_add(L"", L"C:/工具/JDK/bin/", L"C:\\工具\\JDK\\bin", 1);

    if (had_old) {
        CHECK(SetEnvironmentVariableW(L"JVMAN_PATHLIST_TEST_ROOT", old_value) != 0);
    } else {
        CHECK(SetEnvironmentVariableW(L"JVMAN_PATHLIST_TEST_ROOT", NULL) != 0);
    }
}

static void test_invalid_targets(void) {
    static const wchar_t *invalid[] = {
        L"", L"   ", L"relative\\bin", L"C:relative\\bin",
        L"\\root-relative", L"C:\\good;C:\\bad", L"C:\\bad\nname",
        L"\"C:\\quoted\\bin\"", L"C:\\quoted\"bin"
    };
    size_t index;
    wchar_t *result = (wchar_t *)1;
    int changed = 1;
    int contains = 1;

    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        result = (wchar_t *)1;
        changed = 1;
        CHECK(jvman_pathlist_add(L"C:\\Windows", invalid[index],
                                 &result, &changed) ==
              JVMAN_PATHLIST_INVALID_TARGET);
        CHECK(result == NULL);
        CHECK(changed == 0);
    }
    CHECK(jvman_pathlist_add(NULL, L"C:\\jvman", &result, &changed) ==
          JVMAN_PATHLIST_INVALID_ARGUMENT);
    CHECK(jvman_pathlist_add(L"", L"C:\\jvman", NULL, &changed) ==
          JVMAN_PATHLIST_INVALID_ARGUMENT);
    CHECK(jvman_pathlist_remove(L"", L"C:\\jvman", &result, NULL) ==
          JVMAN_PATHLIST_INVALID_ARGUMENT);
    CHECK(jvman_pathlist_contains(L"", L"C:\\jvman", NULL) ==
          JVMAN_PATHLIST_INVALID_ARGUMENT);
    CHECK(jvman_pathlist_contains(NULL, L"C:\\jvman", &contains) ==
          JVMAN_PATHLIST_INVALID_ARGUMENT);
}

static void test_length_limit(void) {
    const size_t maximum_length = JVMAN_PATHLIST_MAX_CHARS - 1u;
    wchar_t *maximum = (wchar_t *)malloc(JVMAN_PATHLIST_MAX_CHARS *
                                         sizeof(wchar_t));
    wchar_t *too_long = (wchar_t *)malloc((JVMAN_PATHLIST_MAX_CHARS + 1u) *
                                          sizeof(wchar_t));
    wchar_t *result = NULL;
    int contains = -1;
    int changed = -1;

    CHECK(maximum != NULL);
    CHECK(too_long != NULL);
    if (!maximum || !too_long) {
        free(maximum);
        free(too_long);
        return;
    }
    wmemset(maximum, L'x', maximum_length);
    maximum[maximum_length] = L'\0';
    wmemset(too_long, L'x', JVMAN_PATHLIST_MAX_CHARS);
    too_long[JVMAN_PATHLIST_MAX_CHARS] = L'\0';

    CHECK(jvman_pathlist_contains(too_long, L"C:\\jvman", &contains) ==
          JVMAN_PATHLIST_TOO_LONG);
    CHECK(jvman_pathlist_add(maximum, L"C:\\jvman", &result, &changed) ==
          JVMAN_PATHLIST_TOO_LONG);
    CHECK(result == NULL);
    CHECK(changed == 0);
    free(maximum);
    free(too_long);
}

int wmain(void) {
    test_add_and_contains();
    test_remove();
    test_environment_and_unicode();
    test_invalid_targets();
    test_length_limit();
    if (failures != 0) {
        fwprintf(stderr, L"%d installer PATH test(s) failed\n", failures);
        return 1;
    }
    fputws(L"All installer PATH tests passed.\n", stdout);
    return 0;
}
