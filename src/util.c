#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <string.h>
#define JVMAN_STRICMP _stricmp
#else
#include <strings.h>
#define JVMAN_STRICMP strcasecmp
#endif

static int is_separator(char ch) {
#if defined(_WIN32)
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

int jvman_path_join(char *out, size_t out_size, const char *left, const char *right) {
    size_t left_len;
    const char *right_start;
    int needs_separator;

    if (!out || !left || !right || out_size == 0) {
        return -1;
    }
    left_len = strlen(left);
    right_start = right;
    while (*right_start && is_separator(*right_start)) {
        ++right_start;
    }
    needs_separator = left_len > 0 && !is_separator(left[left_len - 1]) && *right_start;
    if (left_len + (size_t)needs_separator + strlen(right_start) + 1 > out_size) {
        return -1;
    }
    memcpy(out, left, left_len);
    if (needs_separator) {
        out[left_len++] = JVMAN_DIR_SEP;
    }
    strcpy(out + left_len, right_start);
    return 0;
}

int jvman_path_join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c) {
    char temp[JVMAN_PATH_MAX];
    if (jvman_path_join(temp, sizeof(temp), a, b) != 0) {
        return -1;
    }
    return jvman_path_join(out, out_size, temp, c);
}

void jvman_strip_trailing_separators(char *path) {
    size_t len;
    if (!path) {
        return;
    }
    len = strlen(path);
    while (len > 1 && is_separator(path[len - 1])) {
#if defined(_WIN32)
        if (len == 3 && path[1] == ':') {
            break;
        }
#endif
        path[--len] = '\0';
    }
}

int jvman_valid_name(const char *name) {
    size_t i;
    size_t len;
    if (!name) {
        return 0;
    }
    len = strlen(name);
    if (len == 0 || len > JVMAN_NAME_MAX || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '+')) {
            return 0;
        }
    }
    return 1;
}

int jvman_name_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
#if defined(_WIN32)
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

int jvman_parse_major(const char *text, int *major) {
    char *end;
    long value;
    if (!text || !*text || !major) {
        return -1;
    }
    value = strtol(text, &end, 10);
    if (*end != '\0' || value < 8 || value > 99) {
        return -1;
    }
    *major = (int)value;
    return 0;
}

int jvman_ends_with(const char *text, const char *suffix) {
    size_t text_len;
    size_t suffix_len;
    if (!text || !suffix) {
        return 0;
    }
    text_len = strlen(text);
    suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

int jvman_path_equal(const char *a, const char *b) {
    size_t a_len;
    size_t b_len;
    size_t i;
    if (!a || !b) {
        return 0;
    }
    a_len = strlen(a);
    b_len = strlen(b);
    while (a_len > 1 && is_separator(a[a_len - 1])) {
        --a_len;
    }
    while (b_len > 1 && is_separator(b[b_len - 1])) {
        --b_len;
    }
    if (a_len != b_len) {
        return 0;
    }
    for (i = 0; i < a_len; ++i) {
        char ac = a[i];
        char bc = b[i];
        if (is_separator(ac) && is_separator(bc)) continue;
#if defined(_WIN32)
        if (tolower((unsigned char)ac) != tolower((unsigned char)bc)) {
            return 0;
        }
#else
        if (ac != bc) return 0;
#endif
    }
    return 1;
}

int jvman_path_is_within(const char *parent, const char *child) {
    size_t parent_len;
    size_t i;
    if (!parent || !child) {
        return 0;
    }
    parent_len = strlen(parent);
    while (parent_len > 1 && is_separator(parent[parent_len - 1])) {
        --parent_len;
    }
    if (strlen(child) <= parent_len) {
        return 0;
    }
    for (i = 0; i < parent_len; ++i) {
        char pc = parent[i];
        char cc = child[i];
        if (is_separator(pc) && is_separator(cc)) continue;
#if defined(_WIN32)
        if (tolower((unsigned char)pc) != tolower((unsigned char)cc)) return 0;
#else
        if (pc != cc) return 0;
#endif
    }
    return is_separator(child[parent_len]);
}

char *jvman_read_all(const char *path, size_t *size_out) {
    FILE *file;
    long size;
    char *data;
    size_t read_size;
    file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    read_size = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(data);
        return NULL;
    }
    data[read_size] = '\0';
    if (size_out) {
        *size_out = read_size;
    }
    return data;
}

static const char *find_json_key(const char *start, const char *field) {
    char needle[128];
    if (snprintf(needle, sizeof(needle), "\"%s\"", field) < 0) {
        return NULL;
    }
    return strstr(start, needle);
}

static const char *find_json_object_end(const char *object_start) {
    const char *cursor = object_start;
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    for (; cursor && *cursor; ++cursor) {
        char ch = *cursor;
        if (in_string) {
            if (escaped) escaped = 0;
            else if (ch == '\\') escaped = 1;
            else if (ch == '"') in_string = 0;
            continue;
        }
        if (ch == '"') in_string = 1;
        else if (ch == '{') ++depth;
        else if (ch == '}' && --depth == 0) return cursor;
    }
    return NULL;
}

int jvman_json_string_field(const char *json, const char *anchor,
                            const char *field, char *out, size_t out_size) {
    const char *cursor;
    const char *key;
    const char *object_end = NULL;
    size_t used = 0;
    if (!json || !field || !out || out_size == 0) {
        return -1;
    }
    cursor = anchor ? strstr(json, anchor) : json;
    if (!cursor) return -1;
    if (anchor) {
        cursor = strchr(cursor + strlen(anchor), '{');
        object_end = cursor ? find_json_object_end(cursor) : NULL;
        if (!cursor || !object_end) return -1;
    }
    key = find_json_key(cursor, field);
    if (!key || (object_end && key >= object_end)) {
        return -1;
    }
    cursor = key + strlen(field) + 2;
    while (*cursor && (!object_end || cursor < object_end) &&
           isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor++ != ':') return -1;
    while (*cursor && (!object_end || cursor < object_end) &&
           isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor++ != '"') return -1;
    while (*cursor && (!object_end || cursor < object_end) && *cursor != '"') {
        char ch = *cursor++;
        if (ch == '\\') {
            ch = *cursor++;
            switch (ch) {
                case '"': case '\\': case '/': break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default: return -1;
            }
        }
        if (used + 1 >= out_size) return -1;
        out[used++] = ch;
    }
    if ((!object_end || cursor < object_end) && *cursor != '"') return -1;
    if (object_end && cursor >= object_end) return -1;
    out[used] = '\0';
    return 0;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int jvman_hex_equal(const unsigned char *bytes, size_t byte_count, const char *hex) {
    size_t i;
    if (!bytes || !hex || strlen(hex) != byte_count * 2) {
        return 0;
    }
    for (i = 0; i < byte_count; ++i) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0 || bytes[i] != (unsigned char)((high << 4) | low)) {
            return 0;
        }
    }
    return 1;
}

void jvman_shell_quote_sh(const char *text, char *out, size_t out_size) {
    size_t used = 0;
    const char *cursor = text;
    if (!out || out_size == 0) return;
    if (used + 1 < out_size) out[used++] = '\'';
    while (cursor && *cursor && used + 1 < out_size) {
        if (*cursor == '\'') {
            const char replacement[] = "'\\''";
            if (used + sizeof(replacement) >= out_size) break;
            memcpy(out + used, replacement, sizeof(replacement) - 1);
            used += sizeof(replacement) - 1;
        } else {
            out[used++] = *cursor;
        }
        ++cursor;
    }
    if (used + 1 < out_size) out[used++] = '\'';
    out[used] = '\0';
}

void jvman_shell_quote_powershell(const char *text, char *out, size_t out_size) {
    size_t used = 0;
    const char *cursor = text;
    if (!out || out_size == 0) return;
    if (used + 1 < out_size) out[used++] = '\'';
    while (cursor && *cursor && used + 1 < out_size) {
        if (*cursor == '\'') {
            if (used + 2 >= out_size) break;
            out[used++] = '\'';
            out[used++] = '\'';
        } else {
            out[used++] = *cursor;
        }
        ++cursor;
    }
    if (used + 1 < out_size) out[used++] = '\'';
    out[used] = '\0';
}
