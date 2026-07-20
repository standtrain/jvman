#include "package.h"

#include "common.h"
#include "sha256.h"

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define path_equal _stricmp
#else
#define path_equal strcmp
#endif

static int copy_file(FILE *input, FILE *output, int hash_payload,
                     uint64_t *size_out, unsigned char digest[32]) {
    unsigned char buffer[64 * 1024];
    size_t count;
    uint64_t total = 0;
    int first_block = 1;
    JvmanSha256 sha;
    if (hash_payload) jvman_sha256_init(&sha);
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (first_block) {
            if (count < 2 || buffer[0] != 'M' || buffer[1] != 'Z') return -1;
            first_block = 0;
        }
        if (total > UINT64_MAX - (uint64_t)count ||
            (hash_payload && total + (uint64_t)count > JVMAN_SETUP_PAYLOAD_LIMIT) ||
            fwrite(buffer, 1, count, output) != count) {
            return -1;
        }
        if (hash_payload) jvman_sha256_update(&sha, buffer, count);
        total += (uint64_t)count;
    }
    if (ferror(input) || first_block) return -1;
    if (hash_payload) jvman_sha256_final(&sha, digest);
    if (size_out) *size_out = total;
    return 0;
}

int main(int argc, char **argv) {
    FILE *stub = NULL;
    FILE *payload = NULL;
    FILE *output = NULL;
    char temporary[JVMAN_PATH_MAX];
#if defined(_WIN32)
    char stub_full[JVMAN_PATH_MAX];
    char payload_full[JVMAN_PATH_MAX];
    char output_full[JVMAN_PATH_MAX];
    char temporary_full[JVMAN_PATH_MAX];
#endif
    uint64_t payload_size = 0;
    unsigned char digest[32];
    unsigned char footer[JVMAN_SETUP_FOOTER_SIZE];
    int result = 1;
    if (argc != 4) {
        fprintf(stderr, "usage: pack_setup <stub.exe> <jvman.exe> <setup.exe>\n");
        return 2;
    }
    /* Compare canonical absolute names as well as the spelling supplied by
     * the caller.  This prevents an input such as "out.exe.tmp" from being
     * truncated when it is also the generated temporary output. */
    if (path_equal(argv[1], argv[3]) == 0 || path_equal(argv[2], argv[3]) == 0 ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", argv[3]) < 0 ||
        strlen(temporary) + 1 >= sizeof(temporary)) {
        fprintf(stderr, "pack_setup: invalid or unsafe output path\n");
        return 2;
    }
#if defined(_WIN32)
    if (!_fullpath(stub_full, argv[1], sizeof(stub_full)) ||
        !_fullpath(payload_full, argv[2], sizeof(payload_full)) ||
        !_fullpath(output_full, argv[3], sizeof(output_full)) ||
        !_fullpath(temporary_full, temporary, sizeof(temporary_full)) ||
        path_equal(stub_full, output_full) == 0 ||
        path_equal(payload_full, output_full) == 0 ||
        path_equal(stub_full, temporary_full) == 0 ||
        path_equal(payload_full, temporary_full) == 0) {
        fprintf(stderr, "pack_setup: input and output paths must be distinct\n");
        return 2;
    }
#endif
    stub = fopen(argv[1], "rb");
    payload = fopen(argv[2], "rb");
    output = fopen(temporary, "wb");
    if (!stub || !payload || !output) {
        fprintf(stderr, "pack_setup: cannot open an input or output file\n");
        goto done;
    }
    if (copy_file(stub, output, 0, NULL, digest) != 0 ||
        copy_file(payload, output, 1, &payload_size, digest) != 0) {
        fprintf(stderr, "pack_setup: invalid executable input or write failure\n");
        goto done;
    }
    jvman_setup_footer_encode(footer, payload_size, digest);
    if (fwrite(footer, 1, sizeof(footer), output) != sizeof(footer) ||
        fflush(output) != 0 || fclose(output) != 0) {
        output = NULL;
        fprintf(stderr, "pack_setup: cannot finalize the setup executable\n");
        goto done;
    }
    output = NULL;
    if (remove(argv[3]) != 0 && errno != ENOENT) {
        fprintf(stderr, "pack_setup: cannot replace the existing setup executable\n");
        goto done;
    }
    if (rename(temporary, argv[3]) != 0) {
        fprintf(stderr, "pack_setup: cannot publish the setup executable\n");
        goto done;
    }
    printf("Packed %s (%llu-byte payload)\n", argv[3],
           (unsigned long long)payload_size);
    result = 0;
done:
    if (stub) fclose(stub);
    if (payload) fclose(payload);
    if (output) fclose(output);
    if (result != 0) remove(temporary);
    return result;
}
