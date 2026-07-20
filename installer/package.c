#include "package.h"

#include <string.h>

static const unsigned char setup_magic[16] = {
    'J', 'V', 'M', 'A', 'N', 'S', 'E', 'T',
    'U', 'P', '0', '1', '\r', '\n', 0x1a, 0x00
};

static void write_u32_le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
    out[2] = (unsigned char)(value >> 16);
    out[3] = (unsigned char)(value >> 24);
}

static void write_u64_le(unsigned char *out, uint64_t value) {
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        out[i] = (unsigned char)(value >> (i * 8));
    }
}

static uint32_t read_u32_le(const unsigned char *input) {
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) |
           ((uint32_t)input[3] << 24);
}

static uint64_t read_u64_le(const unsigned char *input) {
    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        value |= (uint64_t)input[i] << (i * 8);
    }
    return value;
}

void jvman_setup_footer_encode(unsigned char footer[JVMAN_SETUP_FOOTER_SIZE],
                               uint64_t payload_size,
                               const unsigned char digest[32]) {
    if (!footer) return;
    memset(footer, 0, JVMAN_SETUP_FOOTER_SIZE);
    if (!digest) return;
    memcpy(footer, setup_magic, sizeof(setup_magic));
    write_u32_le(footer + 16, JVMAN_SETUP_FORMAT_VERSION);
    write_u64_le(footer + 24, payload_size);
    memcpy(footer + 32, digest, 32);
}

int jvman_setup_footer_decode(
    const unsigned char footer[JVMAN_SETUP_FOOTER_SIZE],
    uint64_t *payload_size, unsigned char digest[32]) {
    static const unsigned char zero_reserved[4] = {0, 0, 0, 0};
    uint64_t size;
    if (!footer || !payload_size || !digest ||
        memcmp(footer, setup_magic, sizeof(setup_magic)) != 0 ||
        read_u32_le(footer + 16) != JVMAN_SETUP_FORMAT_VERSION ||
        memcmp(footer + 20, zero_reserved, sizeof(zero_reserved)) != 0) {
        return -1;
    }
    size = read_u64_le(footer + 24);
    if (size == 0 || size > JVMAN_SETUP_PAYLOAD_LIMIT) return -1;
    *payload_size = size;
    memcpy(digest, footer + 32, 32);
    return 0;
}
