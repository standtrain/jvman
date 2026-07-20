#ifndef JVMAN_UTIL_H
#define JVMAN_UTIL_H

#include "common.h"

#include <stddef.h>

int jvman_path_join(char *out, size_t out_size, const char *left, const char *right);
int jvman_path_join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c);
void jvman_strip_trailing_separators(char *path);
int jvman_valid_name(const char *name);
int jvman_name_equal(const char *a, const char *b);
int jvman_parse_major(const char *text, int *major);
int jvman_ends_with(const char *text, const char *suffix);
int jvman_path_equal(const char *a, const char *b);
int jvman_path_is_within(const char *parent, const char *child);
char *jvman_read_all(const char *path, size_t *size_out);
int jvman_json_string_field(const char *json, const char *anchor,
                            const char *field, char *out, size_t out_size);
int jvman_hex_equal(const unsigned char *bytes, size_t byte_count, const char *hex);
void jvman_shell_quote_sh(const char *text, char *out, size_t out_size);
void jvman_shell_quote_powershell(const char *text, char *out, size_t out_size);

#endif
