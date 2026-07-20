#include "discovery.h"

#include "platform.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define JVMAN_RELEASE_FILE_LIMIT (64u * 1024u)

typedef struct DiscoveryVisited {
    char **paths;
    size_t count;
    size_t capacity;
} DiscoveryVisited;

typedef struct RegistryVisitorContext {
    JvmanDiscoveryList *list;
    const JvmanDiscoveryOptions *options;
} RegistryVisitorContext;

typedef struct SourceDescription {
    unsigned int bit;
    const char *name;
} SourceDescription;

static const SourceDescription source_descriptions[] = {
    {JVMAN_SOURCE_EXPLICIT, "explicit"},
    {JVMAN_SOURCE_JAVA_HOME, "JAVA_HOME"},
    {JVMAN_SOURCE_PATH, "PATH"},
    {JVMAN_SOURCE_REGISTRY, "registry"},
    {JVMAN_SOURCE_PROGRAM_FILES, "program-files"},
    {JVMAN_SOURCE_USER_JDKS, "user-jdks"},
    {JVMAN_SOURCE_SDKMAN, "sdkman"},
    {JVMAN_SOURCE_JABBA, "jabba"},
    {JVMAN_SOURCE_SYSTEM, "system"},
    {JVMAN_SOURCE_MACOS, "macos"},
    {JVMAN_SOURCE_HOMEBREW, "homebrew"}
};

static int discovery_is_separator(char ch);
static int discovery_copy(char *out, size_t out_size, const char *text);
static int discovery_add_home_internal(JvmanDiscoveryList *list,
                                       const char *input, unsigned int source,
                                       const char *vendor_hint,
                                       int internal_only);
static int discovery_raw_is_internal(const char *raw,
                                     const JvmanDiscoveryOptions *options);
static void discovery_scan_path_value(JvmanDiscoveryList *list,
                                      const char *path_value,
                                      const JvmanDiscoveryOptions *options);
static int discovery_registry_visitor(const char *home,
                                      const char *vendor_hint,
                                      void *context);
static void discovery_scan_optional_root(JvmanDiscoveryList *list,
                                         const char *root,
                                         unsigned int source,
                                         const char *vendor_hint,
                                         unsigned int max_depth);
static void discovery_scan_user_roots(JvmanDiscoveryList *list,
                                      const char *home);
#if defined(_WIN32)
static void discovery_scan_windows_program_base(JvmanDiscoveryList *list,
                                                const char *base);
#endif
#if defined(__APPLE__)
static void discovery_scan_homebrew(JvmanDiscoveryList *list, const char *root);
#endif

static void discovery_scan_common_roots(JvmanDiscoveryList *list) {
#if defined(_WIN32)
    const char *program_files = getenv("ProgramFiles");
    const char *program_files_x86 = getenv("ProgramFiles(x86)");
    const char *local_app_data = getenv("LOCALAPPDATA");
    char programs[JVMAN_PATH_MAX];
    discovery_scan_windows_program_base(list, program_files);
    discovery_scan_windows_program_base(list, program_files_x86);
    if (local_app_data && *local_app_data &&
        jvman_path_join(programs, sizeof(programs), local_app_data, "Programs") == 0) {
        discovery_scan_windows_program_base(list, programs);
    }
#elif defined(__APPLE__)
    discovery_scan_optional_root(list, "/Library/Java/JavaVirtualMachines",
                                 JVMAN_SOURCE_MACOS, NULL, 1);
    discovery_scan_homebrew(list, "/opt/homebrew/opt");
    discovery_scan_homebrew(list, "/usr/local/opt");
#else
    static const char *const roots[] = {
        "/usr/java",
        "/usr/lib/jvm",
        "/usr/lib64/jvm",
        "/usr/lib32/jvm",
        "/usr/libx32/jvm",
        "/usr/local/lib/jvm"
    };
    size_t i;
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        discovery_scan_optional_root(list, roots[i], JVMAN_SOURCE_SYSTEM,
                                     NULL, 1);
    }
#endif
}

int jvman_discovery_scan(JvmanDiscoveryList *list,
                         const JvmanDiscoveryOptions *options) {
    JvmanDiscoveryOptions defaults;
    const char *java_home;
    const char *path_value;
    const char *user_home;
    RegistryVisitorContext visitor;
    if (!list) return -1;
    if (!options) {
        jvman_discovery_options_init(&defaults);
        options = &defaults;
    }
    java_home = options->java_home;
    path_value = options->path;
    user_home = options->user_home;
    if (options->use_environment) {
        if (!java_home) java_home = getenv("JAVA_HOME");
        if (!path_value) path_value = getenv("PATH");
        if (!user_home) {
#if defined(_WIN32)
            user_home = getenv("USERPROFILE");
#else
            user_home = getenv("HOME");
#endif
        }
    }
    if (java_home && *java_home) {
        (void)discovery_add_home_internal(
            list, java_home, JVMAN_SOURCE_JAVA_HOME, NULL,
            discovery_raw_is_internal(java_home, options));
    }
    discovery_scan_path_value(list, path_value, options);
    if (options->registry_enumerator) {
        int result;
        visitor.list = list;
        visitor.options = options;
        result = options->registry_enumerator(discovery_registry_visitor, &visitor,
                                              options->registry_context);
        if (result != 0) return -1;
    }
    if (options->scan_common_roots) discovery_scan_common_roots(list);
    discovery_scan_user_roots(list, user_home);
    jvman_discovery_sort(list);
    return 0;
}

static int discovery_path_order(const char *left, const char *right) {
    while (*left && *right) {
        unsigned char a = (unsigned char)*left;
        unsigned char b = (unsigned char)*right;
        if (discovery_is_separator((char)a)) a = (unsigned char)'/';
        if (discovery_is_separator((char)b)) b = (unsigned char)'/';
#if defined(_WIN32)
        a = (unsigned char)tolower(a);
        b = (unsigned char)tolower(b);
#endif
        if (a != b) return a < b ? -1 : 1;
        ++left;
        ++right;
    }
    if (*left) return 1;
    if (*right) return -1;
    return 0;
}

static int discovery_candidate_compare(const void *left, const void *right) {
    const JvmanDiscoveryCandidate *a = (const JvmanDiscoveryCandidate *)left;
    const JvmanDiscoveryCandidate *b = (const JvmanDiscoveryCandidate *)right;
    return discovery_path_order(a->home, b->home);
}

void jvman_discovery_sort(JvmanDiscoveryList *list) {
    if (!list || list->count == 0) return;
    qsort(list->items, list->count, sizeof(*list->items),
          discovery_candidate_compare);
}

const char *jvman_discovery_type_name(JvmanDiscoveryType type) {
    switch (type) {
        case JVMAN_DISCOVERY_JDK: return "JDK";
        case JVMAN_DISCOVERY_JRE: return "JRE";
        default: return "INVALID";
    }
}

int jvman_discovery_format_sources(unsigned int sources,
                                   char *out, size_t out_size) {
    size_t i;
    size_t used = 0;
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    for (i = 0; i < sizeof(source_descriptions) /
                        sizeof(source_descriptions[0]); ++i) {
        size_t name_length;
        if (!(sources & source_descriptions[i].bit)) continue;
        name_length = strlen(source_descriptions[i].name);
        if (used + (used ? 1u : 0u) + name_length + 1 > out_size) return -1;
        if (used) out[used++] = ',';
        memcpy(out + used, source_descriptions[i].name, name_length);
        used += name_length;
        out[used] = '\0';
    }
    if (!used) return discovery_copy(out, out_size, "unknown");
    return 0;
}

static int discovery_is_separator(char ch) {
#if defined(_WIN32)
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

static int discovery_copy(char *out, size_t out_size, const char *text) {
    size_t length;
    if (!out || out_size == 0 || !text) return -1;
    length = strlen(text);
    if (length + 1 > out_size) return -1;
    memcpy(out, text, length + 1);
    return 0;
}

static int discovery_ascii_equal(char left, char right) {
    return tolower((unsigned char)left) == tolower((unsigned char)right);
}

static int discovery_text_equal(const char *left, const char *right) {
    if (!left || !right) return 0;
    while (*left && *right) {
        if (!discovery_ascii_equal(*left, *right)) return 0;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int discovery_contains(const char *text, const char *needle) {
    size_t needle_length;
    const char *start;
    if (!text || !needle || !*needle) return 0;
    needle_length = strlen(needle);
    for (start = text; *start; ++start) {
        size_t i;
        for (i = 0; i < needle_length && start[i]; ++i) {
            if (!discovery_ascii_equal(start[i], needle[i])) break;
        }
        if (i == needle_length) return 1;
    }
    return 0;
}

static int discovery_clean_input(const char *input, char *out, size_t out_size) {
    const char *start;
    const char *end;
    char unexpanded[JVMAN_PATH_MAX];
    size_t length;
    if (!input || !out || out_size == 0) return -1;
    start = input;
    while (*start && isspace((unsigned char)*start)) ++start;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if (end - start >= 2 && start[0] == '"' && end[-1] == '"') {
        ++start;
        --end;
        while (end > start && isspace((unsigned char)end[-1])) --end;
    }
    length = (size_t)(end - start);
    if (length == 0 || length >= sizeof(unexpanded)) return -1;
    memcpy(unexpanded, start, length);
    unexpanded[length] = '\0';
#if defined(_WIN32)
    {
        DWORD required = ExpandEnvironmentStringsA(unexpanded, out, (DWORD)out_size);
        if (required == 0 || required > out_size) return -1;
    }
#else
    if (discovery_copy(out, out_size, unexpanded) != 0) return -1;
#endif
    jvman_strip_trailing_separators(out);
    return out[0] ? 0 : -1;
}

static int discovery_dirname(char *path) {
    size_t length;
    if (!path || !*path) return -1;
    jvman_strip_trailing_separators(path);
    length = strlen(path);
    while (length > 0 && !discovery_is_separator(path[length - 1])) --length;
    if (length == 0) return -1;
#if defined(_WIN32)
    if (length == 3 && path[1] == ':') {
        path[length] = '\0';
        return 0;
    }
#endif
    while (length > 1 && discovery_is_separator(path[length - 1])) --length;
    path[length] = '\0';
    return 0;
}

static const char *discovery_basename(const char *path) {
    const char *base = path;
    const char *cursor;
    if (!path) return "";
    for (cursor = path; *cursor; ++cursor) {
        if (discovery_is_separator(*cursor)) base = cursor + 1;
    }
    return base;
}

static int discovery_parse_value(const char *start, const char *end,
                                 char *out, size_t out_size) {
    size_t used = 0;
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if (start == end || !out || out_size == 0) return -1;
    if (*start == '"') {
        int closed = 0;
        ++start;
        while (start < end) {
            char ch = *start++;
            if (ch == '"') {
                closed = 1;
                break;
            }
            if (ch == '\\') {
                if (start == end) return -1;
                ch = *start++;
                switch (ch) {
                    case '"':
                    case '\\':
                    case '/':
                        break;
                    case 'b': ch = '\b'; break;
                    case 'f': ch = '\f'; break;
                    case 'n': ch = '\n'; break;
                    case 'r': ch = '\r'; break;
                    case 't': ch = '\t'; break;
                    default:
                        return -1;
                }
            }
            if (used + 1 >= out_size) return -1;
            out[used++] = ch;
        }
        if (!closed) return -1;
        while (start < end && isspace((unsigned char)*start)) ++start;
        if (start != end) return -1;
    } else {
        while (start < end) {
            if (used + 1 >= out_size) return -1;
            out[used++] = *start++;
        }
        while (used && isspace((unsigned char)out[used - 1])) --used;
    }
    out[used] = '\0';
    return used ? 0 : -1;
}

static int discovery_release_version_valid(const char *version) {
    const unsigned char *cursor = (const unsigned char *)version;
    unsigned int major = 0;
    if (!cursor || !isdigit(*cursor)) return 0;
    while (isdigit(*cursor)) {
        if (major > 100000u) return 0;
        major = major * 10u + (unsigned int)(*cursor - (unsigned char)'0');
        ++cursor;
    }
    if (major == 1u && *cursor == '.' && isdigit(cursor[1])) {
        major = 0;
        ++cursor;
        while (isdigit(*cursor)) {
            if (major > 100000u) return 0;
            major = major * 10u + (unsigned int)(*cursor - (unsigned char)'0');
            ++cursor;
        }
    }
    if (major < 8u) return 0;
    if (*cursor && *cursor != '.' && *cursor != '_' && *cursor != '+' &&
        *cursor != '-' && !(major == 8u && *cursor == 'u')) {
        return 0;
    }
    while (*cursor) {
        if (!isalnum(*cursor) && *cursor != '.' && *cursor != '_' &&
            *cursor != '+' && *cursor != '-') {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

int jvman_discovery_parse_release(const char *path, JvmanJavaRelease *release) {
    FILE *file;
    long file_size;
    char *data;
    size_t size;
    const char *cursor;
    const char *limit;
    int version_found = 0;
    if (!path || !release) return -1;
    memset(release, 0, sizeof(*release));
    file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0 ||
        (file_size = ftell(file)) < 0 ||
        (unsigned long)file_size > JVMAN_RELEASE_FILE_LIMIT ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    size = (size_t)file_size;
    data = (char *)malloc(size + 1);
    if (!data) {
        fclose(file);
        return -1;
    }
    if (size && fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);
    if (memchr(data, '\0', size) != NULL) {
        free(data);
        return -1;
    }
    data[size] = '\0';
    cursor = data;
    limit = data + size;
    if (size >= 3 && (unsigned char)data[0] == 0xef &&
        (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) {
        cursor += 3;
    }
    while (cursor < limit) {
        const char *line_end = memchr(cursor, '\n', (size_t)(limit - cursor));
        const char *key_start;
        const char *key_end;
        const char *value;
        char *destination = NULL;
        size_t destination_size = 0;
        if (!line_end) line_end = limit;
        if (line_end > cursor && line_end[-1] == '\r') --line_end;
        key_start = cursor;
        while (key_start < line_end && isspace((unsigned char)*key_start)) ++key_start;
        key_end = key_start;
        while (key_end < line_end &&
               (isalnum((unsigned char)*key_end) || *key_end == '_')) ++key_end;
        value = key_end;
        while (value < line_end && isspace((unsigned char)*value)) ++value;
        if (value < line_end && *value == '=') {
            size_t key_length = (size_t)(key_end - key_start);
            ++value;
            if (key_length == 12 && memcmp(key_start, "JAVA_VERSION", 12) == 0) {
                destination = release->version;
                destination_size = sizeof(release->version);
            } else if (key_length == 11 && memcmp(key_start, "IMPLEMENTOR", 11) == 0) {
                destination = release->implementor;
                destination_size = sizeof(release->implementor);
            } else if (key_length == 7 && memcmp(key_start, "OS_ARCH", 7) == 0) {
                destination = release->arch;
                destination_size = sizeof(release->arch);
            }
            if (destination &&
                discovery_parse_value(value, line_end, destination,
                                      destination_size) != 0) {
                free(data);
                memset(release, 0, sizeof(*release));
                return -1;
            }
            if (destination == release->version) version_found = 1;
        }
        cursor = line_end < limit ? line_end + 1 : limit;
    }
    free(data);
    if (!version_found ||
        !discovery_release_version_valid(release->version)) {
        memset(release, 0, sizeof(*release));
        return -1;
    }
    return 0;
}

int jvman_discovery_normalize_vendor(const char *implementor,
                                     const char *path_hint,
                                     char *out, size_t out_size) {
    const char *text;
    const char *vendor = "unknown";
    if (!out || out_size == 0) return -1;
    text = implementor && *implementor ? implementor : path_hint;
    if (!text) text = "";
    if (discovery_contains(text, "adoptium") ||
        discovery_contains(text, "temurin") ||
        discovery_contains(text, "adoptopenjdk")) {
        vendor = "temurin";
    } else if (discovery_contains(text, "amazon") ||
               discovery_contains(text, "corretto")) {
        vendor = "corretto";
    } else if (discovery_contains(text, "bellsoft") ||
               discovery_contains(text, "liberica")) {
        vendor = "liberica";
    } else if (discovery_contains(text, "azul") ||
               discovery_contains(text, "zulu")) {
        vendor = "zulu";
    } else if (discovery_contains(text, "microsoft")) {
        vendor = "microsoft";
    } else if (discovery_contains(text, "semeru") ||
               discovery_contains(text, "international business machines") ||
               discovery_contains(text, "ibm")) {
        vendor = "semeru";
    } else if (discovery_contains(text, "oracle")) {
        vendor = "oracle";
    } else if (discovery_contains(text, "sapmachine") ||
               discovery_contains(text, "sap se")) {
        vendor = "sapmachine";
    } else if (discovery_contains(text, "dragonwell") ||
               discovery_contains(text, "alibaba")) {
        vendor = "dragonwell";
    } else if (discovery_contains(text, "kona") ||
               discovery_contains(text, "tencent")) {
        vendor = "kona";
    } else if (discovery_contains(text, "red hat") ||
               discovery_contains(text, "redhat")) {
        vendor = "redhat";
    } else if (discovery_contains(text, "jetbrains") ||
               discovery_contains(text, "jbr")) {
        vendor = "jetbrains";
    } else if (discovery_contains(text, "graalvm") ||
               discovery_contains(text, "graal vm")) {
        vendor = "graalvm";
    } else if (discovery_contains(text, "openlogic")) {
        vendor = "openlogic";
    } else if (discovery_contains(text, "openjdk")) {
        vendor = "openjdk";
    }
    return discovery_copy(out, out_size, vendor);
}

int jvman_discovery_normalize_version(const char *version,
                                      char *out, size_t out_size) {
    const char *cursor;
    char java8[JVMAN_DISCOVERY_VERSION_MAX + 3];
    size_t used = 0;
    int last_was_dash = 0;
    if (!version || !*version || !out || out_size == 0) return -1;
    cursor = version;
    if (strncmp(version, "1.8.0_", 6) == 0 && version[6]) {
        if (snprintf(java8, sizeof(java8), "8u%s", version + 6) >=
            (int)sizeof(java8)) return -1;
        cursor = java8;
    } else if (strcmp(version, "1.8.0") == 0) {
        cursor = "8";
    }
    while (*cursor && used + 1 < out_size) {
        unsigned char ch = (unsigned char)*cursor++;
        if (isalnum(ch) || ch == '.' || ch == '_' || ch == '+' || ch == '-') {
            out[used++] = (char)ch;
            last_was_dash = ch == '-';
        } else if (!last_was_dash && used) {
            out[used++] = '-';
            last_was_dash = 1;
        }
    }
    while (used && out[used - 1] == '-') --used;
    if (!used) return -1;
    out[used] = '\0';
    return 0;
}

int jvman_discovery_suggest_name(const char *vendor, const char *version,
                                 char *out, size_t out_size) {
    char normalized[JVMAN_DISCOVERY_VERSION_MAX + 1];
    const char *safe_vendor = vendor && *vendor ? vendor : "unknown";
    size_t vendor_length;
    size_t version_length;
    size_t available;
    if (!out || out_size < 4 ||
        jvman_discovery_normalize_version(version && *version ? version : "unknown",
                                          normalized, sizeof(normalized)) != 0) {
        return -1;
    }
    available = out_size - 1;
    if (available > JVMAN_NAME_MAX) available = JVMAN_NAME_MAX;
    vendor_length = strlen(safe_vendor);
    if (vendor_length + 2 > available) return -1;
    version_length = strlen(normalized);
    if (version_length > available - vendor_length - 1) {
        version_length = available - vendor_length - 1;
    }
    memcpy(out, safe_vendor, vendor_length);
    out[vendor_length] = '-';
    memcpy(out + vendor_length + 1, normalized, version_length);
    out[vendor_length + 1 + version_length] = '\0';
    return jvman_valid_name(out) ? 0 : -1;
}

static int discovery_home_has_marker(const JvmanDiscoveryCandidate *candidate) {
    char path[JVMAN_PATH_MAX];
    if (!candidate) return 0;
    if (candidate->type != JVMAN_DISCOVERY_INVALID) return 1;
    if (jvman_path_join(path, sizeof(path), candidate->home, "release") == 0 &&
        platform_is_file(path)) return 1;
    if (jvman_path_join3(path, sizeof(path), candidate->home, "bin",
                         JVMAN_JAVA_EXE) == 0 && platform_is_file(path)) return 1;
    if (jvman_path_join3(path, sizeof(path), candidate->home, "bin",
                         JVMAN_JAVAC_EXE) == 0 && platform_is_file(path)) return 1;
    return 0;
}

int jvman_discovery_probe_home(const char *input, unsigned int source,
                               const char *vendor_hint,
                               JvmanDiscoveryCandidate *candidate) {
    char cleaned[JVMAN_PATH_MAX];
    char absolute[JVMAN_PATH_MAX];
    char bundle_home[JVMAN_PATH_MAX];
    char canonical_home[JVMAN_PATH_MAX];
    char java_path[JVMAN_PATH_MAX];
    char javac_path[JVMAN_PATH_MAX];
    char release_path[JVMAN_PATH_MAX];
    char hint[JVMAN_PATH_MAX + JVMAN_DISCOVERY_IMPLEMENTOR_MAX + 2];
    JvmanJavaRelease release;
    int has_java;
    int has_javac;
    if (!candidate || discovery_clean_input(input, cleaned, sizeof(cleaned)) != 0 ||
        platform_absolute_path(cleaned, absolute, sizeof(absolute)) != 0 ||
        !platform_is_directory(absolute)) {
        return -1;
    }
    if (jvman_path_join3(bundle_home, sizeof(bundle_home), absolute,
                         "Contents", "Home") == 0 &&
        platform_is_directory(bundle_home)) {
        char marker[JVMAN_PATH_MAX];
        if ((jvman_path_join3(marker, sizeof(marker), bundle_home, "bin",
                              JVMAN_JAVA_EXE) == 0 && platform_is_file(marker)) ||
            (jvman_path_join(marker, sizeof(marker), bundle_home, "release") == 0 &&
             platform_is_file(marker))) {
            if (platform_absolute_path(bundle_home, canonical_home,
                                       sizeof(canonical_home)) != 0) return -1;
        } else {
            if (discovery_copy(canonical_home, sizeof(canonical_home), absolute) != 0)
                return -1;
        }
    } else if (discovery_copy(canonical_home, sizeof(canonical_home), absolute) != 0) {
        return -1;
    }
    memset(candidate, 0, sizeof(*candidate));
    candidate->sources = source;
    if (discovery_copy(candidate->home, sizeof(candidate->home), canonical_home) != 0 ||
        jvman_path_join3(java_path, sizeof(java_path), canonical_home, "bin",
                         JVMAN_JAVA_EXE) != 0 ||
        jvman_path_join3(javac_path, sizeof(javac_path), canonical_home, "bin",
                         JVMAN_JAVAC_EXE) != 0 ||
        jvman_path_join(release_path, sizeof(release_path), canonical_home,
                        "release") != 0) {
        return -1;
    }
    has_java = platform_is_file(java_path);
    has_javac = platform_is_file(javac_path);
    candidate->release_valid =
        platform_is_file(release_path) &&
        jvman_discovery_parse_release(release_path, &release) == 0;
    if (candidate->release_valid) {
        discovery_copy(candidate->version, sizeof(candidate->version), release.version);
        discovery_copy(candidate->implementor, sizeof(candidate->implementor),
                       release.implementor);
        discovery_copy(candidate->arch, sizeof(candidate->arch), release.arch);
    } else {
        memset(&release, 0, sizeof(release));
        discovery_copy(candidate->version, sizeof(candidate->version), "unknown");
    }
    if (has_java && has_javac && candidate->release_valid) {
        candidate->type = JVMAN_DISCOVERY_JDK;
    } else if (has_java && !has_javac) {
        candidate->type = JVMAN_DISCOVERY_JRE;
    } else {
        candidate->type = JVMAN_DISCOVERY_INVALID;
    }
    snprintf(hint, sizeof(hint), "%s %s", vendor_hint ? vendor_hint : "",
             canonical_home);
    if (jvman_discovery_normalize_vendor(candidate->implementor, hint,
                                         candidate->vendor,
                                         sizeof(candidate->vendor)) != 0 ||
        jvman_discovery_suggest_name(candidate->vendor, candidate->version,
                                     candidate->suggested_name,
                                     sizeof(candidate->suggested_name)) != 0) {
        return -1;
    }
    return 0;
}

int jvman_discovery_probe_java(const char *input, unsigned int source,
                               const char *vendor_hint,
                               JvmanDiscoveryCandidate *candidate) {
    char cleaned[JVMAN_PATH_MAX];
    char executable[JVMAN_PATH_MAX];
    char canonical[JVMAN_PATH_MAX];
    char bin_directory[JVMAN_PATH_MAX];
    char home[JVMAN_PATH_MAX];
    if (!candidate || discovery_clean_input(input, cleaned, sizeof(cleaned)) != 0)
        return -1;
    if (platform_is_directory(cleaned)) {
        if (jvman_path_join(executable, sizeof(executable), cleaned,
                            JVMAN_JAVA_EXE) != 0) return -1;
    } else if (discovery_copy(executable, sizeof(executable), cleaned) != 0) {
        return -1;
    }
    if (!platform_is_file(executable) ||
        platform_absolute_path(executable, canonical, sizeof(canonical)) != 0 ||
        !platform_is_file(canonical) ||
        !discovery_text_equal(discovery_basename(canonical), JVMAN_JAVA_EXE) ||
        discovery_copy(bin_directory, sizeof(bin_directory), canonical) != 0 ||
        discovery_dirname(bin_directory) != 0 ||
        !discovery_text_equal(discovery_basename(bin_directory), "bin") ||
        discovery_copy(home, sizeof(home), bin_directory) != 0 ||
        discovery_dirname(home) != 0) {
        return -1;
    }
    return jvman_discovery_probe_home(home, source, vendor_hint, candidate);
}

int jvman_discovery_find_jdk_home(const char *input,
                                  char *out, size_t out_size) {
    JvmanDiscoveryCandidate candidate;
    if (!out || out_size == 0 ||
        jvman_discovery_probe_home(input, JVMAN_SOURCE_EXPLICIT, NULL,
                                   &candidate) != 0 ||
        candidate.type != JVMAN_DISCOVERY_JDK) {
        return -1;
    }
    return discovery_copy(out, out_size, candidate.home);
}

void jvman_discovery_options_init(JvmanDiscoveryOptions *options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->use_environment = 1;
    options->scan_common_roots = 1;
}

void jvman_discovery_list_init(JvmanDiscoveryList *list) {
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

void jvman_discovery_list_free(JvmanDiscoveryList *list) {
    if (!list) return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int discovery_candidate_quality(const JvmanDiscoveryCandidate *candidate) {
    int quality = candidate ? (int)candidate->type * 4 : 0;
    if (candidate && candidate->release_valid) quality += 2;
    if (candidate && strcmp(candidate->vendor, "unknown") != 0) quality += 1;
    return quality;
}

static int discovery_reserve(JvmanDiscoveryList *list, size_t required) {
    JvmanDiscoveryCandidate *grown;
    size_t capacity;
    if (required <= list->capacity) return 0;
    capacity = list->capacity ? list->capacity * 2 : 16;
    if (capacity < required) capacity = required;
    if (capacity > (size_t)-1 / sizeof(*list->items)) return -1;
    grown = (JvmanDiscoveryCandidate *)realloc(
        list->items, capacity * sizeof(*list->items));
    if (!grown) return -1;
    list->items = grown;
    list->capacity = capacity;
    return 0;
}

static int discovery_merge_candidate(JvmanDiscoveryList *list,
                                     const JvmanDiscoveryCandidate *candidate,
                                     int internal_only) {
    size_t i;
    if (!list || !candidate) return -1;
    for (i = 0; i < list->count; ++i) {
        JvmanDiscoveryCandidate *existing = &list->items[i];
        if (jvman_path_equal(existing->home, candidate->home)) {
            unsigned int sources = existing->sources | candidate->sources;
            int remains_internal = existing->internal_only && internal_only;
            if (discovery_candidate_quality(candidate) >
                discovery_candidate_quality(existing)) {
                *existing = *candidate;
            } else if (strcmp(existing->vendor, "unknown") == 0 &&
                       strcmp(candidate->vendor, "unknown") != 0) {
                discovery_copy(existing->vendor, sizeof(existing->vendor),
                               candidate->vendor);
                discovery_copy(existing->implementor, sizeof(existing->implementor),
                               candidate->implementor);
                jvman_discovery_suggest_name(existing->vendor, existing->version,
                                             existing->suggested_name,
                                             sizeof(existing->suggested_name));
            }
            existing->sources = sources;
            existing->internal_only = remains_internal;
            return 0;
        }
    }
    if (discovery_reserve(list, list->count + 1) != 0) return -1;
    list->items[list->count] = *candidate;
    list->items[list->count].internal_only = internal_only;
    ++list->count;
    return 0;
}

static int discovery_add_home_internal(JvmanDiscoveryList *list,
                                       const char *input, unsigned int source,
                                       const char *vendor_hint,
                                       int internal_only) {
    JvmanDiscoveryCandidate candidate;
    if (jvman_discovery_probe_home(input, source, vendor_hint, &candidate) != 0)
        return -1;
    return discovery_merge_candidate(list, &candidate, internal_only);
}

static int discovery_add_java_internal(JvmanDiscoveryList *list,
                                       const char *input, unsigned int source,
                                       const char *vendor_hint,
                                       int internal_only) {
    JvmanDiscoveryCandidate candidate;
    if (jvman_discovery_probe_java(input, source, vendor_hint, &candidate) != 0)
        return -1;
    return discovery_merge_candidate(list, &candidate, internal_only);
}

int jvman_discovery_add_home(JvmanDiscoveryList *list, const char *input,
                             unsigned int source, const char *vendor_hint) {
    return discovery_add_home_internal(list, input, source, vendor_hint, 0);
}

int jvman_discovery_add_java(JvmanDiscoveryList *list, const char *input,
                             unsigned int source, const char *vendor_hint) {
    return discovery_add_java_internal(list, input, source, vendor_hint, 0);
}

static void discovery_visited_free(DiscoveryVisited *visited) {
    size_t i;
    if (!visited) return;
    for (i = 0; i < visited->count; ++i) free(visited->paths[i]);
    free(visited->paths);
    memset(visited, 0, sizeof(*visited));
}

static int discovery_visited_enter(DiscoveryVisited *visited, const char *path) {
    char canonical[JVMAN_PATH_MAX];
    char *copy;
    size_t i;
    if (!visited || platform_absolute_path(path, canonical, sizeof(canonical)) != 0)
        return -1;
    for (i = 0; i < visited->count; ++i) {
        if (jvman_path_equal(visited->paths[i], canonical)) return 0;
    }
    if (visited->count == visited->capacity) {
        size_t capacity = visited->capacity ? visited->capacity * 2 : 16;
        char **grown;
        if (capacity > (size_t)-1 / sizeof(*visited->paths)) return -1;
        grown = (char **)realloc(visited->paths,
                                 capacity * sizeof(*visited->paths));
        if (!grown) return -1;
        visited->paths = grown;
        visited->capacity = capacity;
    }
    copy = (char *)malloc(strlen(canonical) + 1);
    if (!copy) return -1;
    strcpy(copy, canonical);
    visited->paths[visited->count++] = copy;
    return 1;
}

static int discovery_name_pointer_compare(const void *left, const void *right) {
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
#if defined(_WIN32)
    return _stricmp(*a, *b);
#else
    return strcmp(*a, *b);
#endif
}

static int discovery_scan_tree(JvmanDiscoveryList *list, const char *root,
                               unsigned int source, const char *vendor_hint,
                               unsigned int remaining_depth,
                               DiscoveryVisited *visited) {
    JvmanDiscoveryCandidate candidate;
    char **names = NULL;
    size_t count = 0;
    size_t i;
    int entered;
    int is_candidate = 0;
    if (!platform_is_directory(root)) return 0;
    entered = discovery_visited_enter(visited, root);
    if (entered <= 0) return entered < 0 ? -1 : 0;
    if (jvman_discovery_probe_home(root, source, vendor_hint, &candidate) == 0 &&
        discovery_home_has_marker(&candidate)) {
        if (discovery_merge_candidate(list, &candidate, 0) != 0) return -1;
        is_candidate = 1;
    }
    if (is_candidate || remaining_depth == 0) return 0;
    if (platform_list_directory(root, &names, &count) != 0) return -1;
    if (count > 1) {
        qsort(names, count, sizeof(*names), discovery_name_pointer_compare);
    }
    for (i = 0; i < count; ++i) {
        char child[JVMAN_PATH_MAX];
        if (jvman_path_join(child, sizeof(child), root, names[i]) != 0 ||
            !platform_is_directory(child)) continue;
        if (discovery_scan_tree(list, child, source, vendor_hint,
                                remaining_depth - 1, visited) != 0) {
            platform_free_directory_list(names, count);
            return -1;
        }
    }
    platform_free_directory_list(names, count);
    return 0;
}

int jvman_discovery_scan_root(JvmanDiscoveryList *list, const char *root,
                              unsigned int source, const char *vendor_hint,
                              unsigned int max_depth) {
    DiscoveryVisited visited;
    int result;
    if (!list || !root || !*root) return -1;
    memset(&visited, 0, sizeof(visited));
    result = discovery_scan_tree(list, root, source, vendor_hint,
                                 max_depth, &visited);
    discovery_visited_free(&visited);
    return result;
}

static int discovery_raw_is_internal(const char *raw,
                                     const JvmanDiscoveryOptions *options) {
    char cleaned[JVMAN_PATH_MAX];
    char current[JVMAN_PATH_MAX];
    if (!raw || !options ||
        discovery_clean_input(raw, cleaned, sizeof(cleaned)) != 0) return 0;
    if (options->jvman_root && *options->jvman_root &&
        jvman_path_join(current, sizeof(current), options->jvman_root,
                        "current") == 0 &&
        (jvman_path_equal(current, cleaned) ||
         jvman_path_is_within(current, cleaned))) return 1;
    if (options->jvman_jdks && *options->jvman_jdks &&
        (jvman_path_equal(options->jvman_jdks, cleaned) ||
         jvman_path_is_within(options->jvman_jdks, cleaned))) return 1;
    return 0;
}

static void discovery_scan_path_value(JvmanDiscoveryList *list,
                                      const char *path_value,
                                      const JvmanDiscoveryOptions *options) {
    const char *cursor;
    const char *segment_start;
#if defined(_WIN32)
    int quoted = 0;
#endif
    if (!list || !path_value || !*path_value) return;
    cursor = path_value;
    segment_start = cursor;
    for (;;) {
        int at_end = *cursor == '\0';
#if defined(_WIN32)
        int at_separator = *cursor == ';' && !quoted;
        if (*cursor == '"') quoted = !quoted;
#else
        int at_separator = *cursor == ':';
#endif
        if (at_end || at_separator) {
            size_t length = (size_t)(cursor - segment_start);
            if (length > 0 && length < JVMAN_PATH_MAX) {
                char segment[JVMAN_PATH_MAX];
                memcpy(segment, segment_start, length);
                segment[length] = '\0';
                (void)discovery_add_java_internal(
                    list, segment, JVMAN_SOURCE_PATH, NULL,
                    discovery_raw_is_internal(segment, options));
            }
            if (at_end) break;
            segment_start = cursor + 1;
        }
        ++cursor;
    }
}

static int discovery_registry_visitor(const char *home,
                                      const char *vendor_hint,
                                      void *context) {
    RegistryVisitorContext *visitor = (RegistryVisitorContext *)context;
    JvmanDiscoveryCandidate candidate;
    if (!visitor || !home ||
        jvman_discovery_probe_home(home, JVMAN_SOURCE_REGISTRY, vendor_hint,
                                   &candidate) != 0) {
        return 0;
    }
    return discovery_merge_candidate(
        visitor->list, &candidate,
        discovery_raw_is_internal(home, visitor->options));
}

static void discovery_scan_optional_root(JvmanDiscoveryList *list,
                                         const char *root,
                                         unsigned int source,
                                         const char *vendor_hint,
                                         unsigned int max_depth) {
    if (root && *root && platform_is_directory(root)) {
        (void)jvman_discovery_scan_root(list, root, source, vendor_hint, max_depth);
    }
}

static void discovery_scan_joined_root(JvmanDiscoveryList *list,
                                       const char *base, const char *relative,
                                       unsigned int source,
                                       const char *vendor_hint,
                                       unsigned int max_depth) {
    char root[JVMAN_PATH_MAX];
    if (base && *base &&
        jvman_path_join(root, sizeof(root), base, relative) == 0) {
        discovery_scan_optional_root(list, root, source, vendor_hint, max_depth);
    }
}

static void discovery_scan_user_roots(JvmanDiscoveryList *list,
                                      const char *home) {
    char root[JVMAN_PATH_MAX];
    if (!home || !*home) return;
    discovery_scan_joined_root(list, home, ".jdks", JVMAN_SOURCE_USER_JDKS,
                               NULL, 1);
    if (jvman_path_join3(root, sizeof(root), home, ".sdkman/candidates",
                         "java") == 0) {
        discovery_scan_optional_root(list, root, JVMAN_SOURCE_SDKMAN, NULL, 1);
    }
    if (jvman_path_join3(root, sizeof(root), home, ".jabba", "jdk") == 0) {
        discovery_scan_optional_root(list, root, JVMAN_SOURCE_JABBA, NULL, 1);
    }
#if defined(__APPLE__)
    if (jvman_path_join3(root, sizeof(root), home, "Library/Java",
                         "JavaVirtualMachines") == 0) {
        discovery_scan_optional_root(list, root, JVMAN_SOURCE_MACOS, NULL, 1);
    }
#endif
}

#if defined(_WIN32)
typedef struct WindowsVendorRoot {
    const char *directory;
    const char *vendor_hint;
} WindowsVendorRoot;

static const WindowsVendorRoot windows_vendor_roots[] = {
    {"Java", "Oracle"},
    {"BellSoft", "BellSoft"},
    {"AdoptOpenJDK", "AdoptOpenJDK"},
    {"Eclipse Adoptium", "Eclipse Adoptium"},
    {"Zulu", "Zulu"},
    {"Microsoft", "Microsoft"},
    {"Semeru", "Semeru"},
    {"Amazon Corretto", "Amazon Corretto"}
};

static void discovery_scan_windows_program_base(JvmanDiscoveryList *list,
                                                const char *base) {
    size_t i;
    if (!base || !*base) return;
    for (i = 0; i < sizeof(windows_vendor_roots) /
                        sizeof(windows_vendor_roots[0]); ++i) {
        discovery_scan_joined_root(
            list, base, windows_vendor_roots[i].directory,
            JVMAN_SOURCE_PROGRAM_FILES, windows_vendor_roots[i].vendor_hint, 1);
    }
}
#endif

#if defined(__APPLE__)
static int discovery_formula_name(const char *name) {
    return discovery_contains(name, "openjdk") ||
           discovery_contains(name, "temurin") ||
           discovery_contains(name, "corretto") ||
           discovery_contains(name, "liberica") ||
           discovery_contains(name, "zulu");
}

static void discovery_scan_homebrew(JvmanDiscoveryList *list, const char *root) {
    char **names = NULL;
    size_t count = 0;
    size_t i;
    discovery_scan_optional_root(list, root, JVMAN_SOURCE_HOMEBREW, NULL, 1);
    if (!platform_is_directory(root) ||
        platform_list_directory(root, &names, &count) != 0) return;
    for (i = 0; i < count; ++i) {
        char formula[JVMAN_PATH_MAX];
        char jdk_bundle[JVMAN_PATH_MAX];
        char home[JVMAN_PATH_MAX];
        if (!discovery_formula_name(names[i]) ||
            jvman_path_join(formula, sizeof(formula), root, names[i]) != 0 ||
            jvman_path_join3(jdk_bundle, sizeof(jdk_bundle), formula, "libexec",
                             "openjdk.jdk") != 0 ||
            jvman_path_join3(home, sizeof(home), jdk_bundle, "Contents", "Home") != 0)
            continue;
        (void)jvman_discovery_add_home(list, home, JVMAN_SOURCE_HOMEBREW, NULL);
    }
    platform_free_directory_list(names, count);
}
#endif
