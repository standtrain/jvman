#include "lang.h"

#include <stdio.h>
#include <wchar.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void test_language_table(JvmanInstallerLang language) {
    int id;
    CHECK(jvman_lang_set(language) == 0);
    for (id = 0; id < JVMAN_STR_COUNT; ++id) {
        const wchar_t *value = jvman_lang_str((JvmanStringId)id);
        CHECK(value != NULL);
        CHECK(value && value[0] != L'\0');
    }
}

static void test_status_tables(JvmanInstallerLang language) {
    int status;
    CHECK(jvman_lang_set(language) == 0);
    for (status = JVMAN_ENV_OK; status <= JVMAN_ENV_UNSUPPORTED; ++status) {
        const wchar_t *value = jvman_lang_environment_status(
            (JvmanEnvironmentStatus)status);
        CHECK(value != NULL);
        CHECK(value && value[0] != L'\0');
    }
    for (status = JVMAN_INSTALL_OK;
         status <= JVMAN_INSTALL_SELF_CLEANUP_REQUIRED; ++status) {
        const wchar_t *value = jvman_lang_install_status(
            (JvmanInstallStatus)status);
        CHECK(value != NULL);
        CHECK(value && value[0] != L'\0');
    }
    CHECK(jvman_lang_environment_status((JvmanEnvironmentStatus)-1)[0] != L'\0');
    CHECK(jvman_lang_install_status((JvmanInstallStatus)-1)[0] != L'\0');
}

static void test_language_names(void) {
    int language;
    for (language = 0; language < JVMAN_LANG_COUNT; ++language) {
        const wchar_t *name = jvman_lang_name((JvmanInstallerLang)language);
        CHECK(name != NULL);
        CHECK(name && name[0] != L'\0');
        CHECK(name && wcscmp(name, L"Unknown") != 0);
    }
}

int main(void) {
    test_language_table(JVMAN_LANG_EN);
    test_language_table(JVMAN_LANG_ZH_CN);
    test_status_tables(JVMAN_LANG_EN);
    test_status_tables(JVMAN_LANG_ZH_CN);
    test_language_names();

    CHECK(wcscmp(jvman_lang_name(JVMAN_LANG_EN), L"English") == 0);
    CHECK(wcscmp(jvman_lang_name(JVMAN_LANG_ZH_CN), L"简体中文") == 0);
    CHECK(wcscmp(jvman_lang_name((JvmanInstallerLang)-1), L"Unknown") == 0);

    CHECK(jvman_lang_set(JVMAN_LANG_EN) == 0);
    CHECK(wcscmp(jvman_lang_str(JVMAN_STR_LANG_SYSTEM),
                 L"System default") == 0);
    CHECK(jvman_lang_set((JvmanInstallerLang)-1) != 0);
    CHECK(wcscmp(jvman_lang_str(JVMAN_STR_APP_TITLE), L"jvman Setup") == 0);
    CHECK(wcsstr(jvman_lang_str(JVMAN_STR_UNINSTALL_CONFIRM_FINAL),
                 L"final confirmation") != NULL);
    CHECK(jvman_lang_set(JVMAN_LANG_ZH_CN) == 0);
    CHECK(wcscmp(jvman_lang_str(JVMAN_STR_LANG_SYSTEM), L"跟随系统") == 0);
    CHECK(wcsstr(jvman_lang_str(JVMAN_STR_UNINSTALL_CONFIRM_FINAL),
                 L"再次确认") != NULL);
    CHECK(jvman_lang_str((JvmanStringId)-1)[0] == L'\0');
    CHECK(jvman_lang_str(JVMAN_STR_COUNT)[0] == L'\0');
    jvman_lang_use_system_default();
    CHECK(wcscmp(jvman_lang_str(JVMAN_STR_APP_TITLE), L"jvman Setup") == 0 ||
          wcscmp(jvman_lang_str(JVMAN_STR_APP_TITLE),
                 L"jvman 安装程序") == 0);

    if (failures != 0) {
        fprintf(stderr, "%d language test(s) failed\n", failures);
        return 1;
    }
    puts("All language unit tests passed.");
    return 0;
}
