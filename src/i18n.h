#ifndef JVMAN_I18N_H
#define JVMAN_I18N_H

#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define JVMAN_PRINTF_FORMAT(format_index, arguments_index) \
    __attribute__((format(printf, format_index, arguments_index)))
#else
#define JVMAN_PRINTF_FORMAT(format_index, arguments_index)
#endif

typedef enum JvmanLanguage {
    JVMAN_LANGUAGE_EN = 0,
    JVMAN_LANGUAGE_ZH_CN = 1
} JvmanLanguage;

/* JVMAN_LANG overrides the installer metadata for the current process. */
void jvman_i18n_init(void);
JvmanLanguage jvman_i18n_language(void);
const char *jvman_i18n_language_tag(void);
int jvman_i18n_parse_language(const char *value, JvmanLanguage *language_out);

/* Persists the current user's choice. Returns -2 on unsupported platforms. */
int jvman_i18n_set_persistent(JvmanLanguage language);

const char *jvman_i18n_text(const char *english);
const char *jvman_i18n_plural_suffix(unsigned long count);

/* These formatting helpers must only receive application-owned format text. */
int jvman_i18n_printf(const char *trusted_format, ...)
    JVMAN_PRINTF_FORMAT(1, 2);
int jvman_i18n_fprintf(FILE *stream, const char *trusted_format, ...)
    JVMAN_PRINTF_FORMAT(2, 3);
int jvman_i18n_puts(const char *text);

#undef JVMAN_PRINTF_FORMAT

#endif
