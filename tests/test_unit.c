#include "discovery.h"
#include "platform.h"
#include "sha256.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static char fixture_root[JVMAN_PATH_MAX];

#define TEST_RELEASE_LIMIT (64u * 1024u)

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static int fixture_path(char *out, size_t out_size, const char *name) {
    return jvman_path_join(out, out_size, fixture_root, name);
}

static int write_bytes(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    if (size && fwrite(data, 1, size, file) != size) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int write_text(const char *path, const char *text) {
    return write_bytes(path, text, strlen(text));
}

static int make_java_home(const char *home, int has_java, int has_javac,
                          const char *release_text) {
    char bin[JVMAN_PATH_MAX];
    char path[JVMAN_PATH_MAX];
    if (platform_mkdirs(home) != 0 ||
        jvman_path_join(bin, sizeof(bin), home, "bin") != 0 ||
        platform_mkdirs(bin) != 0) {
        return -1;
    }
    if (has_java) {
        if (jvman_path_join(path, sizeof(path), bin, JVMAN_JAVA_EXE) != 0 ||
            write_bytes(path, "", 0) != 0) {
            return -1;
        }
    }
    if (has_javac) {
        if (jvman_path_join(path, sizeof(path), bin, JVMAN_JAVAC_EXE) != 0 ||
            write_bytes(path, "", 0) != 0) {
            return -1;
        }
    }
    if (release_text) {
        if (jvman_path_join(path, sizeof(path), home, "release") != 0 ||
            write_text(path, release_text) != 0) {
            return -1;
        }
    }
    return 0;
}

static int create_fixture_root(void) {
    const char *temporary;
#if defined(_WIN32)
    temporary = getenv("TEMP");
#else
    temporary = getenv("TMPDIR");
#endif
    if (!temporary || !*temporary) temporary = ".";
    if (snprintf(fixture_root, sizeof(fixture_root),
                 "%s%cjvman-unit-%lu", temporary, JVMAN_DIR_SEP,
                 platform_process_id()) >= (int)sizeof(fixture_root)) {
        return -1;
    }
    if (platform_remove_tree(fixture_root) != 0) return -1;
    return platform_mkdirs(fixture_root);
}

static void test_names(void) {
    int major = 0;
    CHECK(jvman_valid_name("21"));
    CHECK(jvman_valid_name("temurin-21.0.8+9"));
    CHECK(!jvman_valid_name("../21"));
    CHECK(!jvman_valid_name("bad name"));
    CHECK(!jvman_valid_name(""));
#if defined(_WIN32)
    CHECK(jvman_name_equal("Temurin", "temurin"));
#else
    CHECK(!jvman_name_equal("Temurin", "temurin"));
#endif
    CHECK(jvman_parse_major("8", &major) == 0 && major == 8);
    CHECK(jvman_parse_major("21", &major) == 0 && major == 21);
    CHECK(jvman_parse_major("21.0.1", &major) != 0);
    CHECK(jvman_parse_major("7", &major) != 0);
}

static void test_paths(void) {
    char path[128];
    CHECK(jvman_path_join(path, sizeof(path), "root", "child") == 0);
    CHECK(strcmp(path, "root" JVMAN_DIR_SEP_STR "child") == 0);
    CHECK(jvman_path_join3(path, sizeof(path), "root", "a", "b") == 0);
#if defined(_WIN32)
    CHECK(jvman_path_equal("root/a/", "root\\a"));
#else
    CHECK(jvman_path_equal("root/a/", "root/a"));
    CHECK(!jvman_path_equal("root/a", "root\\a"));
#endif
    CHECK(jvman_path_is_within("root/a", "root/a/b"));
    CHECK(!jvman_path_is_within("root/a", "root/ab"));
}

static void test_json(void) {
    const char json[] =
        "[{\"binary\":{\"package\":{\"checksum\":\"abcd\","
        "\"link\":\"https:\\/\\/example.test\\/jdk.zip\"}},"
        "\"version\":{\"openjdk_version\":\"21.0.8+9-LTS\"}}]";
    char value[128];
    CHECK(jvman_json_string_field(json, "\"package\"", "checksum", value,
                                  sizeof(value)) == 0);
    CHECK(strcmp(value, "abcd") == 0);
    CHECK(jvman_json_string_field(json, "\"package\"", "link", value,
                                  sizeof(value)) == 0);
    CHECK(strcmp(value, "https://example.test/jdk.zip") == 0);
    CHECK(jvman_json_string_field(json, "\"version\"", "openjdk_version", value,
                                  sizeof(value)) == 0);
    CHECK(strcmp(value, "21.0.8+9-LTS") == 0);
    CHECK(jvman_json_string_field(
              "[{\"package\":{\"link\":\"https://first\"}},"
              "{\"package\":{\"checksum\":\"second\"}}]",
              "\"package\"", "checksum", value, sizeof(value)) != 0);
}

static void test_sha256(void) {
    static const unsigned char expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    unsigned char digest[32];
    JvmanSha256 context;
    jvman_sha256_init(&context);
    jvman_sha256_update(&context, "abc", 3);
    jvman_sha256_final(&context, digest);
    CHECK(memcmp(digest, expected, sizeof(expected)) == 0);
    CHECK(jvman_hex_equal(digest, sizeof(digest),
                          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(!jvman_hex_equal(digest, sizeof(digest),
                           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae"));
}

static void test_release_parsing(void) {
    char path[JVMAN_PATH_MAX];
    JvmanJavaRelease release;
    FILE *file;
    size_t i;

    CHECK(fixture_path(path, sizeof(path), "release-java8") == 0);
    CHECK(write_text(path,
                     "JAVA_VERSION=\"1.8.0_131\"\r\n"
                     "OS_ARCH=\"amd64\"\r\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) == 0);
    CHECK(strcmp(release.version, "1.8.0_131") == 0);
    CHECK(strcmp(release.implementor, "") == 0);
    CHECK(strcmp(release.arch, "amd64") == 0);

    CHECK(fixture_path(path, sizeof(path), "release-modern") == 0);
    CHECK(write_text(path,
                     "JAVA_VERSION = \"23.0.1+11-LTS\"\n"
                     "IMPLEMENTOR=\"Eclipse \\\"Adoptium\\\"\\\\Runtime\"\n"
                     "OS_ARCH=\"x86_64\"\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) == 0);
    CHECK(strcmp(release.version, "23.0.1+11-LTS") == 0);
    CHECK(strcmp(release.implementor,
                 "Eclipse \"Adoptium\"\\Runtime") == 0);
    CHECK(strcmp(release.arch, "x86_64") == 0);

    CHECK(fixture_path(path, sizeof(path), "release-malformed") == 0);
    CHECK(write_text(path, "JAVA_VERSION=\"21\nIMPLEMENTOR=\"Oracle\"\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) != 0);

    CHECK(fixture_path(path, sizeof(path), "release-no-version") == 0);
    CHECK(write_text(path, "IMPLEMENTOR=\"Oracle Corporation\"\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) != 0);

    CHECK(fixture_path(path, sizeof(path), "release-garbage-version") == 0);
    CHECK(write_text(path, "JAVA_VERSION=\"garbage\"\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) != 0);

    CHECK(fixture_path(path, sizeof(path), "release-java7") == 0);
    CHECK(write_text(path, "JAVA_VERSION=\"1.7.0_80\"\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) != 0);

    CHECK(fixture_path(path, sizeof(path), "release-control-character") == 0);
    CHECK(write_text(path, "JAVA_VERSION=\"21\\nforged\"\n") == 0);
    CHECK(jvman_discovery_parse_release(path, &release) != 0);

    CHECK(fixture_path(path, sizeof(path), "release-at-limit") == 0);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        static const char prefix[] = "JAVA_VERSION=\"21\"\n";
        CHECK(fwrite(prefix, 1, sizeof(prefix) - 1, file) == sizeof(prefix) - 1);
        for (i = sizeof(prefix) - 1; i < TEST_RELEASE_LIMIT; ++i) {
            CHECK(fputc('x', file) != EOF);
        }
        CHECK(fclose(file) == 0);
        CHECK(jvman_discovery_parse_release(path, &release) == 0);
    }

    CHECK(fixture_path(path, sizeof(path), "release-too-large") == 0);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        static const char prefix[] = "JAVA_VERSION=\"21\"\n";
        CHECK(fwrite(prefix, 1, sizeof(prefix) - 1, file) == sizeof(prefix) - 1);
        for (i = sizeof(prefix) - 1; i <= TEST_RELEASE_LIMIT; ++i) {
            CHECK(fputc('x', file) != EOF);
        }
        CHECK(fclose(file) == 0);
        CHECK(jvman_discovery_parse_release(path, &release) != 0);
    }
}

static void test_discovery_normalization(void) {
    struct VendorCase {
        const char *implementor;
        const char *hint;
        const char *expected;
    } cases[] = {
        {"Oracle Corporation", "", "oracle"},
        {"Eclipse Adoptium", "", "temurin"},
        {"Amazon.com Inc.", "", "corretto"},
        {"Azul Systems, Inc.", "", "zulu"},
        {"BellSoft", "", "liberica"},
        {"Microsoft", "", "microsoft"},
        {"IBM Semeru Runtime", "", "semeru"},
        {"Oracle Corporation", "C:/Java/BellSoft/liberica-21", "oracle"},
        {"", "C:/Java/BellSoft/liberica-21", "liberica"},
        {"Example JVM", "C:/custom/runtime", "unknown"}
    };
    char value[160];
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(jvman_discovery_normalize_vendor(
                  cases[i].implementor, cases[i].hint,
                  value, sizeof(value)) == 0);
        CHECK(strcmp(value, cases[i].expected) == 0);
    }
    CHECK(jvman_discovery_normalize_version("1.8.0_131", value,
                                             sizeof(value)) == 0);
    CHECK(strcmp(value, "8u131") == 0);
    CHECK(jvman_discovery_normalize_version("23.0.1+11-LTS", value,
                                             sizeof(value)) == 0);
    CHECK(strcmp(value, "23.0.1+11-LTS") == 0);
    CHECK(jvman_discovery_normalize_version("21 ea/3", value,
                                             sizeof(value)) == 0);
    CHECK(strcmp(value, "21-ea-3") == 0);
    CHECK(jvman_discovery_suggest_name("oracle", "1.8.0_131", value,
                                       sizeof(value)) == 0);
    CHECK(strcmp(value, "oracle-8u131") == 0);
}

static void test_discovery_classification(void) {
    static const char modern_release[] =
        "JAVA_VERSION=\"21.0.8+9-LTS\"\n"
        "IMPLEMENTOR=\"Eclipse Adoptium\"\n"
        "OS_ARCH=\"x86_64\"\n";
    char jdk[JVMAN_PATH_MAX];
    char jre[JVMAN_PATH_MAX];
    char unknown_jre[JVMAN_PATH_MAX];
    char invalid[JVMAN_PATH_MAX];
    char resolved[JVMAN_PATH_MAX];
    JvmanDiscoveryCandidate candidate;

    CHECK(fixture_path(jdk, sizeof(jdk), "classification-jdk") == 0);
    CHECK(fixture_path(jre, sizeof(jre), "classification-jre") == 0);
    CHECK(fixture_path(unknown_jre, sizeof(unknown_jre),
                       "classification-unknown-jre") == 0);
    CHECK(fixture_path(invalid, sizeof(invalid), "classification-invalid") == 0);
    CHECK(make_java_home(jdk, 1, 1, modern_release) == 0);
    CHECK(make_java_home(jre, 1, 0,
                         "JAVA_VERSION=\"17.0.12\"\n"
                         "IMPLEMENTOR=\"Amazon Corretto\"\n") == 0);
    CHECK(make_java_home(unknown_jre, 1, 0, NULL) == 0);
    CHECK(make_java_home(invalid, 1, 1, "JAVA_VERSION=\"broken\n") == 0);

    CHECK(jvman_discovery_probe_home(jdk, JVMAN_SOURCE_EXPLICIT, NULL,
                                     &candidate) == 0);
    CHECK(candidate.type == JVMAN_DISCOVERY_JDK);
    CHECK(candidate.release_valid);
    CHECK(strcmp(candidate.vendor, "temurin") == 0);
    CHECK(strcmp(candidate.suggested_name, "temurin-21.0.8+9-LTS") == 0);
    CHECK(jvman_discovery_find_jdk_home(jdk, resolved, sizeof(resolved)) == 0);
    CHECK(jvman_path_equal(candidate.home, resolved));

    CHECK(jvman_discovery_probe_home(jre, JVMAN_SOURCE_EXPLICIT, NULL,
                                     &candidate) == 0);
    CHECK(candidate.type == JVMAN_DISCOVERY_JRE);
    CHECK(candidate.release_valid);
    CHECK(strcmp(candidate.vendor, "corretto") == 0);
    CHECK(jvman_discovery_find_jdk_home(jre, resolved, sizeof(resolved)) != 0);

    CHECK(jvman_discovery_probe_home(unknown_jre, JVMAN_SOURCE_EXPLICIT, NULL,
                                     &candidate) == 0);
    CHECK(candidate.type == JVMAN_DISCOVERY_JRE);
    CHECK(!candidate.release_valid);
    CHECK(strcmp(candidate.version, "unknown") == 0);

    CHECK(jvman_discovery_probe_home(invalid, JVMAN_SOURCE_EXPLICIT, NULL,
                                     &candidate) == 0);
    CHECK(candidate.type == JVMAN_DISCOVERY_INVALID);
    CHECK(!candidate.release_valid);
    CHECK(jvman_discovery_find_jdk_home(invalid, resolved, sizeof(resolved)) != 0);
    CHECK(strcmp(jvman_discovery_type_name(JVMAN_DISCOVERY_JDK), "JDK") == 0);
    CHECK(strcmp(jvman_discovery_type_name(JVMAN_DISCOVERY_JRE), "JRE") == 0);
    CHECK(strcmp(jvman_discovery_type_name(JVMAN_DISCOVERY_INVALID),
                 "INVALID") == 0);
}

typedef struct MockRegistryContext {
    const char *home;
    const char *vendor_hint;
} MockRegistryContext;

static int mock_registry_enumerator(JvmanDiscoveryPathVisitor visitor,
                                    void *visitor_context,
                                    void *platform_context) {
    const MockRegistryContext *mock =
        (const MockRegistryContext *)platform_context;
    return visitor(mock->home, mock->vendor_hint, visitor_context);
}

static void test_discovery_sources_and_conflicts(void) {
    static const char merged_release[] =
        "JAVA_VERSION=\"17.0.13\"\n"
        "IMPLEMENTOR=\"Oracle Corporation\"\n";
    static const char conflict_release[] =
        "JAVA_VERSION=\"21.0.9\"\n"
        "IMPLEMENTOR=\"Oracle Corporation\"\n";
    char merged[JVMAN_PATH_MAX];
    char merged_bin[JVMAN_PATH_MAX];
    char path_value[JVMAN_PATH_MAX + 4];
    char conflict_a[JVMAN_PATH_MAX];
    char conflict_b[JVMAN_PATH_MAX];
    char canonical_a[JVMAN_PATH_MAX];
    char sources[160];
    JvmanDiscoveryOptions options;
    JvmanDiscoveryList list;
    MockRegistryContext registry;

    CHECK(fixture_path(merged, sizeof(merged), "merged jdk") == 0);
    CHECK(make_java_home(merged, 1, 1, merged_release) == 0);
    CHECK(jvman_path_join(merged_bin, sizeof(merged_bin), merged, "bin") == 0);
#if defined(_WIN32)
    CHECK(snprintf(path_value, sizeof(path_value), "\"%s\"", merged_bin) <
          (int)sizeof(path_value));
#else
    CHECK(snprintf(path_value, sizeof(path_value), "%s", merged_bin) <
          (int)sizeof(path_value));
#endif
    registry.home = merged;
    registry.vendor_hint = "Oracle";
    jvman_discovery_options_init(&options);
    options.java_home = merged;
    options.path = path_value;
    options.user_home = "";
    options.use_environment = 0;
    options.scan_common_roots = 0;
    options.registry_enumerator = mock_registry_enumerator;
    options.registry_context = &registry;
    jvman_discovery_list_init(&list);
    CHECK(jvman_discovery_scan(&list, &options) == 0);
    CHECK(list.count == 1);
    if (list.count == 1) {
        CHECK(list.items[0].type == JVMAN_DISCOVERY_JDK);
        CHECK((list.items[0].sources & JVMAN_SOURCE_JAVA_HOME) != 0);
        CHECK((list.items[0].sources & JVMAN_SOURCE_PATH) != 0);
        CHECK((list.items[0].sources & JVMAN_SOURCE_REGISTRY) != 0);
        CHECK(jvman_discovery_format_sources(list.items[0].sources,
                                             sources, sizeof(sources)) == 0);
        CHECK(strcmp(sources, "JAVA_HOME,PATH,registry") == 0);
    }
    jvman_discovery_list_free(&list);

    CHECK(fixture_path(conflict_a, sizeof(conflict_a), "conflict-a") == 0);
    CHECK(fixture_path(conflict_b, sizeof(conflict_b), "conflict-b") == 0);
    CHECK(make_java_home(conflict_a, 1, 1, conflict_release) == 0);
    CHECK(make_java_home(conflict_b, 1, 1, conflict_release) == 0);
    CHECK(platform_absolute_path(conflict_a, canonical_a,
                                 sizeof(canonical_a)) == 0);
    jvman_discovery_list_init(&list);
    CHECK(jvman_discovery_add_home(&list, conflict_b,
                                   JVMAN_SOURCE_EXPLICIT, NULL) == 0);
    CHECK(jvman_discovery_add_home(&list, conflict_a,
                                   JVMAN_SOURCE_EXPLICIT, NULL) == 0);
    CHECK(list.count == 2);
    jvman_discovery_sort(&list);
    if (list.count == 2) {
        CHECK(jvman_path_equal(list.items[0].home, canonical_a));
        CHECK(strcmp(list.items[0].suggested_name, "oracle-21.0.9") == 0);
        CHECK(strcmp(list.items[1].suggested_name, "oracle-21.0.9") == 0);
    }
    jvman_discovery_list_free(&list);
}

int main(void) {
    if (create_fixture_root() != 0) {
        fprintf(stderr, "Cannot create unit-test fixture directory: %s\n",
                platform_last_error());
        return 1;
    }
    test_names();
    test_paths();
    test_json();
    test_sha256();
    test_release_parsing();
    test_discovery_normalization();
    test_discovery_classification();
    test_discovery_sources_and_conflicts();
    if (platform_remove_tree(fixture_root) != 0) {
        fprintf(stderr, "Cannot remove unit-test fixture directory: %s\n",
                platform_last_error());
        ++failures;
    }
    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    puts("All unit tests passed.");
    return 0;
}
