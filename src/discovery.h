#ifndef JVMAN_DISCOVERY_H
#define JVMAN_DISCOVERY_H

#include "common.h"

#include <stddef.h>

#define JVMAN_DISCOVERY_VERSION_MAX 127
#define JVMAN_DISCOVERY_IMPLEMENTOR_MAX 127
#define JVMAN_DISCOVERY_VENDOR_MAX 31
#define JVMAN_DISCOVERY_ARCH_MAX 63

typedef enum JvmanDiscoveryType {
    JVMAN_DISCOVERY_INVALID = 0,
    JVMAN_DISCOVERY_JRE = 1,
    JVMAN_DISCOVERY_JDK = 2
} JvmanDiscoveryType;

typedef enum JvmanDiscoverySource {
    JVMAN_SOURCE_EXPLICIT = 1u << 0,
    JVMAN_SOURCE_JAVA_HOME = 1u << 1,
    JVMAN_SOURCE_PATH = 1u << 2,
    JVMAN_SOURCE_REGISTRY = 1u << 3,
    JVMAN_SOURCE_PROGRAM_FILES = 1u << 4,
    JVMAN_SOURCE_USER_JDKS = 1u << 5,
    JVMAN_SOURCE_SDKMAN = 1u << 6,
    JVMAN_SOURCE_JABBA = 1u << 7,
    JVMAN_SOURCE_SYSTEM = 1u << 8,
    JVMAN_SOURCE_MACOS = 1u << 9,
    JVMAN_SOURCE_HOMEBREW = 1u << 10
} JvmanDiscoverySource;

typedef struct JvmanJavaRelease {
    char version[JVMAN_DISCOVERY_VERSION_MAX + 1];
    char implementor[JVMAN_DISCOVERY_IMPLEMENTOR_MAX + 1];
    char arch[JVMAN_DISCOVERY_ARCH_MAX + 1];
} JvmanJavaRelease;

typedef struct JvmanDiscoveryCandidate {
    JvmanDiscoveryType type;
    unsigned int sources;
    int release_valid;
    int internal_only;
    char home[JVMAN_PATH_MAX];
    char version[JVMAN_DISCOVERY_VERSION_MAX + 1];
    char implementor[JVMAN_DISCOVERY_IMPLEMENTOR_MAX + 1];
    char vendor[JVMAN_DISCOVERY_VENDOR_MAX + 1];
    char arch[JVMAN_DISCOVERY_ARCH_MAX + 1];
    char suggested_name[JVMAN_NAME_MAX + 1];
} JvmanDiscoveryCandidate;

typedef struct JvmanDiscoveryList {
    JvmanDiscoveryCandidate *items;
    size_t count;
    size_t capacity;
} JvmanDiscoveryList;

typedef int (*JvmanDiscoveryPathVisitor)(const char *home,
                                         const char *vendor_hint,
                                         void *context);
typedef int (*JvmanDiscoveryRegistryEnumerator)(
    JvmanDiscoveryPathVisitor visitor,
    void *visitor_context,
    void *platform_context);

typedef struct JvmanDiscoveryOptions {
    const char *java_home;
    const char *path;
    const char *user_home;
    const char *jvman_root;
    const char *jvman_jdks;
    int use_environment;
    int scan_common_roots;
    JvmanDiscoveryRegistryEnumerator registry_enumerator;
    void *registry_context;
} JvmanDiscoveryOptions;

void jvman_discovery_options_init(JvmanDiscoveryOptions *options);
void jvman_discovery_list_init(JvmanDiscoveryList *list);
void jvman_discovery_list_free(JvmanDiscoveryList *list);

int jvman_discovery_parse_release(const char *path, JvmanJavaRelease *release);
int jvman_discovery_normalize_vendor(const char *implementor,
                                     const char *path_hint,
                                     char *out, size_t out_size);
int jvman_discovery_normalize_version(const char *version,
                                      char *out, size_t out_size);
int jvman_discovery_suggest_name(const char *vendor, const char *version,
                                 char *out, size_t out_size);

int jvman_discovery_probe_home(const char *input, unsigned int source,
                               const char *vendor_hint,
                               JvmanDiscoveryCandidate *candidate);
int jvman_discovery_probe_java(const char *input, unsigned int source,
                               const char *vendor_hint,
                               JvmanDiscoveryCandidate *candidate);
int jvman_discovery_find_jdk_home(const char *input,
                                  char *out, size_t out_size);

int jvman_discovery_add_home(JvmanDiscoveryList *list, const char *input,
                             unsigned int source, const char *vendor_hint);
int jvman_discovery_add_java(JvmanDiscoveryList *list, const char *input,
                             unsigned int source, const char *vendor_hint);
int jvman_discovery_scan_root(JvmanDiscoveryList *list, const char *root,
                              unsigned int source, const char *vendor_hint,
                              unsigned int max_depth);
int jvman_discovery_scan(JvmanDiscoveryList *list,
                         const JvmanDiscoveryOptions *options);
void jvman_discovery_sort(JvmanDiscoveryList *list);

const char *jvman_discovery_type_name(JvmanDiscoveryType type);
int jvman_discovery_format_sources(unsigned int sources,
                                   char *out, size_t out_size);

#endif
