#ifndef JVMAN_DOWNLOAD_SOURCE_H
#define JVMAN_DOWNLOAD_SOURCE_H

#include <stddef.h>

typedef enum JvmanDownloadSourceKind {
    JVMAN_DOWNLOAD_SOURCE_ADOPTIUM = 0,
    JVMAN_DOWNLOAD_SOURCE_FOOJAY = 1
} JvmanDownloadSourceKind;

typedef struct JvmanDownloadSource {
    const char *name;
    const char *label;
    JvmanDownloadSourceKind kind;
} JvmanDownloadSource;

const JvmanDownloadSource *jvman_download_source_default(void);
const JvmanDownloadSource *jvman_download_source_find(const char *name);
size_t jvman_download_source_count(void);
const JvmanDownloadSource *jvman_download_source_at(size_t index);

int jvman_download_source_build_metadata_url(
    const JvmanDownloadSource *source, int major, const char *os,
    const char *architecture, const char *archive_extension,
    char *out, size_t out_size);
int jvman_download_source_parse_catalog(
    const JvmanDownloadSource *source, const char *json, int major,
    char *detail_url, size_t detail_url_size,
    char *exact_version, size_t version_size);
int jvman_download_source_parse_package(
    const JvmanDownloadSource *source, const char *json,
    char *download_url, size_t url_size,
    char *checksum, size_t checksum_size);

#endif
