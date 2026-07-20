#include "package.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

int main(void) {
    unsigned char footer[JVMAN_SETUP_FOOTER_SIZE];
    unsigned char digest[32];
    unsigned char decoded[32];
    uint64_t size = 0;
    unsigned int i;
    for (i = 0; i < sizeof(digest); ++i) digest[i] = (unsigned char)(i * 7u);

    jvman_setup_footer_encode(footer, 0x0102030405060708ULL, digest);
    CHECK(footer[16] == JVMAN_SETUP_FORMAT_VERSION);
    CHECK(footer[24] == 0x08 && footer[31] == 0x01);
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) != 0);

    jvman_setup_footer_encode(footer, 7654321u, digest);
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) == 0);
    CHECK(size == 7654321u);
    CHECK(memcmp(digest, decoded, sizeof(digest)) == 0);

    footer[0] ^= 1u;
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) != 0);
    footer[0] ^= 1u;
    footer[16] = 2;
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) != 0);
    footer[16] = JVMAN_SETUP_FORMAT_VERSION;
    footer[20] = 1;
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) != 0);

    jvman_setup_footer_encode(footer, 0, digest);
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) != 0);
    jvman_setup_footer_encode(footer,
                              (uint64_t)JVMAN_SETUP_PAYLOAD_LIMIT + 1u, digest);
    CHECK(jvman_setup_footer_decode(footer, &size, decoded) != 0);

    if (failures) {
        fprintf(stderr, "%d installer package test(s) failed\n", failures);
        return 1;
    }
    puts("All installer package tests passed.");
    return 0;
}
