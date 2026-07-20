#include "platform.h"

#include <stdio.h>
#include <string.h>

static void digest_to_hex(const unsigned char digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < 32u; ++index) {
        out[index * 2u] = hex[digest[index] >> 4];
        out[index * 2u + 1u] = hex[digest[index] & 0x0fu];
    }
    out[64] = '\0';
}

int main(int argc, char **argv) {
    char source[JVMAN_PATH_MAX];
    char current[JVMAN_PATH_MAX];
    char staged[JVMAN_PATH_MAX] = {0};
    char source_checksum[65];
    char current_checksum[65];
    unsigned char digest[32];
    int helper_handled = 0;
    int helper_result = platform_handle_update_helper(
        argc, argv, &helper_handled);
    int deferred = 0;
    int reject_stage = 0;
    int reject_current = 0;
    int remove_stage = 0;
    int publish_result;
    const char *source_argument;
    if (helper_handled) return helper_result;
    if (helper_result != 0) return helper_result;
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) return 0;
    if (argc == 2) {
        source_argument = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--reject-stage") == 0) {
        reject_stage = 1;
        source_argument = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--reject-current") == 0) {
        reject_current = 1;
        source_argument = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--remove-stage") == 0) {
        remove_stage = 1;
        source_argument = argv[2];
    } else {
        fprintf(stderr,
                "usage: test_update_helper [--reject-stage|--reject-current|--remove-stage] <replacement-executable>\n");
        return 2;
    }
    if (!source_argument || strlen(source_argument) >= JVMAN_PATH_MAX ||
        platform_absolute_path(source_argument, source, sizeof(source)) != 0 ||
        platform_current_executable(current, sizeof(current)) != 0 ||
        strcmp(source, current) == 0 ||
        platform_validate_executable_image(source) != 0 ||
        platform_sha256_file(source, digest) != 0) {
        fprintf(stderr, "cannot validate helper test input: %s\n",
                platform_last_error());
        return 3;
    }
    digest_to_hex(digest, source_checksum);
    if (platform_sha256_file(current, digest) != 0) {
        fprintf(stderr, "cannot fingerprint helper test target: %s\n",
                platform_last_error());
        return 3;
    }
    digest_to_hex(digest, current_checksum);
    if (platform_stage_executable_update(source, current, staged,
                                         sizeof(staged)) != 0) {
        fprintf(stderr, "cannot stage helper test update: %s\n",
                platform_last_error());
        return 4;
    }
    if (reject_stage) source_checksum[0] = source_checksum[0] == '0' ? '1' : '0';
    if (reject_current) {
        current_checksum[0] = current_checksum[0] == '0' ? '1' : '0';
    }
    publish_result = platform_publish_executable_update(
        staged, current, source_checksum, current_checksum, &deferred);
    if (reject_stage || reject_current) {
        if (publish_result == 0) {
            fprintf(stderr, "helper test accepted an incorrect checksum\n");
            return 5;
        }
        (void)platform_remove_file(staged);
        return 0;
    }
    if (publish_result != 0
#if defined(_WIN32)
        || !deferred
#else
        || deferred
#endif
    ) {
        (void)platform_remove_file(staged);
        fprintf(stderr, "cannot schedule helper test update: %s\n",
                platform_last_error());
        return 4;
    }
    if (remove_stage) {
#if defined(_WIN32)
        if (platform_remove_file(staged) != 0) {
            fprintf(stderr, "cannot remove the staged helper test update\n");
            return 6;
        }
#endif
        return 0;
    }
    return 0;
}
