#include "download_source.h"

#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JVMAN_SOURCE_URL_MAX 2048u
#define JVMAN_SOURCE_VERSION_MAX 128u
#define JVMAN_JSON_KEY_MAX 64u
#define JVMAN_JSON_DEPTH_MAX 64u

static const JvmanDownloadSource download_sources[] = {
    {"adoptium", "Adoptium API", JVMAN_DOWNLOAD_SOURCE_ADOPTIUM},
    {"foojay", "Foojay Disco API", JVMAN_DOWNLOAD_SOURCE_FOOJAY}
};

const JvmanDownloadSource *jvman_download_source_default(void) {
    return &download_sources[0];
}

const JvmanDownloadSource *jvman_download_source_find(const char *name) {
    size_t i;
    if (!name || !*name) return NULL;
    for (i = 0; i < sizeof(download_sources) / sizeof(download_sources[0]); ++i) {
        if (strcmp(name, download_sources[i].name) == 0) return &download_sources[i];
    }
    return NULL;
}

size_t jvman_download_source_count(void) {
    return sizeof(download_sources) / sizeof(download_sources[0]);
}

const JvmanDownloadSource *jvman_download_source_at(size_t index) {
    return index < jvman_download_source_count() ? &download_sources[index] : NULL;
}

static const char *foojay_os(const char *os) {
    if (strcmp(os, "mac") == 0) return "macos";
    if (strcmp(os, "alpine-linux") == 0) return "alpine_linux";
    return os;
}

static const char *foojay_architecture(const char *architecture) {
    if (strcmp(architecture, "x32") == 0) return "x86";
    if (strcmp(architecture, "arm") == 0) return "arm32";
    return architecture;
}

int jvman_download_source_build_metadata_url(
    const JvmanDownloadSource *source, int major, const char *os,
    const char *architecture, const char *archive_extension,
    char *out, size_t out_size) {
    int written;
    if (!source || major < 8 || major > 999 || !os || !*os ||
        !architecture || !*architecture || !archive_extension ||
        !*archive_extension || !out || out_size == 0) {
        return -1;
    }
    if (source->kind == JVMAN_DOWNLOAD_SOURCE_ADOPTIUM) {
        written = snprintf(
            out, out_size,
            "https://api.adoptium.net/v3/assets/latest/%d/hotspot?"
            "architecture=%s&heap_size=normal&image_type=jdk&jvm_impl=hotspot&"
            "os=%s&page=0&page_size=1&project=jdk&sort_method=DEFAULT&"
            "sort_order=DESC&vendor=eclipse",
            major, architecture, os);
    } else if (source->kind == JVMAN_DOWNLOAD_SOURCE_FOOJAY) {
        const char *archive_type = archive_extension[0] == '.'
                                       ? archive_extension + 1
                                       : archive_extension;
        written = snprintf(
            out, out_size,
            "https://api.foojay.io/disco/v3.0/packages?version=%d&"
            "distribution=temurin&architecture=%s&archive_type=%s&"
            "package_type=jdk&operating_system=%s&release_status=ga&"
            "latest=overall&directly_downloadable=true",
            major, foojay_architecture(architecture), archive_type,
            foojay_os(os));
    } else {
        return -1;
    }
    return written >= 0 && (size_t)written < out_size ? 0 : -1;
}

static int valid_sha256(const char *text) {
    size_t i;
    if (!text || strlen(text) != 64) return 0;
    for (i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static int valid_https_url(const char *url) {
    const unsigned char *cursor = (const unsigned char *)url;
    size_t length = 0;
    if (!url) return 0;
    while (length < JVMAN_SOURCE_URL_MAX && url[length]) ++length;
    if (length < 9 || length >= JVMAN_SOURCE_URL_MAX ||
        strncmp(url, "https://", 8) != 0) {
        return 0;
    }
    for (; *cursor; ++cursor) {
        if (*cursor <= 0x20 || *cursor == 0x7f) return 0;
    }
    return 1;
}

static int valid_version_for_major(const char *version, int major) {
    const char *cursor;
    char *end;
    long resolved;
    if (!version || !*version || strlen(version) >= 128) return 0;
    for (cursor = version; *cursor; ++cursor) {
        if (!isalnum((unsigned char)*cursor) && *cursor != '.' &&
            *cursor != '_' && *cursor != '+' && *cursor != '-') {
            return 0;
        }
    }
    if (major == 8 && strncmp(version, "1.8.", 4) == 0) return 1;
    resolved = strtol(version, &end, 10);
    return end != version && resolved == major &&
           (*end == '\0' || *end == '.' || *end == '_' ||
            *end == '+' || *end == '-');
}

static void json_skip_whitespace(const char **cursor) {
    while (isspace((unsigned char)**cursor)) ++*cursor;
}

static int json_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Parses one JSON string and always consumes it, even when out is too small. */
static int json_read_string(const char **cursor, char *out, size_t out_size,
                            int *output_fits) {
    const char *current = *cursor;
    size_t used = 0;
    int fits = out == NULL || out_size > 0;
    if (*current++ != '"') return -1;
    while (*current && *current != '"') {
        unsigned int decoded = (unsigned char)*current++;
        if (decoded < 0x20) return -1;
        if (decoded == '\\') {
            char escaped = *current++;
            if (!escaped) return -1;
            switch (escaped) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    int index;
                    decoded = 0;
                    for (index = 0; index < 4; ++index) {
                        int digit;
                        if (!*current || (digit = json_hex_digit(*current)) < 0) {
                            return -1;
                        }
                        decoded = (decoded << 4) | (unsigned int)digit;
                        ++current;
                    }
                    break;
                }
                default: return -1;
            }
        }
        if (out) {
            if (decoded == 0 || decoded > 0x7f || used + 1 >= out_size) {
                fits = 0;
            } else if (fits) {
                out[used++] = (char)decoded;
            }
        }
    }
    if (*current++ != '"') return -1;
    if (out) {
        if (fits) out[used] = '\0';
        else if (out_size) out[0] = '\0';
    }
    if (output_fits) *output_fits = fits;
    *cursor = current;
    return 0;
}

static int json_skip_value_depth(const char **cursor, unsigned int depth);

static int json_skip_array(const char **cursor, unsigned int depth) {
    const char *current = *cursor;
    if (*current++ != '[') return -1;
    json_skip_whitespace(&current);
    if (*current == ']') {
        *cursor = current + 1;
        return 0;
    }
    for (;;) {
        if (json_skip_value_depth(&current, depth + 1) != 0) return -1;
        json_skip_whitespace(&current);
        if (*current == ']') {
            *cursor = current + 1;
            return 0;
        }
        if (*current++ != ',') return -1;
        json_skip_whitespace(&current);
    }
}

static int json_skip_object(const char **cursor, unsigned int depth) {
    const char *current = *cursor;
    if (*current++ != '{') return -1;
    json_skip_whitespace(&current);
    if (*current == '}') {
        *cursor = current + 1;
        return 0;
    }
    for (;;) {
        if (json_read_string(&current, NULL, 0, NULL) != 0) return -1;
        json_skip_whitespace(&current);
        if (*current++ != ':') return -1;
        if (json_skip_value_depth(&current, depth + 1) != 0) return -1;
        json_skip_whitespace(&current);
        if (*current == '}') {
            *cursor = current + 1;
            return 0;
        }
        if (*current++ != ',') return -1;
        json_skip_whitespace(&current);
    }
}

static int json_skip_number(const char **cursor) {
    const char *current = *cursor;
    if (*current == '-') ++current;
    if (*current == '0') {
        ++current;
    } else {
        if (!isdigit((unsigned char)*current)) return -1;
        while (isdigit((unsigned char)*current)) ++current;
    }
    if (*current == '.') {
        ++current;
        if (!isdigit((unsigned char)*current)) return -1;
        while (isdigit((unsigned char)*current)) ++current;
    }
    if (*current == 'e' || *current == 'E') {
        ++current;
        if (*current == '+' || *current == '-') ++current;
        if (!isdigit((unsigned char)*current)) return -1;
        while (isdigit((unsigned char)*current)) ++current;
    }
    *cursor = current;
    return 0;
}

static int json_skip_value_depth(const char **cursor, unsigned int depth) {
    const char *current = *cursor;
    if (depth > JVMAN_JSON_DEPTH_MAX) return -1;
    json_skip_whitespace(&current);
    if (*current == '"') {
        if (json_read_string(&current, NULL, 0, NULL) != 0) return -1;
    } else if (*current == '{') {
        if (json_skip_object(&current, depth) != 0) return -1;
    } else if (*current == '[') {
        if (json_skip_array(&current, depth) != 0) return -1;
    } else if (strncmp(current, "true", 4) == 0) {
        current += 4;
    } else if (strncmp(current, "false", 5) == 0) {
        current += 5;
    } else if (strncmp(current, "null", 4) == 0) {
        current += 4;
    } else if (*current == '-' || isdigit((unsigned char)*current)) {
        if (json_skip_number(&current) != 0) return -1;
    } else {
        return -1;
    }
    *cursor = current;
    return 0;
}

static int foojay_find_result_array(const char *json,
                                    const char **result_array) {
    const char *current = json;
    const char *found = NULL;
    json_skip_whitespace(&current);
    if (*current++ != '{') return -1;
    json_skip_whitespace(&current);
    if (*current != '}') {
        for (;;) {
            char key[JVMAN_JSON_KEY_MAX];
            int key_fits;
            if (json_read_string(&current, key, sizeof(key), &key_fits) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current++ != ':') return -1;
            json_skip_whitespace(&current);
            if (key_fits && strcmp(key, "result") == 0) {
                if (found || *current != '[') return -1;
                found = current;
            }
            if (json_skip_value_depth(&current, 1) != 0) return -1;
            json_skip_whitespace(&current);
            if (*current == '}') break;
            if (*current++ != ',') return -1;
            json_skip_whitespace(&current);
        }
    }
    ++current;
    json_skip_whitespace(&current);
    if (*current || !found) return -1;
    *result_array = found;
    return 0;
}

static int json_copy_string(char *destination, size_t destination_size,
                            const char *source) {
    size_t length = strlen(source);
    if (length >= destination_size) return -1;
    memcpy(destination, source, length + 1);
    return 0;
}

/* Returns zero for one valid links object, one for a non-matching object. */
static int foojay_parse_links(const char **cursor,
                              char pkg_info_uri[JVMAN_SOURCE_URL_MAX]) {
    const char *current = *cursor;
    int invalid = 0;
    int seen_uri = 0;
    if (*current++ != '{') return -1;
    json_skip_whitespace(&current);
    if (*current != '}') {
        for (;;) {
            char key[JVMAN_JSON_KEY_MAX];
            int key_fits;
            if (json_read_string(&current, key, sizeof(key), &key_fits) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current++ != ':') return -1;
            json_skip_whitespace(&current);
            if (key_fits && strcmp(key, "pkg_info_uri") == 0) {
                int value_fits = 0;
                if (seen_uri) invalid = 1;
                seen_uri = 1;
                if (*current == '"') {
                    if (json_read_string(&current, pkg_info_uri,
                                         JVMAN_SOURCE_URL_MAX,
                                         &value_fits) != 0) {
                        return -1;
                    }
                    if (!value_fits || !valid_https_url(pkg_info_uri)) invalid = 1;
                } else {
                    invalid = 1;
                    if (json_skip_value_depth(&current, 3) != 0) return -1;
                }
            } else if (json_skip_value_depth(&current, 3) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current == '}') break;
            if (*current++ != ',') return -1;
            json_skip_whitespace(&current);
        }
    }
    *cursor = current + 1;
    return seen_uri && !invalid ? 0 : 1;
}

/* Returns zero for a valid package, one for an unrelated/invalid package. */
static int foojay_parse_catalog_item(
    const char **cursor, int major,
    char pkg_info_uri[JVMAN_SOURCE_URL_MAX],
    char java_version[JVMAN_SOURCE_VERSION_MAX]) {
    const char *current = *cursor;
    int invalid = 0;
    int seen_links = 0;
    int seen_version = 0;
    if (*current++ != '{') return -1;
    json_skip_whitespace(&current);
    if (*current != '}') {
        for (;;) {
            char key[JVMAN_JSON_KEY_MAX];
            int key_fits;
            if (json_read_string(&current, key, sizeof(key), &key_fits) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current++ != ':') return -1;
            json_skip_whitespace(&current);
            if (key_fits && strcmp(key, "java_version") == 0) {
                int value_fits = 0;
                if (seen_version) invalid = 1;
                seen_version = 1;
                if (*current == '"') {
                    if (json_read_string(&current, java_version,
                                         JVMAN_SOURCE_VERSION_MAX,
                                         &value_fits) != 0) {
                        return -1;
                    }
                    if (!value_fits ||
                        !valid_version_for_major(java_version, major)) {
                        invalid = 1;
                    }
                } else {
                    invalid = 1;
                    if (json_skip_value_depth(&current, 2) != 0) return -1;
                }
            } else if (key_fits && strcmp(key, "links") == 0) {
                int links_result;
                if (seen_links) invalid = 1;
                seen_links = 1;
                if (*current == '{') {
                    links_result = foojay_parse_links(&current, pkg_info_uri);
                    if (links_result < 0) return -1;
                    if (links_result != 0) invalid = 1;
                } else {
                    invalid = 1;
                    if (json_skip_value_depth(&current, 2) != 0) return -1;
                }
            } else if (json_skip_value_depth(&current, 2) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current == '}') break;
            if (*current++ != ',') return -1;
            json_skip_whitespace(&current);
        }
    }
    *cursor = current + 1;
    return seen_links && seen_version && !invalid ? 0 : 1;
}

static int foojay_parse_catalog_result(
    const char *array, int major,
    char pkg_info_uri[JVMAN_SOURCE_URL_MAX],
    char java_version[JVMAN_SOURCE_VERSION_MAX]) {
    const char *current = array;
    int found = 0;
    if (*current++ != '[') return -1;
    json_skip_whitespace(&current);
    if (*current == ']') return 1;
    for (;;) {
        if (*current == '{') {
            char candidate_uri[JVMAN_SOURCE_URL_MAX] = {0};
            char candidate_version[JVMAN_SOURCE_VERSION_MAX] = {0};
            int item_result = foojay_parse_catalog_item(
                &current, major, candidate_uri, candidate_version);
            if (item_result < 0) return -1;
            if (!found && item_result == 0) {
                memcpy(pkg_info_uri, candidate_uri, sizeof(candidate_uri));
                memcpy(java_version, candidate_version,
                       sizeof(candidate_version));
                found = 1;
            }
        } else if (json_skip_value_depth(&current, 2) != 0) {
            return -1;
        }
        json_skip_whitespace(&current);
        if (*current == ']') return found ? 0 : 1;
        if (*current++ != ',') return -1;
        json_skip_whitespace(&current);
    }
}

static int foojay_read_direct_string(const char **cursor, char *out,
                                     size_t out_size, int *seen,
                                     int *invalid) {
    int value_fits = 0;
    if (*seen) *invalid = 1;
    *seen = 1;
    if (**cursor != '"') {
        *invalid = 1;
        return json_skip_value_depth(cursor, 2);
    }
    if (json_read_string(cursor, out, out_size, &value_fits) != 0) return -1;
    if (!value_fits) *invalid = 1;
    return 0;
}

static int foojay_parse_detail_item(
    const char **cursor, char download_uri[JVMAN_SOURCE_URL_MAX],
    char checksum[65]) {
    const char *current = *cursor;
    char checksum_type[16] = {0};
    int invalid = 0;
    int seen_uri = 0;
    int seen_checksum = 0;
    int seen_type = 0;
    if (*current++ != '{') return -1;
    json_skip_whitespace(&current);
    if (*current != '}') {
        for (;;) {
            char key[JVMAN_JSON_KEY_MAX];
            int key_fits;
            if (json_read_string(&current, key, sizeof(key), &key_fits) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current++ != ':') return -1;
            json_skip_whitespace(&current);
            if (key_fits && strcmp(key, "direct_download_uri") == 0) {
                if (foojay_read_direct_string(&current, download_uri,
                                              JVMAN_SOURCE_URL_MAX, &seen_uri,
                                              &invalid) != 0) {
                    return -1;
                }
            } else if (key_fits && strcmp(key, "checksum") == 0) {
                if (foojay_read_direct_string(&current, checksum, 65,
                                              &seen_checksum, &invalid) != 0) {
                    return -1;
                }
            } else if (key_fits && strcmp(key, "checksum_type") == 0) {
                if (foojay_read_direct_string(&current, checksum_type,
                                              sizeof(checksum_type), &seen_type,
                                              &invalid) != 0) {
                    return -1;
                }
            } else if (json_skip_value_depth(&current, 2) != 0) {
                return -1;
            }
            json_skip_whitespace(&current);
            if (*current == '}') break;
            if (*current++ != ',') return -1;
            json_skip_whitespace(&current);
        }
    }
    *cursor = current + 1;
    if (!seen_uri || !seen_checksum || !seen_type || invalid ||
        strcmp(checksum_type, "sha256") != 0 ||
        !valid_https_url(download_uri) || !valid_sha256(checksum)) {
        return 1;
    }
    return 0;
}

static int foojay_parse_detail_result(
    const char *array, char download_uri[JVMAN_SOURCE_URL_MAX],
    char checksum[65]) {
    const char *current = array;
    int found = 0;
    if (*current++ != '[') return -1;
    json_skip_whitespace(&current);
    if (*current == ']') return 1;
    for (;;) {
        if (*current == '{') {
            char candidate_uri[JVMAN_SOURCE_URL_MAX] = {0};
            char candidate_checksum[65] = {0};
            int item_result = foojay_parse_detail_item(
                &current, candidate_uri, candidate_checksum);
            if (item_result < 0) return -1;
            if (!found && item_result == 0) {
                memcpy(download_uri, candidate_uri, sizeof(candidate_uri));
                memcpy(checksum, candidate_checksum, sizeof(candidate_checksum));
                found = 1;
            }
        } else if (json_skip_value_depth(&current, 2) != 0) {
            return -1;
        }
        json_skip_whitespace(&current);
        if (*current == ']') return found ? 0 : 1;
        if (*current++ != ',') return -1;
        json_skip_whitespace(&current);
    }
}

int jvman_download_source_parse_catalog(
    const JvmanDownloadSource *source, const char *json, int major,
    char *detail_url, size_t detail_url_size,
    char *exact_version, size_t version_size) {
    if (!source || !json || !detail_url || detail_url_size == 0 ||
        !exact_version || version_size == 0) {
        return -1;
    }
    detail_url[0] = '\0';
    exact_version[0] = '\0';
    if (source->kind == JVMAN_DOWNLOAD_SOURCE_ADOPTIUM) {
        if (jvman_json_string_field(json, "\"version\"", "openjdk_version",
                                    exact_version, version_size) != 0 ||
            !valid_version_for_major(exact_version, major)) {
            exact_version[0] = '\0';
            return -1;
        }
        return 0;
    } else if (source->kind == JVMAN_DOWNLOAD_SOURCE_FOOJAY) {
        const char *result_array;
        char parsed_url[JVMAN_SOURCE_URL_MAX] = {0};
        char parsed_version[JVMAN_SOURCE_VERSION_MAX] = {0};
        if (major < 8 || major > 999 ||
            foojay_find_result_array(json, &result_array) != 0 ||
            foojay_parse_catalog_result(result_array, major, parsed_url,
                                        parsed_version) != 0 ||
            json_copy_string(detail_url, detail_url_size, parsed_url) != 0 ||
            json_copy_string(exact_version, version_size, parsed_version) != 0) {
            detail_url[0] = '\0';
            exact_version[0] = '\0';
            return -1;
        }
        return 0;
    } else {
        return -1;
    }
}

int jvman_download_source_parse_package(
    const JvmanDownloadSource *source, const char *json,
    char *download_url, size_t url_size,
    char *checksum, size_t checksum_size) {
    if (!source || !json || !download_url || url_size == 0 ||
        !checksum || checksum_size < 65) {
        return -1;
    }
    download_url[0] = '\0';
    checksum[0] = '\0';
    if (source->kind == JVMAN_DOWNLOAD_SOURCE_ADOPTIUM) {
        if (jvman_json_string_field(json, "\"package\"", "link",
                                    download_url, url_size) != 0 ||
            jvman_json_string_field(json, "\"package\"", "checksum",
                                    checksum, checksum_size) != 0 ||
            !valid_https_url(download_url) || !valid_sha256(checksum)) {
            download_url[0] = '\0';
            checksum[0] = '\0';
            return -1;
        }
        return 0;
    } else if (source->kind == JVMAN_DOWNLOAD_SOURCE_FOOJAY) {
        const char *result_array;
        char parsed_url[JVMAN_SOURCE_URL_MAX] = {0};
        char parsed_checksum[65] = {0};
        if (foojay_find_result_array(json, &result_array) != 0 ||
            foojay_parse_detail_result(result_array, parsed_url,
                                       parsed_checksum) != 0 ||
            json_copy_string(download_url, url_size, parsed_url) != 0 ||
            json_copy_string(checksum, checksum_size, parsed_checksum) != 0) {
            download_url[0] = '\0';
            checksum[0] = '\0';
            return -1;
        }
        return 0;
    } else {
        return -1;
    }
}
