#include "common.h"
#include "platform.h"
#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NEW_VERSION "4294967295.4294967295.4294967295"

typedef enum MockFailure {
    MOCK_FAIL_NONE = 0,
    MOCK_FAIL_TEMPORARY,
    MOCK_FAIL_METADATA_DOWNLOAD,
    MOCK_FAIL_METADATA_CONTENT,
    MOCK_FAIL_CHECKSUM_DOWNLOAD,
    MOCK_FAIL_CHECKSUM_CONTENT,
    MOCK_FAIL_BINARY_DOWNLOAD,
    MOCK_FAIL_CURRENT,
    MOCK_FAIL_STALE_IMAGE,
    MOCK_FAIL_CURRENT_HASH,
    MOCK_FAIL_HASH,
    MOCK_FAIL_IMAGE,
    MOCK_FAIL_BINARY_VERSION,
    MOCK_FAIL_STAGE,
    MOCK_FAIL_PUBLISH
} MockFailure;

typedef enum MockDownloadKind {
    MOCK_DOWNLOAD_NONE = 0,
    MOCK_DOWNLOAD_METADATA,
    MOCK_DOWNLOAD_CHECKSUM,
    MOCK_DOWNLOAD_BINARY
} MockDownloadKind;

typedef struct MockDownload {
    char path[128];
    MockDownloadKind kind;
    size_t limit;
    int progress;
} MockDownload;

typedef struct MockState {
    MockFailure failure;
    MockDownload downloads[8];
    char removed[12][128];
    size_t download_count;
    size_t removed_count;
    unsigned int temporary_count;
    int current_calls;
    int hash_calls;
    int image_calls;
    int stage_calls;
    int publish_calls;
    int deferred;
    char events[32];
    size_t event_count;
} MockState;

static MockState mock;
static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static void mock_reset(MockFailure failure) {
    memset(&mock, 0, sizeof(mock));
    mock.failure = failure;
}

static void mock_event(char event) {
    if (mock.event_count < sizeof(mock.events)) {
        mock.events[mock.event_count++] = event;
    }
}

static int mock_copy(char *out, size_t out_size, const char *text) {
    size_t length;
    if (!out || out_size == 0 || !text) return -1;
    length = strlen(text);
    if (length + 1u > out_size) return -1;
    memcpy(out, text, length + 1u);
    return 0;
}

static MockDownloadKind mock_url_kind(const char *url) {
    if (!url) return MOCK_DOWNLOAD_NONE;
    if (strcmp(url,
               "https://api.github.com/repos/standtrain/jvman/releases/latest") == 0) {
        return MOCK_DOWNLOAD_METADATA;
    }
    if (strcmp(url,
               "https://github.com/standtrain/jvman/releases/download/v"
               TEST_NEW_VERSION "/SHA256SUMS") == 0) {
        return MOCK_DOWNLOAD_CHECKSUM;
    }
    if (strcmp(url,
               "https://github.com/standtrain/jvman/releases/download/v"
               TEST_NEW_VERSION "/jvman-windows-x86_64.exe") == 0) {
        return MOCK_DOWNLOAD_BINARY;
    }
    return MOCK_DOWNLOAD_NONE;
}

static MockDownload *mock_find_download(const char *path) {
    size_t index;
    if (!path) return NULL;
    for (index = 0; index < mock.download_count; ++index) {
        if (strcmp(mock.downloads[index].path, path) == 0) {
            return &mock.downloads[index];
        }
    }
    return NULL;
}

static int mock_removed(const char *path) {
    size_t index;
    for (index = 0; index < mock.removed_count; ++index) {
        if (strcmp(mock.removed[index], path) == 0) return 1;
    }
    return 0;
}

const char *platform_last_error(void) {
    return "mock platform failure";
}

void platform_clear_error(void) {
}

int platform_monotonic_millis(uint64_t *value_out) {
    if (value_out) *value_out = 0;
    return 0;
}

int platform_https_probe(const char *url, size_t sample_size,
                         unsigned int timeout_seconds) {
    (void)url;
    (void)sample_size;
    (void)timeout_seconds;
    return -1;
}

const char *platform_os_name(void) {
    return "windows";
}

const char *platform_arch_name(void) {
    return "x64";
}

int platform_create_temporary_file(char *out, size_t out_size) {
    char path[128];
    int written;
    if (!out || out_size == 0) return -1;
    if (mock.failure == MOCK_FAIL_TEMPORARY) return -1;
    ++mock.temporary_count;
    written = snprintf(path, sizeof(path), "mock-temp-%u",
                       mock.temporary_count);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    return mock_copy(out, out_size, path);
}

int platform_https_download(const char *url, const char *destination,
                            size_t limit, int show_progress) {
    MockDownloadKind kind = mock_url_kind(url);
    MockDownload *entry;
    if (!destination || kind == MOCK_DOWNLOAD_NONE ||
        mock.download_count >= sizeof(mock.downloads) /
                                   sizeof(mock.downloads[0])) {
        return -1;
    }
    if ((kind == MOCK_DOWNLOAD_METADATA &&
         mock.failure == MOCK_FAIL_METADATA_DOWNLOAD) ||
        (kind == MOCK_DOWNLOAD_CHECKSUM &&
         mock.failure == MOCK_FAIL_CHECKSUM_DOWNLOAD) ||
        (kind == MOCK_DOWNLOAD_BINARY &&
         mock.failure == MOCK_FAIL_BINARY_DOWNLOAD)) {
        return -1;
    }
    entry = &mock.downloads[mock.download_count++];
    if (mock_copy(entry->path, sizeof(entry->path), destination) != 0) {
        return -1;
    }
    entry->kind = kind;
    mock_event(kind == MOCK_DOWNLOAD_METADATA ? 'M' :
               kind == MOCK_DOWNLOAD_CHECKSUM ? 'S' : 'B');
    entry->limit = limit;
    entry->progress = show_progress;
    return 0;
}

int platform_read_file_limited(const char *path, size_t limit,
                               char **data_out, size_t *size_out) {
    static const char metadata[] =
        "{\"tag_name\":\"v" TEST_NEW_VERSION "\",\"draft\":false}";
    static const char metadata_with_nul[] =
        "{\"tag_name\":\"v" TEST_NEW_VERSION "\"}\0trailing";
    static const char checksum[] =
        "1111111111111111111111111111111111111111111111111111111111111111"
        "  jvman-windows-x86_64.exe\n";
    static const char bad_checksum[] = "not-a-checksum\n";
    static const char current_image[] =
        "\x01JVMAN-SELF-VERSION:" JVMAN_VERSION;
    static const char stale_image[] =
        "\x01JVMAN-SELF-VERSION:" TEST_NEW_VERSION;
    static const char binary_image[] =
        "\x01JVMAN-SELF-VERSION:" TEST_NEW_VERSION;
    static const char wrong_binary_image[] =
        "\x01JVMAN-SELF-VERSION:" JVMAN_VERSION;
    MockDownload *entry = mock_find_download(path);
    const char *source;
    size_t size;
    char *copy;
    if (!path || !data_out || !size_out) return -1;
    if (strcmp(path, "mock-jvman.exe") == 0) {
        source = mock.failure == MOCK_FAIL_STALE_IMAGE
                     ? stale_image : current_image;
        size = mock.failure == MOCK_FAIL_STALE_IMAGE
                   ? sizeof(stale_image) : sizeof(current_image);
        mock_event('V');
        copy = (char *)malloc(size + 1u);
        if (!copy) return -1;
        memcpy(copy, source, size);
        copy[size] = '\0';
        *data_out = copy;
        *size_out = size;
        return 0;
    }
    if (!entry || entry->limit != limit) return -1;
    if (entry->kind == MOCK_DOWNLOAD_METADATA) {
        if (mock.failure == MOCK_FAIL_METADATA_CONTENT) {
            source = metadata_with_nul;
            size = sizeof(metadata_with_nul) - 1u;
        } else {
            source = metadata;
            size = sizeof(metadata) - 1u;
        }
    } else if (entry->kind == MOCK_DOWNLOAD_CHECKSUM) {
        source = mock.failure == MOCK_FAIL_CHECKSUM_CONTENT
                     ? bad_checksum : checksum;
        size = strlen(source);
    } else if (entry->kind == MOCK_DOWNLOAD_BINARY) {
        source = mock.failure == MOCK_FAIL_BINARY_VERSION
                     ? wrong_binary_image : binary_image;
        size = mock.failure == MOCK_FAIL_BINARY_VERSION
                   ? sizeof(wrong_binary_image) : sizeof(binary_image);
    } else {
        return -1;
    }
    copy = (char *)malloc(size + 1u);
    if (!copy) return -1;
    memcpy(copy, source, size);
    copy[size] = '\0';
    *data_out = copy;
    *size_out = size;
    return 0;
}

int platform_remove_file(const char *path) {
    if (!path || mock.removed_count >= sizeof(mock.removed) /
                                      sizeof(mock.removed[0]) ||
        mock_copy(mock.removed[mock.removed_count],
                  sizeof(mock.removed[mock.removed_count]), path) != 0) {
        return -1;
    }
    ++mock.removed_count;
    return 0;
}

int platform_current_executable(char *out, size_t out_size) {
    ++mock.current_calls;
    mock_event('C');
    if (mock.failure == MOCK_FAIL_CURRENT) return -1;
    return mock_copy(out, out_size, "mock-jvman.exe");
}

int platform_sha256_file(const char *path, unsigned char digest[32]) {
    MockDownload *entry = mock_find_download(path);
    if (!path || !digest) return -1;
    ++mock.hash_calls;
    if (strcmp(path, "mock-jvman.exe") == 0) {
        mock_event('H');
        if (mock.failure == MOCK_FAIL_CURRENT_HASH) return -1;
        memset(digest, 0x33, 32u);
        return 0;
    }
    if (!entry || entry->kind != MOCK_DOWNLOAD_BINARY) return -1;
    memset(digest, mock.failure == MOCK_FAIL_HASH ? 0x22 : 0x11, 32u);
    return 0;
}

int platform_validate_executable_image(const char *path) {
    MockDownload *entry = mock_find_download(path);
    if (!entry || entry->kind != MOCK_DOWNLOAD_BINARY) return -1;
    ++mock.image_calls;
    return mock.failure == MOCK_FAIL_IMAGE ? -1 : 0;
}

int platform_stage_executable_update(const char *source, const char *target,
                                     char *staged_out, size_t staged_size) {
    MockDownload *entry = mock_find_download(source);
    if (!entry || entry->kind != MOCK_DOWNLOAD_BINARY || !target ||
        strcmp(target, "mock-jvman.exe") != 0) {
        return -1;
    }
    ++mock.stage_calls;
    if (mock.failure == MOCK_FAIL_STAGE) return -1;
    return mock_copy(staged_out, staged_size,
                     "mock-jvman.exe.jvman-update-1.tmp");
}

int platform_publish_executable_update(const char *staged, const char *target,
                                       const char *expected_sha256,
                                       const char *expected_current_sha256,
                                       int *deferred_out) {
    static const char expected[] =
        "1111111111111111111111111111111111111111111111111111111111111111";
    static const char expected_current[] =
        "3333333333333333333333333333333333333333333333333333333333333333";
    if (!staged || strcmp(staged, "mock-jvman.exe.jvman-update-1.tmp") != 0 ||
        !target || strcmp(target, "mock-jvman.exe") != 0 ||
        !expected_sha256 || strcmp(expected_sha256, expected) != 0 ||
        !expected_current_sha256 ||
        strcmp(expected_current_sha256, expected_current) != 0 ||
        !deferred_out) {
        return -1;
    }
    ++mock.publish_calls;
    if (mock.failure == MOCK_FAIL_PUBLISH) return -1;
    *deferred_out = mock.deferred;
    return 0;
}

static void test_no_network_paths(void) {
    char *same_version[] = {
        "jvman", "update", "--check", "--version", JVMAN_VERSION,
        "--source", "github", NULL
    };
    char *duplicate_check[] = {
        "jvman", "update", "--check", "--check", NULL
    };
    char *missing_version[] = {
        "jvman", "update", "--version", "--check", NULL
    };
    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_command(1, JVMAN_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) == 0);
    CHECK(mock.download_count == 0);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_run_cli(7, same_version) == 0);
    CHECK(mock.download_count == 0);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_run_cli(4, duplicate_check) != 0);
    CHECK(mock.download_count == 0);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_run_cli(4, missing_version) != 0);
    CHECK(mock.download_count == 0);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_run_cli(0, NULL) != 0);
    CHECK(mock.download_count == 0);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_command(0, "0.1.0", JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 0);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_command(0, "1.2", JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 0);
}

static void test_latest_check(void) {
    char *explicit_check[] = {
        "jvman", "update", "--check", "--version", TEST_NEW_VERSION,
        "--source", "github", NULL
    };
    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_command(1, NULL, JVMAN_UPDATE_SOURCE_GITHUB) == 0);
    CHECK(mock.download_count == 2);
    CHECK(mock.downloads[0].kind == MOCK_DOWNLOAD_METADATA);
    CHECK(mock.downloads[0].limit == 1024u * 1024u);
    CHECK(mock.downloads[0].progress == 0);
    CHECK(mock.downloads[1].kind == MOCK_DOWNLOAD_CHECKSUM);
    CHECK(mock.downloads[1].limit == 64u * 1024u);
    CHECK(mock.current_calls == 0);
    CHECK(mock.stage_calls == 0);
    CHECK(mock.publish_calls == 0);
    CHECK(mock.removed_count == 2);

    mock_reset(MOCK_FAIL_NONE);
    CHECK(jvman_update_run_cli(7, explicit_check) == 0);
    CHECK(mock.download_count == 1);
    CHECK(mock.downloads[0].kind == MOCK_DOWNLOAD_CHECKSUM);
    CHECK(mock.current_calls == 0);
    CHECK(mock.stage_calls == 0);
    CHECK(mock.publish_calls == 0);
}

static void test_successful_update(void) {
    mock_reset(MOCK_FAIL_NONE);
    mock.deferred = 1;
    CHECK(jvman_update_command(0, "v" TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) == 0);
    CHECK(mock.download_count == 2);
    CHECK(mock.downloads[0].kind == MOCK_DOWNLOAD_CHECKSUM);
    CHECK(mock.downloads[1].kind == MOCK_DOWNLOAD_BINARY);
    CHECK(mock.downloads[1].limit == 64u * 1024u * 1024u);
    CHECK(mock.downloads[1].progress == 1);
    CHECK(mock.event_count >= 5);
    CHECK(mock.events[0] == 'C');
    CHECK(mock.events[1] == 'H');
    CHECK(mock.events[2] == 'V');
    CHECK(mock.events[3] == 'S');
    CHECK(mock.events[4] == 'B');
    CHECK(mock.current_calls == 1);
    CHECK(mock.hash_calls == 2);
    CHECK(mock.image_calls == 1);
    CHECK(mock.stage_calls == 1);
    CHECK(mock.publish_calls == 1);
    CHECK(mock.removed_count == 2);
    CHECK(mock_removed("mock-temp-1"));
    CHECK(mock_removed("mock-temp-2"));
}

static void test_invalid_remote_data(void) {
    mock_reset(MOCK_FAIL_METADATA_DOWNLOAD);
    CHECK(jvman_update_command(1, NULL, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 0);
    CHECK(mock.removed_count == 1);

    mock_reset(MOCK_FAIL_METADATA_CONTENT);
    CHECK(jvman_update_command(1, NULL, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 1);
    CHECK(mock.removed_count == 1);

    mock_reset(MOCK_FAIL_CHECKSUM_CONTENT);
    CHECK(jvman_update_command(1, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 1);
    CHECK(mock.removed_count == 1);

    mock_reset(MOCK_FAIL_CHECKSUM_DOWNLOAD);
    CHECK(jvman_update_command(1, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 0);
    CHECK(mock.removed_count == 1);

    mock_reset(MOCK_FAIL_HASH);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.hash_calls == 2);
    CHECK(mock.image_calls == 0);
    CHECK(mock.stage_calls == 0);
    CHECK(mock.removed_count == 2);
}

static void test_failure_cleanup(void) {
    mock_reset(MOCK_FAIL_TEMPORARY);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.download_count == 0);
    CHECK(mock.removed_count == 0);

    mock_reset(MOCK_FAIL_BINARY_DOWNLOAD);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.removed_count == 2);

    mock_reset(MOCK_FAIL_IMAGE);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.image_calls == 1);
    CHECK(mock.stage_calls == 0);
    CHECK(mock.removed_count == 2);

    mock_reset(MOCK_FAIL_BINARY_VERSION);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.image_calls == 1);
    CHECK(mock.stage_calls == 0);
    CHECK(mock.removed_count == 2);

    mock_reset(MOCK_FAIL_STAGE);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.stage_calls == 1);
    CHECK(mock.publish_calls == 0);
    CHECK(mock.removed_count == 2);

    mock_reset(MOCK_FAIL_PUBLISH);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.publish_calls == 1);
    CHECK(mock.removed_count == 3);
    CHECK(mock_removed("mock-jvman.exe.jvman-update-1.tmp"));

    mock_reset(MOCK_FAIL_CURRENT);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.current_calls == 1);
    CHECK(mock.removed_count == 0);

    mock_reset(MOCK_FAIL_CURRENT_HASH);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.hash_calls == 1);
    CHECK(mock.removed_count == 0);

    mock_reset(MOCK_FAIL_STALE_IMAGE);
    CHECK(jvman_update_command(0, TEST_NEW_VERSION, JVMAN_UPDATE_SOURCE_GITHUB) != 0);
    CHECK(mock.current_calls == 1);
    CHECK(mock.hash_calls == 1);
    CHECK(mock.download_count == 0);
}

int main(void) {
    test_no_network_paths();
    test_latest_check();
    test_successful_update();
    test_invalid_remote_data();
    test_failure_cleanup();
    if (failures) {
        fprintf(stderr, "%d update flow test(s) failed\n", failures);
        return 1;
    }
    puts("All update flow tests passed.");
    return 0;
}
