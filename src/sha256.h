#ifndef JVMAN_SHA256_H
#define JVMAN_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct JvmanSha256 {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_size;
} JvmanSha256;

void jvman_sha256_init(JvmanSha256 *context);
void jvman_sha256_update(JvmanSha256 *context, const void *data, size_t size);
void jvman_sha256_final(JvmanSha256 *context, unsigned char digest[32]);
int jvman_sha256_file(const char *path, unsigned char digest[32]);

#endif
