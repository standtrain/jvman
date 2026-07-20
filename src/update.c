#include "update.h"

#include "common.h"
#include "platform.h"
#include "sha256.h"
#include "util.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JVMAN_UPDATE_JSON_LIMIT (1024u * 1024u)
#define JVMAN_UPDATE_CHECKSUM_LIMIT (64u * 1024u)
#define JVMAN_UPDATE_BINARY_LIMIT (64u * 1024u * 1024u)
#define JVMAN_UPDATE_URL_MAX 1024u

static const char update_latest_url[] =
    "https://api.github.com/repos/standtrain/jvman/releases/latest";
static const char update_version_marker[] =
    "\x01JVMAN-SELF-VERSION:" JVMAN_VERSION;

typedef struct JvmanJsonCursor {
    const char *cursor;
    const char *end;
    unsigned int depth;
} JvmanJsonCursor;

static int update_error(const char *message) {
    fprintf(stderr, "jvman: %s\n", message);
    return 1;
}

static int update_platform_error(const char *message) {
    fprintf(stderr, "jvman: %s: %s\n", message, platform_last_error());
    return 1;
}

static void update_digest_to_hex(const unsigned char digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < 32u; ++index) {
        out[index * 2u] = hex[digest[index] >> 4];
        out[index * 2u + 1u] = hex[digest[index] & 0x0fu];
    }
    out[64] = '\0';
}

static int update_parse_component(const char **cursor, uint32_t *value_out) {
    const char *start;
    uint32_t value = 0;
    if (!cursor || !*cursor || !value_out) return -1;
    start = *cursor;
    if (*start < '0' || *start > '9') return -1;
    if (*start == '0' && start[1] >= '0' && start[1] <= '9') return -1;
    while (**cursor >= '0' && **cursor <= '9') {
        uint32_t digit = (uint32_t)(**cursor - '0');
        if (value > (UINT32_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
        ++*cursor;
    }
    *value_out = value;
    return 0;
}

int jvman_update_parse_version(const char *text, JvmanUpdateVersion *version) {
    const char *cursor;
    JvmanUpdateVersion parsed;
    if (!text || !*text || !version) return -1;
    cursor = text;
    if (*cursor == 'v') ++cursor;
    if (update_parse_component(&cursor, &parsed.major) != 0 ||
        *cursor++ != '.' ||
        update_parse_component(&cursor, &parsed.minor) != 0 ||
        *cursor++ != '.' ||
        update_parse_component(&cursor, &parsed.patch) != 0 ||
        *cursor != '\0') {
        return -1;
    }
    *version = parsed;
    return 0;
}

int jvman_update_compare_versions(const char *left, const char *right,
                                  int *comparison) {
    JvmanUpdateVersion a;
    JvmanUpdateVersion b;
    if (!comparison || jvman_update_parse_version(left, &a) != 0 ||
        jvman_update_parse_version(right, &b) != 0) return -1;
    if (a.major != b.major) *comparison = a.major < b.major ? -1 : 1;
    else if (a.minor != b.minor) *comparison = a.minor < b.minor ? -1 : 1;
    else if (a.patch != b.patch) *comparison = a.patch < b.patch ? -1 : 1;
    else *comparison = 0;
    return 0;
}

static int update_format_version(const JvmanUpdateVersion *version,
                                 char *out, size_t out_size) {
    int written;
    if (!version || !out || out_size == 0) return -1;
    written = snprintf(out, out_size, "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                       version->major, version->minor, version->patch);
    return written < 0 || (size_t)written >= out_size ? -1 : 0;
}

static void json_skip_space(JvmanJsonCursor *json) {
    while (json->cursor < json->end &&
           isspace((unsigned char)*json->cursor)) ++json->cursor;
}

static int json_hex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int json_parse_string(JvmanJsonCursor *json, char *out,
                             size_t out_size) {
    size_t used = 0;
    if (!json || json->cursor >= json->end || *json->cursor++ != '"') return -1;
    while (json->cursor < json->end) {
        unsigned char ch = (unsigned char)*json->cursor++;
        if (ch == '"') {
            if (out) {
                if (used >= out_size) return -1;
                out[used] = '\0';
            }
            return 0;
        }
        if (ch < 0x20u) return -1;
        if (ch == '\\') {
            unsigned int value;
            int h0, h1, h2, h3;
            if (json->cursor >= json->end) return -1;
            ch = (unsigned char)*json->cursor++;
            switch (ch) {
                case '"': case '\\': case '/': break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'u':
                    if ((size_t)(json->end - json->cursor) < 4u) return -1;
                    h0 = json_hex(json->cursor[0]);
                    h1 = json_hex(json->cursor[1]);
                    h2 = json_hex(json->cursor[2]);
                    h3 = json_hex(json->cursor[3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return -1;
                    value = (unsigned int)((h0 << 12) | (h1 << 8) |
                                           (h2 << 4) | h3);
                    json->cursor += 4;
                    if (!out) continue;
                    if (value > 0x7fu || (value >= 0xd800u && value <= 0xdfffu)) {
                        return -1;
                    }
                    ch = (unsigned char)value;
                    break;
                default: return -1;
            }
        }
        if (out) {
            if (ch == '\0') return -1;
            if (used + 1u >= out_size) return -1;
            out[used++] = (char)ch;
        }
    }
    return -1;
}

static int json_skip_value(JvmanJsonCursor *json);

static int json_skip_number(JvmanJsonCursor *json) {
    const char *cursor = json->cursor;
    if (cursor < json->end && *cursor == '-') ++cursor;
    if (cursor >= json->end) return -1;
    if (*cursor == '0') {
        ++cursor;
        if (cursor < json->end && isdigit((unsigned char)*cursor)) return -1;
    } else {
        if (*cursor < '1' || *cursor > '9') return -1;
        while (cursor < json->end && isdigit((unsigned char)*cursor)) ++cursor;
    }
    if (cursor < json->end && *cursor == '.') {
        ++cursor;
        if (cursor >= json->end || !isdigit((unsigned char)*cursor)) return -1;
        while (cursor < json->end && isdigit((unsigned char)*cursor)) ++cursor;
    }
    if (cursor < json->end && (*cursor == 'e' || *cursor == 'E')) {
        ++cursor;
        if (cursor < json->end && (*cursor == '+' || *cursor == '-')) ++cursor;
        if (cursor >= json->end || !isdigit((unsigned char)*cursor)) return -1;
        while (cursor < json->end && isdigit((unsigned char)*cursor)) ++cursor;
    }
    json->cursor = cursor;
    return 0;
}

static int json_skip_sequence(JvmanJsonCursor *json, char close,
                              int object) {
    int first = 1;
    if (++json->depth > 64u) return -1;
    for (;;) {
        json_skip_space(json);
        if (json->cursor >= json->end) return -1;
        if (*json->cursor == close) {
            ++json->cursor;
            --json->depth;
            return 0;
        }
        if (!first) {
            if (*json->cursor++ != ',') return -1;
            json_skip_space(json);
        }
        if (object) {
            if (json_parse_string(json, NULL, 0) != 0) return -1;
            json_skip_space(json);
            if (json->cursor >= json->end || *json->cursor++ != ':') return -1;
            json_skip_space(json);
        }
        if (json_skip_value(json) != 0) return -1;
        first = 0;
    }
}

static int json_skip_value(JvmanJsonCursor *json) {
    static const char *const literals[] = {"true", "false", "null"};
    size_t i;
    if (!json) return -1;
    json_skip_space(json);
    if (json->cursor >= json->end) return -1;
    if (*json->cursor == '"') return json_parse_string(json, NULL, 0);
    if (*json->cursor == '{') {
        ++json->cursor;
        return json_skip_sequence(json, '}', 1);
    }
    if (*json->cursor == '[') {
        ++json->cursor;
        return json_skip_sequence(json, ']', 0);
    }
    for (i = 0; i < sizeof(literals) / sizeof(literals[0]); ++i) {
        size_t length = strlen(literals[i]);
        if ((size_t)(json->end - json->cursor) >= length &&
            memcmp(json->cursor, literals[i], length) == 0) {
            json->cursor += length;
            return 0;
        }
    }
    return json_skip_number(json);
}

int jvman_update_parse_release_json(const char *text, size_t text_size,
                                    char *version, size_t version_size) {
    JvmanJsonCursor json;
    char key[64];
    char raw_version[64];
    JvmanUpdateVersion parsed;
    int found = 0;
    int first = 1;
    if (!text || text_size == 0 || text_size > JVMAN_UPDATE_JSON_LIMIT ||
        !version || version_size == 0 ||
        memchr(text, '\0', text_size) != NULL) return -1;
    json.cursor = text;
    json.end = text + text_size;
    json.depth = 1;
    json_skip_space(&json);
    if (json.cursor >= json.end || *json.cursor++ != '{') return -1;
    for (;;) {
        json_skip_space(&json);
        if (json.cursor >= json.end) return -1;
        if (*json.cursor == '}') {
            ++json.cursor;
            break;
        }
        if (!first) {
            if (*json.cursor++ != ',') return -1;
            json_skip_space(&json);
        }
        if (json_parse_string(&json, key, sizeof(key)) != 0) return -1;
        json_skip_space(&json);
        if (json.cursor >= json.end || *json.cursor++ != ':') return -1;
        json_skip_space(&json);
        if (strcmp(key, "tag_name") == 0) {
            if (found || json_parse_string(&json, raw_version,
                                            sizeof(raw_version)) != 0 ||
                jvman_update_parse_version(raw_version, &parsed) != 0) {
                return -1;
            }
            found = 1;
        } else if (json_skip_value(&json) != 0) {
            return -1;
        }
        first = 0;
    }
    json_skip_space(&json);
    if (!found || json.cursor != json.end ||
        update_format_version(&parsed, version, version_size) != 0) return -1;
    return 0;
}

static int update_asset_allowed(const char *asset) {
    static const char *const assets[] = {
        "jvman-windows-x86_64.exe",
        "jvman-setup-windows-x86_64.exe",
        "jvman-linux-x86_64",
        "jvman-macos-x86_64",
        "jvman-macos-aarch64",
        "SHA256SUMS"
    };
    size_t i;
    if (!asset) return 0;
    for (i = 0; i < sizeof(assets) / sizeof(assets[0]); ++i) {
        if (strcmp(asset, assets[i]) == 0) return 1;
    }
    return 0;
}

int jvman_update_parse_checksum(const char *text, size_t text_size,
                                const char *asset, char out[65]) {
    size_t offset = 0;
    int found = 0;
    if (!text || !asset || !out || !update_asset_allowed(asset) ||
        text_size == 0 || text_size > JVMAN_UPDATE_CHECKSUM_LIMIT ||
        memchr(text, '\0', text_size) != NULL) return -1;
    while (offset < text_size) {
        const char *line = text + offset;
        const char *newline = memchr(line, '\n', text_size - offset);
        size_t length = newline ? (size_t)(newline - line) : text_size - offset;
        size_t filename_start;
        size_t filename_length;
        size_t i;
        if (length > 0 && line[length - 1u] == '\r') --length;
        if (length == 0) {
            offset += newline ? 1u : 0u;
            if (!newline) break;
            continue;
        }
        if (length < 67u || length > 255u) return -1;
        for (i = 0; i < 64u; ++i) {
            if (!isxdigit((unsigned char)line[i])) return -1;
        }
        if (line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) return -1;
        filename_start = 66u;
        filename_length = length - filename_start;
        if (filename_length == 0 || filename_length > 128u) return -1;
        for (i = 0; i < filename_length; ++i) {
            unsigned char ch = (unsigned char)line[filename_start + i];
            if (!(isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) return -1;
        }
        if (strlen(asset) == filename_length &&
            memcmp(line + filename_start, asset, filename_length) == 0) {
            if (found) return -1;
            for (i = 0; i < 64u; ++i) {
                out[i] = (char)tolower((unsigned char)line[i]);
            }
            out[64] = '\0';
            found = 1;
        }
        if (!newline) break;
        offset = (size_t)(newline - text) + 1u;
    }
    return found ? 0 : -1;
}

int jvman_update_build_release_url(const char *version, const char *asset,
                                   char *out, size_t out_size) {
    JvmanUpdateVersion parsed;
    char canonical[64];
    int written;
    if (!out || out_size == 0 || !update_asset_allowed(asset) ||
        jvman_update_parse_version(version, &parsed) != 0 ||
        update_format_version(&parsed, canonical, sizeof(canonical)) != 0) {
        return -1;
    }
    written = snprintf(
        out, out_size,
        "https://github.com/standtrain/jvman/releases/download/v%s/%s",
        canonical, asset);
    return written < 0 || (size_t)written >= out_size ? -1 : 0;
}

const char *jvman_update_asset_for_platform(const char *os,
                                            const char *arch) {
    if (!os || !arch) return NULL;
    if (strcmp(os, "windows") == 0 && strcmp(arch, "x64") == 0) {
        return "jvman-windows-x86_64.exe";
    }
    if ((strcmp(os, "linux") == 0 || strcmp(os, "alpine-linux") == 0) &&
        strcmp(arch, "x64") == 0) {
        return "jvman-linux-x86_64";
    }
    if (strcmp(os, "mac") == 0 && strcmp(arch, "x64") == 0) {
        return "jvman-macos-x86_64";
    }
    if (strcmp(os, "mac") == 0 && strcmp(arch, "aarch64") == 0) {
        return "jvman-macos-aarch64";
    }
    return NULL;
}

static const char *update_platform_asset(void) {
    return jvman_update_asset_for_platform(platform_os_name(),
                                           platform_arch_name());
}

static int update_disk_image_contains_marker(const char *path,
                                             const char *marker,
                                             size_t marker_size) {
    char *data = NULL;
    size_t size = 0;
    size_t offset;
    int found = 0;
    if (!path || !marker || marker_size == 0 || platform_read_file_limited(
                     path, JVMAN_UPDATE_BINARY_LIMIT, &data, &size) != 0) {
        return -1;
    }
    if (size >= marker_size) {
        for (offset = 0; offset <= size - marker_size; ++offset) {
            if (memcmp(data + offset, marker, marker_size) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(data);
    return found;
}

static int update_download_text(const char *url, size_t limit,
                                char **text_out, size_t *size_out) {
    char temporary[JVMAN_PATH_MAX] = {0};
    char *text = NULL;
    size_t size = 0;
    int result = -1;
    if (!url || !text_out || !size_out) return -1;
    *text_out = NULL;
    *size_out = 0;
    if (platform_create_temporary_file(temporary, sizeof(temporary)) != 0) {
        return -1;
    }
    if (platform_https_download(url, temporary, limit, 0) != 0 ||
        platform_read_file_limited(temporary, limit, &text, &size) != 0) {
        goto done;
    }
    *text_out = text;
    *size_out = size;
    text = NULL;
    result = 0;
done:
    free(text);
    if (temporary[0]) (void)platform_remove_file(temporary);
    return result;
}

static int update_resolve_version(const char *requested, char *out,
                                  size_t out_size) {
    JvmanUpdateVersion parsed;
    char *json = NULL;
    size_t json_size = 0;
    int result;
    if (requested) {
        if (jvman_update_parse_version(requested, &parsed) != 0) return -1;
        return update_format_version(&parsed, out, out_size);
    }
    if (update_download_text(update_latest_url, JVMAN_UPDATE_JSON_LIMIT,
                             &json, &json_size) != 0) return -2;
    result = jvman_update_parse_release_json(json, json_size, out, out_size);
    free(json);
    return result == 0 ? 0 : -3;
}

int jvman_update_command(int check_only, const char *requested_version) {
    const char *asset = update_platform_asset();
    JvmanUpdateVersion requested_parsed;
    char version[64];
    char checksums_url[JVMAN_UPDATE_URL_MAX];
    char asset_url[JVMAN_UPDATE_URL_MAX];
    char checksum[65];
    char current_checksum[65];
    char release_version_marker[96];
    char self[JVMAN_PATH_MAX];
    char download[JVMAN_PATH_MAX] = {0};
    char staged[JVMAN_PATH_MAX] = {0};
    char *checksums = NULL;
    size_t checksums_size = 0;
    unsigned char digest[32];
    int comparison;
    int deferred = 0;
    int result = 1;
    int resolved;

    if (!asset) return update_error("self-update is not supported on this platform and architecture");
    if (requested_version &&
        jvman_update_parse_version(requested_version,
                                   &requested_parsed) != 0) {
        return update_error("update version must be MAJOR.MINOR.PATCH");
    }
    if (!check_only) {
        int version_match;
        if (platform_current_executable(self, sizeof(self)) != 0) {
            return update_platform_error(
                "cannot locate the running jvman executable");
        }
        /* Hash first, then verify the embedded version marker. If another
         * updater replaces the path between these reads, the marker check
         * rejects the new image; a replacement after both reads is rejected
         * by the publication CAS against current_checksum. */
        if (platform_sha256_file(self, digest) != 0) {
            return update_platform_error(
                "cannot fingerprint the running jvman executable");
        }
        version_match = update_disk_image_contains_marker(
            self, update_version_marker, sizeof(update_version_marker));
        if (version_match < 0) {
            return update_platform_error(
                "cannot verify the running jvman executable version");
        }
        if (!version_match) {
            return update_error(
                "the jvman executable changed after this process started");
        }
        update_digest_to_hex(digest, current_checksum);
    }
    resolved = update_resolve_version(requested_version, version, sizeof(version));
    if (resolved == -1) return update_error("update version must be MAJOR.MINOR.PATCH");
    if (resolved == -2) return update_platform_error("cannot read the GitHub release metadata");
    if (resolved != 0) return update_error("the GitHub release metadata is invalid");
    if (jvman_update_compare_versions(version, JVMAN_VERSION, &comparison) != 0) {
        return update_error("cannot compare update versions");
    }
    if (comparison < 0) {
        fprintf(stderr, "jvman: refusing to downgrade from %s to %s\n",
                JVMAN_VERSION, version);
        return 1;
    }
    if (comparison == 0) {
        printf("jvman %s is already up to date.\n", JVMAN_VERSION);
        return 0;
    }
    if (jvman_update_build_release_url(version, "SHA256SUMS", checksums_url,
                                       sizeof(checksums_url)) != 0 ||
        jvman_update_build_release_url(version, asset, asset_url,
                                       sizeof(asset_url)) != 0) {
        return update_error("cannot construct the fixed GitHub release URL");
    }
    if (update_download_text(checksums_url, JVMAN_UPDATE_CHECKSUM_LIMIT,
                             &checksums, &checksums_size) != 0) {
        return update_platform_error("cannot download SHA256SUMS");
    }
    if (jvman_update_parse_checksum(checksums, checksums_size, asset,
                                    checksum) != 0) {
        free(checksums);
        return update_error("the release checksum manifest is invalid");
    }
    free(checksums);
    if (check_only) {
        printf("Update available: %s -> %s\n", JVMAN_VERSION, version);
        return 0;
    }
    if (platform_create_temporary_file(download, sizeof(download)) != 0) {
        return update_platform_error("cannot create an update download file");
    }
    printf("Downloading jvman %s...\n", version);
    if (platform_https_download(asset_url, download, JVMAN_UPDATE_BINARY_LIMIT, 1) != 0) {
        update_platform_error("cannot download the release binary");
        goto done;
    }
    if (platform_sha256_file(download, digest) != 0 ||
        !jvman_hex_equal(digest, sizeof(digest), checksum)) {
        update_error("downloaded release binary failed SHA-256 verification");
        goto done;
    }
    if (platform_validate_executable_image(download) != 0) {
        update_platform_error("downloaded release asset is not a valid executable for this platform");
        goto done;
    }
    {
        int marker_length = snprintf(
            release_version_marker, sizeof(release_version_marker),
            "\x01JVMAN-SELF-VERSION:%s", version);
        int marker_match;
        if (marker_length < 0 ||
            (size_t)marker_length >= sizeof(release_version_marker)) {
            update_error("release version marker is too long");
            goto done;
        }
        marker_match = update_disk_image_contains_marker(
            download, release_version_marker, (size_t)marker_length + 1u);
        if (marker_match < 0) {
            update_platform_error(
                "cannot verify the downloaded release version");
            goto done;
        }
        if (!marker_match) {
            update_error(
                "downloaded release binary does not match the requested version");
            goto done;
        }
    }
    if (platform_stage_executable_update(download, self, staged,
                                         sizeof(staged)) != 0) {
        update_platform_error("cannot stage the update beside the running executable");
        goto done;
    }
    if (platform_remove_file(download) == 0) download[0] = '\0';
    if (platform_publish_executable_update(staged, self, checksum,
                                           current_checksum,
                                           &deferred) != 0) {
        update_platform_error("cannot replace the running jvman executable");
        goto done;
    }
    staged[0] = '\0';
    if (deferred) {
        printf("Update to jvman %s is scheduled and will finish after this process exits.\n",
               version);
    } else {
        printf("Updated jvman %s -> %s.\n", JVMAN_VERSION, version);
    }
    result = 0;
done:
    if (download[0]) (void)platform_remove_file(download);
    if (staged[0]) (void)platform_remove_file(staged);
    return result;
}

int jvman_update_run_cli(int argc, char **argv) {
    const char *version = NULL;
    int check_only = 0;
    int index;
    if (argc < 2 || !argv || !argv[1] || strcmp(argv[1], "update") != 0) {
        return update_error(
            "usage: jvman update [--check] [--version <version>]");
    }
    for (index = 2; index < argc; ++index) {
        if (!argv[index]) return update_error("invalid null update argument");
        if (strcmp(argv[index], "--check") == 0) {
            if (check_only) {
                return update_error(
                    "update option --check was specified more than once");
            }
            check_only = 1;
        } else if (strcmp(argv[index], "--version") == 0) {
            if (version || index + 1 >= argc || !argv[index + 1] ||
                strncmp(argv[index + 1], "--", 2) == 0) {
                return update_error(
                    "update option --version requires one version");
            }
            version = argv[++index];
        } else {
            return update_error(
                "usage: jvman update [--check] [--version <version>]");
        }
    }
    return jvman_update_command(check_only, version);
}
