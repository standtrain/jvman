#ifndef JVMAN_UPDATE_H
#define JVMAN_UPDATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct JvmanUpdateVersion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} JvmanUpdateVersion;

typedef enum JvmanUpdateSource {
    JVMAN_UPDATE_SOURCE_AUTO = 0,
    JVMAN_UPDATE_SOURCE_GITHUB,
    JVMAN_UPDATE_SOURCE_GITEE
} JvmanUpdateSource;

int jvman_update_parse_version(const char *text, JvmanUpdateVersion *version);
int jvman_update_compare_versions(const char *left, const char *right,
                                  int *comparison);
int jvman_update_parse_release_json(const char *json, size_t json_size,
                                    char *version, size_t version_size);
int jvman_update_parse_checksum(const char *text, size_t text_size,
                                const char *asset, char out[65]);
int jvman_update_build_release_url(const char *base, const char *version,
                                   const char *asset,
                                   char *out, size_t out_size);
const char *jvman_update_asset_for_platform(const char *os,
                                            const char *arch);

/* Parse a source name ("auto", "github", "gitee") into an enum. Returns -1 on
 * unknown input. */
int jvman_update_source_parse(const char *name, JvmanUpdateSource *out);
/* Print the available update sources to stdout. */
void jvman_update_source_list(void);

/* Runs the public update command. requested_version is NULL for latest. */
int jvman_update_command(int check_only, const char *requested_version,
                         JvmanUpdateSource source);
int jvman_update_run_cli(int argc, char **argv);

#endif
