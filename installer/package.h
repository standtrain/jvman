#ifndef JVMAN_INSTALLER_PACKAGE_H
#define JVMAN_INSTALLER_PACKAGE_H

#include <stdint.h>

#define JVMAN_SETUP_FORMAT_VERSION 1u
#define JVMAN_SETUP_FOOTER_SIZE 64u
#define JVMAN_SETUP_PAYLOAD_LIMIT (512u * 1024u * 1024u)

void jvman_setup_footer_encode(unsigned char footer[JVMAN_SETUP_FOOTER_SIZE],
                               uint64_t payload_size,
                               const unsigned char digest[32]);
int jvman_setup_footer_decode(
    const unsigned char footer[JVMAN_SETUP_FOOTER_SIZE],
    uint64_t *payload_size, unsigned char digest[32]);

#endif
