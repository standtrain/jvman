#include "update.h"
#include "platform.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static int write_bytes(const char *path, const unsigned char *data,
                       size_t size) {
    FILE *file;
    size_t written;
    int close_result;
    if (!path || !data || size == 0) return -1;
    file = fopen(path, "wb");
    if (!file) return -1;
    written = fwrite(data, 1, size, file);
    close_result = fclose(file);
    return written == size && close_result == 0 ? 0 : -1;
}

static void check_version_valid(const char *text) {
    JvmanUpdateVersion version;
    CHECK(jvman_update_parse_version(text, &version) == 0);
}

static void check_version_invalid(const char *text) {
    JvmanUpdateVersion version;
    CHECK(jvman_update_parse_version(text, &version) != 0);
}

static void check_version_comparison(const char *left, const char *right,
                                     int expected) {
    int comparison = 99;
    CHECK(jvman_update_compare_versions(left, right, &comparison) == 0);
    if (comparison < 0) comparison = -1;
    if (comparison > 0) comparison = 1;
    CHECK(comparison == expected);
}

static int parse_release_json_text(const char *text, char *version,
                                   size_t version_size) {
    return jvman_update_parse_release_json(
        text, text ? strlen(text) : 0, version, version_size);
}

static void test_versions(void) {
    static const char *const invalid[] = {
        "", "v", "vv1.2.3", "1", "1.2", "1.2.3.4",
        "01.2.3", "1.02.3", "1.2.03", "+1.2.3", "-1.2.3",
        "1.2.3-alpha", "1.2.3+build", "1.2.3 ", " 1.2.3",
        "1/2/3", "184467440737095516160.2.3"
    };
    int comparison = 0;
    size_t i;

    check_version_valid("0.0.0");
    check_version_valid("0.2.0");
    check_version_valid("v12.345.6789");
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        check_version_invalid(invalid[i]);
    }

    CHECK(jvman_update_parse_version(NULL, NULL) != 0);
    CHECK(jvman_update_parse_version("1.2.3", NULL) != 0);

    check_version_comparison("0.2.0", "v0.2.0", 0);
    check_version_comparison("v0.3.0", "0.2.99", 1);
    check_version_comparison("1.10.0", "1.2.99", 1);
    check_version_comparison("10.0.0", "11.0.0", -1);
    check_version_comparison("3.4.5", "3.5.0", -1);
    check_version_comparison("3.4.6", "3.4.5", 1);

    CHECK(jvman_update_compare_versions("1.2", "1.2.3", &comparison) != 0);
    CHECK(jvman_update_compare_versions("1.2.3", "1.2.3", NULL) != 0);
    CHECK(jvman_update_compare_versions(NULL, "1.2.3", &comparison) != 0);
}

static void test_release_json(void) {
    char version[64];
    char guarded[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', '\0'};

    CHECK(parse_release_json_text(
              "{\"url\":\"https://api.github.com/repos/standtrain/jvman/releases/1\","
              "\"tag_name\":\"v0.3.0\",\"draft\":false,\"assets\":[]}",
              version, sizeof(version)) == 0);
    CHECK(strcmp(version, "0.3.0") == 0);

    CHECK(parse_release_json_text(
              " { \"name\" : \"release\", \"tag_name\" : \"12.34.56\" } ",
              version, sizeof(version)) == 0);
    CHECK(strcmp(version, "12.34.56") == 0);

    /* A nested or string-embedded tag must not replace the top-level tag. */
    CHECK(parse_release_json_text(
              "{\"body\":\"\\\"tag_name\\\":\\\"v9.9.9\\\"\","
              "\"nested\":{\"tag_name\":\"v8.8.8\"},"
              "\"tag_name\":\"v1.2.3\"}",
              version, sizeof(version)) == 0);
    CHECK(strcmp(version, "1.2.3") == 0);

    CHECK(parse_release_json_text("{}", version,
                                           sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag_name\":\"v01.2.3\"}", version,
              sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag_name\":\"v1.2.3-beta\"}", version,
              sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag_name\":\"v1.2.3\\u0000suffix\"}", version,
              sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag\\u0000_name\":\"v1.2.3\"}", version,
              sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag_name\":123}", version, sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag_name\":\"v1.2.3\",\"tag_name\":\"v2.0.0\"}",
              version, sizeof(version)) != 0);
    CHECK(parse_release_json_text(
              "{\"tag_name\":\"v123.456.789\"}", guarded, 4) != 0);
    CHECK(guarded[7] == '\0');
    CHECK(parse_release_json_text(NULL, version,
                                           sizeof(version)) != 0);
    CHECK(parse_release_json_text("{\"tag_name\":\"v1.2.3\"}",
                                           NULL, 0) != 0);

    {
        static const char embedded_nul[] =
            "{\"tag_name\":\"v1.2.3\"}\0{\"tag_name\":\"v9.9.9\"}";
        CHECK(jvman_update_parse_release_json(
                  embedded_nul, sizeof(embedded_nul) - 1u,
                  version, sizeof(version)) != 0);
    }
}

static void test_checksums(void) {
    static const char hash_a[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const char hash_b_upper[] =
        "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";
    static const char asset[] = "jvman-windows-x86_64.exe";
    char text[1024];
    char digest[65];
    char embedded_nul[256];
    char overlong[9000];
    int length;

    length = snprintf(text, sizeof(text),
                      "%s  jvman-linux-x86_64\n%s *%s\r\n",
                      hash_a, hash_b_upper, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) == 0);
    CHECK(strcmp(digest,
                 "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") == 0);

    length = snprintf(text, sizeof(text), "%s  %s\n", hash_a, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) == 0);
    CHECK(strcmp(digest, hash_a) == 0);

    length = snprintf(text, sizeof(text), "%s  %s\n%s *%s\n",
                      hash_a, asset, hash_b_upper, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(text, sizeof(text), "%s  %s.extra\n", hash_a, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(text, sizeof(text), "%.*s  %s\n",
                      63, hash_a, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(text, sizeof(text), "z%s  %s\n", hash_a + 1, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(text, sizeof(text), " %s  %s\n", hash_a, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(text, sizeof(text), "%s %s\n", hash_a, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(text, sizeof(text), "%s  %s\nmalformed\n",
                      hash_a, asset);
    CHECK(length > 0 && (size_t)length < sizeof(text));
    CHECK(jvman_update_parse_checksum(text, (size_t)length, asset, digest) != 0);

    length = snprintf(embedded_nul, sizeof(embedded_nul), "%s  %s", hash_a,
                      asset);
    CHECK(length > 0 && (size_t)length + 2 < sizeof(embedded_nul));
    embedded_nul[length] = '\0';
    embedded_nul[length + 1] = '\n';
    CHECK(jvman_update_parse_checksum(embedded_nul, (size_t)length + 2,
                                      asset, digest) != 0);

    memset(overlong, 'x', sizeof(overlong));
    overlong[sizeof(overlong) - 1] = '\n';
    CHECK(jvman_update_parse_checksum(overlong, sizeof(overlong), asset,
                                      digest) != 0);

    CHECK(jvman_update_parse_checksum("", 0, asset, digest) != 0);
    CHECK(jvman_update_parse_checksum(NULL, 1, asset, digest) != 0);
    CHECK(jvman_update_parse_checksum(text, strlen(text), "../jvman.exe",
                                      digest) != 0);
    CHECK(jvman_update_parse_checksum(text, strlen(text), "bad\nasset",
                                      digest) != 0);
    CHECK(jvman_update_parse_checksum(text, strlen(text), asset, NULL) != 0);
}

static void check_platform_asset(const char *os, const char *arch,
                                 const char *expected) {
    const char *asset = jvman_update_asset_for_platform(os, arch);
    CHECK(asset != NULL);
    if (asset) CHECK(strcmp(asset, expected) == 0);
}

static void test_release_urls(void) {
    static const char *const allowed_assets[] = {
        "jvman-windows-x86_64.exe",
        "jvman-setup-windows-x86_64.exe",
        "jvman-linux-x86_64",
        "jvman-macos-x86_64",
        "jvman-macos-aarch64",
        "SHA256SUMS"
    };
    static const char *const rejected_assets[] = {
        "", "jvman.exe", "JVMAN-linux-x86_64",
        "../jvman-linux-x86_64", "jvman-linux-x86_64?raw=1",
        "jvman-linux-x86_64/other", "jvman-linux-x86_64\nother"
    };
    char url[512];
    char expected[512];
    size_t i;

    for (i = 0; i < sizeof(allowed_assets) / sizeof(allowed_assets[0]); ++i) {
        CHECK(jvman_update_build_release_url("0.3.0", allowed_assets[i],
                                             url, sizeof(url)) == 0);
        CHECK(snprintf(expected, sizeof(expected),
                       "https://github.com/standtrain/jvman/releases/download/v0.3.0/%s",
                       allowed_assets[i]) > 0);
        CHECK(strcmp(url, expected) == 0);
    }

    CHECK(jvman_update_build_release_url("v0.3.0", "SHA256SUMS", url,
                                         sizeof(url)) == 0);
    CHECK(strcmp(url,
                 "https://github.com/standtrain/jvman/releases/download/v0.3.0/SHA256SUMS") == 0);

    for (i = 0; i < sizeof(rejected_assets) / sizeof(rejected_assets[0]); ++i) {
        CHECK(jvman_update_build_release_url("0.3.0", rejected_assets[i],
                                             url, sizeof(url)) != 0);
    }
    CHECK(jvman_update_build_release_url("1.2.3/other", "SHA256SUMS", url,
                                         sizeof(url)) != 0);
    CHECK(jvman_update_build_release_url("1.2.3?x=1", "SHA256SUMS", url,
                                         sizeof(url)) != 0);
    CHECK(jvman_update_build_release_url("01.2.3", "SHA256SUMS", url,
                                         sizeof(url)) != 0);
    CHECK(jvman_update_build_release_url("1.2.3", "SHA256SUMS", url, 8) != 0);
    CHECK(jvman_update_build_release_url(NULL, "SHA256SUMS", url,
                                         sizeof(url)) != 0);
    CHECK(jvman_update_build_release_url("1.2.3", NULL, url,
                                         sizeof(url)) != 0);
    CHECK(jvman_update_build_release_url("1.2.3", "SHA256SUMS", NULL, 0) != 0);

    check_platform_asset("windows", "x64", "jvman-windows-x86_64.exe");
    check_platform_asset("linux", "x64", "jvman-linux-x86_64");
    check_platform_asset("alpine-linux", "x64", "jvman-linux-x86_64");
    check_platform_asset("mac", "x64", "jvman-macos-x86_64");
    check_platform_asset("mac", "aarch64", "jvman-macos-aarch64");
    CHECK(jvman_update_asset_for_platform("windows", "aarch64") == NULL);
    CHECK(jvman_update_asset_for_platform("linux", "aarch64") == NULL);
    CHECK(jvman_update_asset_for_platform(NULL, "x64") == NULL);
}

static void test_executable_images(void) {
    char current[JVMAN_PATH_MAX];
    char temporary[JVMAN_PATH_MAX] = {0};
    unsigned char image[70];
    size_t image_size = 64u;
    CHECK(platform_current_executable(current, sizeof(current)) == 0);
    CHECK(platform_validate_executable_image(current) == 0);
    CHECK(platform_create_temporary_file(temporary, sizeof(temporary)) == 0);
    if (!temporary[0]) return;

    memset(image, 0, sizeof(image));
    CHECK(write_bytes(temporary, image, 64u) == 0);
    CHECK(platform_validate_executable_image(temporary) != 0);

    memset(image, 0, sizeof(image));
#if defined(_WIN32)
    image[0] = 'M';
    image[1] = 'Z';
    image[60] = 64u;
    memcpy(image + 64u, "PE\0\0", 4u);
    image_size = 70u;
#elif defined(__linux__)
    image[0] = 0x7fu;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2u;
    image[5] = 1u;
#elif defined(__APPLE__)
    image[0] = 0xcfu;
    image[1] = 0xfau;
    image[2] = 0xedu;
    image[3] = 0xfeu;
#endif
    CHECK(write_bytes(temporary, image, image_size) == 0);
    CHECK(platform_validate_executable_image(temporary) != 0);
    CHECK(platform_remove_file(temporary) == 0);
}

int main(void) {
    test_versions();
    test_release_json();
    test_checksums();
    test_release_urls();
    test_executable_images();
    if (failures) {
        fprintf(stderr, "%d update test(s) failed\n", failures);
        return 1;
    }
    puts("All update unit tests passed.");
    return 0;
}
