#include "manager.h"

#include "download_source.h"
#include "discovery.h"
#include "env_persist.h"
#include "i18n.h"
#include "platform.h"
#include "sha256.h"
#include "update.h"
#include "util.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Direct stdout formats below are application-owned literals, never input. */
#define printf(...) jvman_i18n_printf(__VA_ARGS__)

#define JVMAN_METADATA_LIMIT (1024u * 1024u)
#define JVMAN_DOWNLOAD_PROBE_SIZE (64u * 1024u)
#define JVMAN_DOWNLOAD_PROBE_TIMEOUT 15u
#define JVMAN_JDK_ARCHIVE_LIMIT ((size_t)1024u * 1024u * 1024u)
#define JVMAN_DOWNLOAD_SOURCE_LIMIT 64u
#define JVMAN_CUSTOM_SOURCE_LIMIT 32u
#define JVMAN_SOURCE_TEMPLATE_MAX 2048u
#define JVMAN_SOURCE_LABEL_MAX (JVMAN_NAME_MAX + 32u)

typedef struct DownloadSourceRegistry {
    const JvmanDownloadSource *items[JVMAN_DOWNLOAD_SOURCE_LIMIT];
    JvmanDownloadSource custom[JVMAN_CUSTOM_SOURCE_LIMIT];
    char names[JVMAN_CUSTOM_SOURCE_LIMIT][JVMAN_NAME_MAX + 1];
    char templates[JVMAN_CUSTOM_SOURCE_LIMIT][JVMAN_SOURCE_TEMPLATE_MAX];
    size_t count;
    size_t custom_count;
} DownloadSourceRegistry;

static const char *download_source_display_label(
    const JvmanDownloadSource *source, char *buffer, size_t buffer_size) {
    int written;
    if (!source || !source->label) return "";
    if (source->kind != JVMAN_DOWNLOAD_SOURCE_CUSTOM_ADOPTIUM) {
        return jvman_i18n_text(source->label);
    }
    if (!buffer || buffer_size == 0 || !source->name) return "";
    written = snprintf(buffer, buffer_size,
                       jvman_i18n_text(source->label), source->name);
    if (written < 0 || (size_t)written >= buffer_size) return source->name;
    return buffer;
}

static int print_error(const char *message) {
    jvman_i18n_fprintf(stderr, "jvman: %s\n", jvman_i18n_text(message));
    return 1;
}

static int print_platform_error(const char *message) {
    jvman_i18n_fprintf(stderr, "jvman: %s: %s\n",
                       jvman_i18n_text(message), platform_last_error());
    return 1;
}

int jvman_context_init(JvmanContext *context) {
    char raw_root[JVMAN_PATH_MAX];
    if (!context || platform_default_root(raw_root, sizeof(raw_root)) != 0 ||
        platform_absolute_path(raw_root, context->root, sizeof(context->root)) != 0) {
        return -1;
    }
    if (jvman_path_join(context->versions, sizeof(context->versions), context->root,
                        "versions") != 0 ||
        jvman_path_join(context->jdks, sizeof(context->jdks), context->root,
                        "jdks") != 0 ||
        jvman_path_join(context->cache, sizeof(context->cache), context->root,
                        "cache") != 0 ||
        jvman_path_join(context->staging, sizeof(context->staging), context->root,
                        "staging") != 0 ||
        jvman_path_join(context->sources, sizeof(context->sources), context->root,
                        "sources") != 0 ||
        jvman_path_join(context->current_link, sizeof(context->current_link), context->root,
                        "current") != 0 ||
        jvman_path_join(context->current_state, sizeof(context->current_state), context->root,
                        "current.version") != 0 ||
        jvman_path_join(context->download_source, sizeof(context->download_source),
                        context->root, "source.conf") != 0 ||
        jvman_path_join(context->lock_file, sizeof(context->lock_file), context->root,
                        "state.lock") != 0) {
        return -1;
    }
    return 0;
}

static int prepare_layout(const JvmanContext *context) {
    if (platform_mkdirs(context->root) != 0 ||
        platform_mkdirs(context->versions) != 0 ||
        platform_mkdirs(context->jdks) != 0 ||
        platform_mkdirs(context->cache) != 0 ||
        platform_mkdirs(context->staging) != 0 ||
        platform_mkdirs(context->sources) != 0) {
        return -1;
    }
    return 0;
}

static int registration_path(const JvmanContext *context, const char *name,
                             char *out, size_t out_size) {
    char filename[JVMAN_NAME_MAX + 16];
    if (!jvman_valid_name(name) ||
        snprintf(filename, sizeof(filename), "%s.conf", name) >= (int)sizeof(filename)) {
        return -1;
    }
    return jvman_path_join(out, out_size, context->versions, filename);
}

static int read_registration(const JvmanContext *context, const char *name,
                             JvmanRegistration *registration) {
    char path[JVMAN_PATH_MAX];
    FILE *file;
    char line[JVMAN_LINE_MAX];
    int found_home = 0;
    int found_managed = 0;
    if (!registration || registration_path(context, name, path, sizeof(path)) != 0) {
        return -1;
    }
    file = fopen(path, "rb");
    if (!file) return -1;
    memset(registration, 0, sizeof(*registration));
    strcpy(registration->name, name);
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
            line[--length] = '\0';
        }
        if (strncmp(line, "managed=", 8) == 0) {
            if (strcmp(line + 8, "0") != 0 && strcmp(line + 8, "1") != 0) {
                fclose(file); return -1;
            }
            registration->managed = line[8] == '1';
            found_managed = 1;
        } else if (strncmp(line, "home=", 5) == 0) {
            if (strlen(line + 5) >= sizeof(registration->home)) {
                fclose(file); return -1;
            }
            strcpy(registration->home, line + 5);
            found_home = 1;
        }
    }
    fclose(file);
    return found_home && found_managed ? 0 : -1;
}

static int write_registration(const JvmanContext *context,
                              const JvmanRegistration *registration) {
    char path[JVMAN_PATH_MAX];
    char content[JVMAN_PATH_MAX + 64];
    if (registration_path(context, registration->name, path, sizeof(path)) != 0 ||
        strchr(registration->home, '\n') || strchr(registration->home, '\r') ||
        snprintf(content, sizeof(content), "managed=%d\nhome=%s\n",
                 registration->managed ? 1 : 0, registration->home) >= (int)sizeof(content)) {
        return -1;
    }
    return platform_write_text_atomic(path, content);
}

static int registration_exists(const JvmanContext *context, const char *name) {
    char path[JVMAN_PATH_MAX];
    return registration_path(context, name, path, sizeof(path)) == 0 &&
           platform_is_file(path);
}

static int current_name(const JvmanContext *context, char *out, size_t out_size) {
    if (platform_read_line(context->current_state, out, out_size) != 0 ||
        !jvman_valid_name(out)) {
        if (out_size) out[0] = '\0';
        return -1;
    }
    return 0;
}

static int find_jdk_home(const char *input, char *out, size_t out_size) {
    return jvman_discovery_find_jdk_home(input, out, out_size);
}

static int acquire_lock(const JvmanContext *context, PlatformLock *lock) {
    if (prepare_layout(context) != 0) return -1;
    return platform_lock_acquire(context->lock_file, lock);
}

static int command_add(const JvmanContext *context, const char *name, const char *path) {
    JvmanRegistration registration;
    PlatformLock lock;
    int result = 1;
    if (!jvman_valid_name(name)) return print_error("invalid version name");
    if (find_jdk_home(path, registration.home, sizeof(registration.home)) != 0) {
        return print_error("the path is not a JDK home (bin/java and bin/javac are required)");
    }
    if (jvman_path_equal(registration.home, context->current_link) ||
        jvman_path_is_within(context->current_link, registration.home)) {
        return print_error("a JDK registration cannot point to jvman's current link");
    }
    strcpy(registration.name, name);
    registration.managed = 0;
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    if (registration_exists(context, name)) {
        print_error("that version name is already registered");
        goto done;
    }
    if (write_registration(context, &registration) != 0) {
        print_platform_error("cannot save registration");
        goto done;
    }
    printf("Registered %s -> %s\n", name, registration.home);
    result = 0;
done:
    platform_lock_release(&lock);
    return result;
}

static int switch_to_registration(const JvmanContext *context,
                                  const JvmanRegistration *registration) {
    char state[JVMAN_NAME_MAX + 4];
    char previous[JVMAN_NAME_MAX + 1] = {0};
    int had_previous = current_name(context, previous, sizeof(previous)) == 0;
    if (!platform_is_directory(registration->home)) {
        return print_error("registered JDK home no longer exists");
    }
    if (snprintf(state, sizeof(state), "%s\n", registration->name) >= (int)sizeof(state) ||
        platform_write_text_atomic(context->current_state, state) != 0) {
        return print_platform_error("cannot record the selected JDK");
    }
    if (platform_replace_directory_link(context->current_link, registration->home) != 0) {
        char switch_error[512];
        snprintf(switch_error, sizeof(switch_error), "%s", platform_last_error());
        if (had_previous) {
            snprintf(state, sizeof(state), "%s\n", previous);
            if (platform_write_text_atomic(context->current_state, state) != 0) {
                jvman_i18n_fprintf(
                    stderr,
                    "jvman: cannot switch current JDK: %s; state rollback also failed\n",
                    switch_error);
                return 1;
            }
        } else if (platform_remove_file(context->current_state) != 0) {
            jvman_i18n_fprintf(
                stderr,
                "jvman: cannot switch current JDK: %s; state rollback also failed\n",
                switch_error);
            return 1;
        }
        jvman_i18n_fprintf(stderr, "jvman: cannot switch current JDK: %s\n",
                           switch_error);
        return 1;
    }
    return 0;
}

static int shell_path_starts_with(const char *directory) {
    const char *path = getenv("PATH");
    const char *first_end;
    size_t first_length;
    char first[JVMAN_PATH_MAX];
    if (!directory || !path || !*path) return 0;
#if defined(_WIN32)
    first_end = strchr(path, ';');
#else
    first_end = strchr(path, ':');
#endif
    first_length = first_end ? (size_t)(first_end - path) : strlen(path);
    if (first_length == 0 || first_length >= sizeof(first)) return 0;
    memcpy(first, path, first_length);
    first[first_length] = '\0';
    return jvman_path_equal(first, directory);
}

static int shell_resolves_selected_java(const JvmanContext *context) {
    char current_bin[JVMAN_PATH_MAX];
    char expected[JVMAN_PATH_MAX];
    char expected_canonical[JVMAN_PATH_MAX];
    char actual[JVMAN_PATH_MAX];
    const char *java_home = getenv("JAVA_HOME");
    if (!context || !java_home ||
        !jvman_path_equal(java_home, context->current_link) ||
        jvman_path_join(current_bin, sizeof(current_bin), context->current_link,
                        "bin") != 0 ||
        !shell_path_starts_with(current_bin) ||
        jvman_path_join3(expected, sizeof(expected), context->current_link,
                         "bin", JVMAN_JAVA_EXE) != 0 ||
        platform_absolute_path(expected, expected_canonical,
                               sizeof(expected_canonical)) != 0 ||
        platform_find_executable(JVMAN_JAVA_EXE, actual, sizeof(actual)) != 0) {
        return 0;
    }
    return jvman_path_equal(expected_canonical, actual);
}

static void print_shell_initialization_hint(const JvmanContext *context) {
    int owned = 0;
    if (shell_resolves_selected_java(context)) return;
    if (jvman_persist_is_owned(context, &owned) == 0 && owned) {
        printf("Persistent activation is already in place; new terminals and IDEs will pick up the change.\n");
        printf("To refresh the current shell without reopening, run:\n");
#if defined(_WIN32)
        printf("  CMD: for /f \"delims=\" %%L in ('jvman init cmd') do @%%L\n");
        printf("  PowerShell: jvman init powershell | Invoke-Expression\n");
#else
        printf("  sh: eval \"$(jvman init sh)\"\n");
#endif
        return;
    }
#if defined(_WIN32)
    printf("The selected Java is not active in this shell. A child process cannot update its parent shell. Initialize this shell once with one of:\n");
    printf("  CMD: for /f \"delims=\" %%L in ('jvman init cmd') do @%%L\n");
    printf("  PowerShell: jvman init powershell | Invoke-Expression\n");
#else
    printf("The selected Java is not active in this shell. A child process cannot update its parent shell. Initialize this shell once with:\n");
    printf("  sh: eval \"$(jvman init sh)\"\n");
#endif
    printf("Add the same initialization to your shell startup file for future terminals.\n");
}

static int command_use(const JvmanContext *context, const char *name,
                       int no_persist) {
    JvmanRegistration registration;
    PlatformLock lock;
    int result = 1;
    if (!jvman_valid_name(name)) return print_error("unknown JDK version");
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    if (read_registration(context, name, &registration) != 0) {
        print_error("unknown JDK version");
        goto done;
    }
    if (find_jdk_home(registration.home, registration.home, sizeof(registration.home)) != 0) {
        print_error("registered path is not a valid JDK anymore");
        goto done;
    }
    if (jvman_path_equal(registration.home, context->current_link) ||
        jvman_path_is_within(context->current_link, registration.home)) {
        print_error("registered JDK resolves through jvman's current link");
        goto done;
    }
    result = switch_to_registration(context, &registration);
    if (result == 0 && !no_persist) {
        JvmanPersistOptions persist_opts;
        persist_opts.ctx = context;
        persist_opts.replace_java_home = 0;
        persist_opts.quiet = 1;
        (void)jvman_persist_activate(&persist_opts);
    }
done:
    platform_lock_release(&lock);
    if (result == 0) {
        printf("Now using %s (%s)\n", registration.name, registration.home);
        print_shell_initialization_hint(context);
    }
    return result;
}

static int name_compare(const void *left, const void *right) {
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static int command_list(const JvmanContext *context) {
    char **entries = NULL;
    size_t count = 0;
    size_t i;
    size_t shown = 0;
    char active[JVMAN_NAME_MAX + 1] = {0};
    if (prepare_layout(context) != 0) return print_platform_error("cannot prepare data directory");
    current_name(context, active, sizeof(active));
    if (platform_list_directory(context->versions, &entries, &count) != 0) {
        return print_platform_error("cannot list registrations");
    }
    qsort(entries, count, sizeof(*entries), name_compare);
    printf("  %-20s %-9s %s\n", jvman_i18n_text("VERSION"),
           jvman_i18n_text("SOURCE"), "JAVA_HOME");
    for (i = 0; i < count; ++i) {
        JvmanRegistration registration;
        size_t length;
        if (!jvman_ends_with(entries[i], ".conf")) continue;
        length = strlen(entries[i]) - 5;
        if (length == 0 || length > JVMAN_NAME_MAX) continue;
        entries[i][length] = '\0';
        if (read_registration(context, entries[i], &registration) != 0) continue;
        printf("%c %-20s %-9s %s%s\n",
               jvman_name_equal(active, registration.name) ? '*' : ' ',
               registration.name,
               jvman_i18n_text(registration.managed ? "managed" : "external"),
               registration.home,
               platform_is_directory(registration.home)
                   ? "" : jvman_i18n_text(" [missing]"));
        ++shown;
    }
    platform_free_directory_list(entries, count);
    if (!shown) printf("  (no JDKs registered)\n");
    return 0;
}

typedef struct JvmanRegistrationList {
    JvmanRegistration *items;
    size_t count;
    size_t capacity;
} JvmanRegistrationList;

typedef struct JvmanDiscoveryView {
    char name[JVMAN_NAME_MAX + 1];
    char status[JVMAN_NAME_MAX + 32];
    int reserves_name;
} JvmanDiscoveryView;

static void registration_list_free(JvmanRegistrationList *list) {
    if (!list) return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int registration_list_reserve(JvmanRegistrationList *list, size_t needed) {
    JvmanRegistration *grown;
    size_t capacity;
    if (needed <= list->capacity) return 0;
    capacity = list->capacity ? list->capacity * 2 : 16;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2) return -1;
        capacity *= 2;
    }
    grown = (JvmanRegistration *)realloc(list->items, capacity * sizeof(*grown));
    if (!grown) return -1;
    list->items = grown;
    list->capacity = capacity;
    return 0;
}

static int registration_list_append(JvmanRegistrationList *list,
                                    const JvmanRegistration *registration) {
    if (registration_list_reserve(list, list->count + 1) != 0) return -1;
    list->items[list->count++] = *registration;
    return 0;
}

static int load_registrations(const JvmanContext *context,
                              JvmanRegistrationList *list) {
    char **entries = NULL;
    size_t count = 0;
    size_t i;
    memset(list, 0, sizeof(*list));
    if (!platform_path_exists(context->versions)) return 0;
    if (platform_list_directory(context->versions, &entries, &count) != 0) return -1;
    if (count > 1) qsort(entries, count, sizeof(*entries), name_compare);
    for (i = 0; i < count; ++i) {
        size_t length;
        JvmanRegistration registration;
        if (!jvman_ends_with(entries[i], ".conf")) continue;
        length = strlen(entries[i]) - 5;
        if (length == 0 || length > JVMAN_NAME_MAX) continue;
        entries[i][length] = '\0';
        if (read_registration(context, entries[i], &registration) == 0 &&
            registration_list_append(list, &registration) != 0) {
            platform_free_directory_list(entries, count);
            registration_list_free(list);
            return -1;
        }
    }
    platform_free_directory_list(entries, count);
    return 0;
}

static int registration_index_by_home(const JvmanRegistrationList *list,
                                      const char *home) {
    char canonical_home[JVMAN_PATH_MAX];
    size_t i;
    if (platform_absolute_path(home, canonical_home, sizeof(canonical_home)) != 0) {
        return -1;
    }
    for (i = 0; i < list->count; ++i) {
        char registered_home[JVMAN_PATH_MAX];
        if (platform_absolute_path(list->items[i].home, registered_home,
                                   sizeof(registered_home)) == 0 &&
            jvman_path_equal(canonical_home, registered_home)) {
            return (int)i;
        }
    }
    return -1;
}

static int discovery_name_used(const JvmanRegistrationList *registrations,
                               const JvmanDiscoveryView *views,
                               size_t view_count, const char *name) {
    size_t i;
    for (i = 0; i < registrations->count; ++i) {
        if (jvman_name_equal(registrations->items[i].name, name)) return 1;
    }
    for (i = 0; i < view_count; ++i) {
        if (views[i].reserves_name && jvman_name_equal(views[i].name, name)) return 1;
    }
    return 0;
}

static int discovery_unique_name(const JvmanContext *context,
                                 const char *base,
                                 const JvmanRegistrationList *registrations,
                                 const JvmanDiscoveryView *views,
                                 size_t view_count, char *out, size_t out_size) {
    unsigned int suffix = 1;
    const char *safe_base = base && *base ? base : "unknown-java";
    for (;;) {
        char config_path[JVMAN_PATH_MAX];
        char suffix_text[24] = {0};
        size_t suffix_length = 0;
        size_t base_length;
        if (suffix > 1) {
            int written = snprintf(suffix_text, sizeof(suffix_text), "-%u", suffix);
            if (written < 0 || written >= (int)sizeof(suffix_text)) return -1;
            suffix_length = (size_t)written;
        }
        if (suffix_length + 1 > out_size) return -1;
        base_length = strlen(safe_base);
        if (base_length + suffix_length + 1 > out_size) {
            base_length = out_size - suffix_length - 1;
        }
        memcpy(out, safe_base, base_length);
        memcpy(out + base_length, suffix_text, suffix_length + 1);
        if (jvman_valid_name(out)) {
            if (registration_path(context, out, config_path,
                                  sizeof(config_path)) != 0) return -1;
            if (!platform_path_exists(config_path) &&
                !discovery_name_used(registrations, views, view_count, out)) {
                return 0;
            }
        }
        if (suffix == (unsigned int)-1) return -1;
        ++suffix;
    }
}

static int discovery_registry_enumerator(JvmanDiscoveryPathVisitor visitor,
                                         void *visitor_context,
                                         void *platform_context) {
    (void)platform_context;
    return platform_visit_java_registry_homes(visitor, visitor_context);
}

static int discovery_home_is_internal(const JvmanContext *context,
                                      const char *home) {
    if (jvman_path_equal(home, context->current_link) ||
        jvman_path_is_within(context->jdks, home)) {
        return 1;
    }
    return 0;
}

static void discovery_set_registered_view(JvmanDiscoveryView *view,
                                          const char *name) {
    snprintf(view->name, sizeof(view->name), "%s", name);
    snprintf(view->status, sizeof(view->status), "registered:%s", name);
    view->reserves_name = 0;
}

static int prepare_discovery_preview(const JvmanContext *context,
                                     const JvmanDiscoveryList *discovered,
                                     const JvmanRegistrationList *registrations,
                                     JvmanDiscoveryView *views) {
    size_t i;
    for (i = 0; i < discovered->count; ++i) {
        const JvmanDiscoveryCandidate *candidate = &discovered->items[i];
        int existing = registration_index_by_home(registrations, candidate->home);
        if (existing >= 0) {
            discovery_set_registered_view(&views[i], registrations->items[existing].name);
        } else if (candidate->internal_only ||
                   discovery_home_is_internal(context, candidate->home)) {
            snprintf(views[i].name, sizeof(views[i].name), "%s",
                     candidate->suggested_name[0] ? candidate->suggested_name : "-");
            strcpy(views[i].status, "invalid");
        } else if (candidate->type == JVMAN_DISCOVERY_JDK && candidate->release_valid) {
            if (discovery_unique_name(context, candidate->suggested_name,
                                      registrations,
                                      views, i, views[i].name,
                                      sizeof(views[i].name)) != 0) return -1;
            strcpy(views[i].status, "new");
            views[i].reserves_name = 1;
        } else {
            snprintf(views[i].name, sizeof(views[i].name), "%s",
                     candidate->suggested_name[0] ? candidate->suggested_name : "-");
            strcpy(views[i].status,
                   candidate->type == JVMAN_DISCOVERY_JRE ? "jre" : "invalid");
        }
    }
    return 0;
}

static void print_discovery_results(const JvmanDiscoveryList *discovered,
                                    const JvmanDiscoveryView *views) {
    size_t i;
    size_t new_count = 0;
    size_t registered_count = 0;
    size_t jre_count = 0;
    size_t invalid_count = 0;
    jvman_i18n_puts("TYPE    VERSION          VENDOR         NAME                     STATUS                   SOURCES                  JAVA_HOME");
    for (i = 0; i < discovered->count; ++i) {
        const JvmanDiscoveryCandidate *candidate = &discovered->items[i];
        const char *status = views[i].status[0] ? views[i].status : "invalid";
        char localized_status[JVMAN_NAME_MAX + 32];
        char sources[160];
        if (strncmp(status, "registered:", 11) == 0) {
            int written = snprintf(localized_status, sizeof(localized_status),
                                   "%s:%s", jvman_i18n_text("registered"),
                                   status + 11);
            if (written >= 0 && written < (int)sizeof(localized_status)) {
                status = localized_status;
            }
        } else {
            status = jvman_i18n_text(status);
        }
        jvman_discovery_format_sources(candidate->sources, sources, sizeof(sources));
        printf("%-7s %-16s %-14s %-24s %-24s %-24s %s\n",
               jvman_discovery_type_name(candidate->type),
               candidate->version[0] ? candidate->version : "-",
               candidate->vendor[0] ? candidate->vendor
                                    : jvman_i18n_text("unknown"),
               views[i].name[0] ? views[i].name : "-",
               status,
               sources[0] ? sources : "-", candidate->home);
        if (strcmp(views[i].status, "new") == 0) ++new_count;
        else if (strncmp(views[i].status, "registered:", 11) == 0) ++registered_count;
        else if (strcmp(views[i].status, "jre") == 0) ++jre_count;
        else ++invalid_count;
    }
    if (discovered->count == 0) {
        jvman_i18n_puts("(no Java installations discovered)");
    }
    printf("Summary: %lu new, %lu registered, %lu JRE, %lu invalid\n",
           (unsigned long)new_count, (unsigned long)registered_count,
           (unsigned long)jre_count, (unsigned long)invalid_count);
}

static int command_discover(const JvmanContext *context, int register_found) {
    JvmanDiscoveryOptions options;
    JvmanDiscoveryList discovered;
    JvmanRegistrationList registrations;
    JvmanDiscoveryView *views = NULL;
    PlatformLock lock;
    int lock_held = 0;
    int result = 1;
    size_t i;
    size_t added = 0;
    int write_failed = 0;

    jvman_discovery_options_init(&options);
    options.jvman_root = context->root;
    options.jvman_jdks = context->jdks;
    options.registry_enumerator = discovery_registry_enumerator;
    jvman_discovery_list_init(&discovered);
    memset(&registrations, 0, sizeof(registrations));
    if (jvman_discovery_scan(&discovered, &options) != 0) {
        print_platform_error("Java discovery failed");
        goto done;
    }
    jvman_discovery_sort(&discovered);
    if (discovered.count > 0) {
        views = (JvmanDiscoveryView *)calloc(discovered.count, sizeof(*views));
        if (!views) {
            print_error("out of memory preparing discovery results");
            goto done;
        }
    }

    if (!register_found) {
        if (load_registrations(context, &registrations) != 0 ||
            prepare_discovery_preview(context, &discovered,
                                      &registrations, views) != 0) {
            print_error("cannot compare discovered JDKs with registrations");
            goto done;
        }
        print_discovery_results(&discovered, views);
        result = 0;
        goto done;
    }

    if (acquire_lock(context, &lock) != 0) {
        print_platform_error("cannot lock state");
        goto done;
    }
    lock_held = 1;
    if (load_registrations(context, &registrations) != 0) {
        print_error("cannot load registrations");
        goto done;
    }
    for (i = 0; i < discovered.count; ++i) {
        JvmanDiscoveryCandidate *candidate = &discovered.items[i];
        JvmanDiscoveryCandidate fresh;
        JvmanRegistration registration;
        int existing = registration_index_by_home(&registrations, candidate->home);
        if (existing >= 0) {
            discovery_set_registered_view(&views[i], registrations.items[existing].name);
            continue;
        }
        if (candidate->type == JVMAN_DISCOVERY_JRE) {
            snprintf(views[i].name, sizeof(views[i].name), "%s",
                     candidate->suggested_name[0] ? candidate->suggested_name : "-");
            strcpy(views[i].status, "jre");
            continue;
        }
        if (candidate->type != JVMAN_DISCOVERY_JDK || !candidate->release_valid ||
            candidate->internal_only || discovery_home_is_internal(context, candidate->home)) {
            snprintf(views[i].name, sizeof(views[i].name), "%s",
                     candidate->suggested_name[0] ? candidate->suggested_name : "-");
            strcpy(views[i].status, "invalid");
            continue;
        }
        if (jvman_discovery_probe_home(candidate->home, candidate->sources,
                                       candidate->vendor, &fresh) != 0 ||
            fresh.type != JVMAN_DISCOVERY_JDK || !fresh.release_valid ||
            discovery_home_is_internal(context, fresh.home)) {
            snprintf(views[i].name, sizeof(views[i].name), "%s",
                     candidate->suggested_name[0] ? candidate->suggested_name : "-");
            strcpy(views[i].status, "invalid");
            jvman_i18n_fprintf(
                stderr,
                "jvman: discovered JDK disappeared or became invalid: %s\n",
                candidate->home);
            continue;
        }
        fresh.sources = candidate->sources;
        *candidate = fresh;
        existing = registration_index_by_home(&registrations, candidate->home);
        if (existing >= 0) {
            discovery_set_registered_view(&views[i], registrations.items[existing].name);
            continue;
        }
        if (discovery_unique_name(context, candidate->suggested_name,
                                  &registrations,
                                  views, i, views[i].name,
                                  sizeof(views[i].name)) != 0 ||
            registration_list_reserve(&registrations, registrations.count + 1) != 0) {
            print_error("cannot allocate a unique discovered JDK name");
            strcpy(views[i].status, "error");
            write_failed = 1;
            continue;
        }
        memset(&registration, 0, sizeof(registration));
        strcpy(registration.name, views[i].name);
        strcpy(registration.home, candidate->home);
        registration.managed = 0;
        if (registration_exists(context, registration.name)) {
            print_error("a discovered JDK name became occupied while registering");
            strcpy(views[i].status, "error");
            write_failed = 1;
            continue;
        }
        if (write_registration(context, &registration) != 0) {
            print_platform_error("cannot register discovered JDK");
            strcpy(views[i].status, "error");
            write_failed = 1;
            continue;
        }
        registrations.items[registrations.count++] = registration;
        discovery_set_registered_view(&views[i], registration.name);
        ++added;
    }
    print_discovery_results(&discovered, views);
    printf("Registered %lu new JDK%s.\n", (unsigned long)added,
           jvman_i18n_plural_suffix((unsigned long)added));
    result = write_failed ? 1 : 0;

done:
    if (lock_held) platform_lock_release(&lock);
    registration_list_free(&registrations);
    free(views);
    jvman_discovery_list_free(&discovered);
    return result;
}

static int command_current(const JvmanContext *context) {
    char name[JVMAN_NAME_MAX + 1];
    JvmanRegistration registration;
    if (current_name(context, name, sizeof(name)) != 0 ||
        read_registration(context, name, &registration) != 0) {
        puts("none");
        return 0;
    }
    printf("%s\n", name);
    return 0;
}

static int command_which(const JvmanContext *context, const char *requested) {
    char name[JVMAN_NAME_MAX + 1];
    JvmanRegistration registration;
    if (requested) {
        if (strlen(requested) > JVMAN_NAME_MAX) return print_error("invalid version name");
        strcpy(name, requested);
    } else if (current_name(context, name, sizeof(name)) != 0) {
        return print_error("no JDK is currently selected");
    }
    if (read_registration(context, name, &registration) != 0) {
        return print_error("unknown JDK version");
    }
    printf("%s\n", registration.home);
    return 0;
}

static int command_remove(const JvmanContext *context, const char *name) {
    JvmanRegistration registration;
    PlatformLock lock;
    char active[JVMAN_NAME_MAX + 1] = {0};
    char config[JVMAN_PATH_MAX];
    char install_dir[JVMAN_PATH_MAX];
    int result = 1;
    if (!jvman_valid_name(name)) return print_error("unknown JDK version");
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    if (read_registration(context, name, &registration) != 0) {
        print_error("unknown JDK version");
        goto done;
    }
    current_name(context, active, sizeof(active));
    if (jvman_name_equal(active, name)) {
        print_error("cannot remove the current JDK; switch to another version first");
        goto done;
    }
    if (registration.managed) {
        char canonical_install[JVMAN_PATH_MAX];
        char canonical_home[JVMAN_PATH_MAX];
        char linked_home[JVMAN_PATH_MAX];
        if (jvman_path_join(install_dir, sizeof(install_dir), context->jdks, name) != 0 ||
            platform_absolute_path(install_dir, canonical_install, sizeof(canonical_install)) != 0 ||
            platform_absolute_path(registration.home, canonical_home, sizeof(canonical_home)) != 0 ||
            !(jvman_path_equal(canonical_install, canonical_home) ||
              jvman_path_is_within(canonical_install, canonical_home))) {
            print_error("managed registration is corrupt; refusing to delete files");
            goto done;
        }
        if (platform_absolute_path(context->current_link, linked_home,
                                   sizeof(linked_home)) == 0 &&
            (jvman_path_equal(canonical_install, linked_home) ||
             jvman_path_is_within(canonical_install, linked_home))) {
            print_error("cannot remove a managed JDK used by the current link");
            goto done;
        }
        if (platform_remove_tree(canonical_install) != 0) {
            print_platform_error("cannot remove managed JDK files");
            goto done;
        }
    }
    if (registration_path(context, name, config, sizeof(config)) != 0 ||
        platform_remove_file(config) != 0) {
        print_platform_error("cannot remove registration");
        goto done;
    }
    printf("Removed %s%s\n", name,
           jvman_i18n_text(registration.managed
                               ? " and its managed files" : " registration"));
    result = 0;
done:
    platform_lock_release(&lock);
    return result;
}

int jvman_uninstall_run_cli(void) {
    if (platform_launch_jvman_uninstaller() != 0) {
        return print_platform_error("cannot start jvman uninstaller");
    }
    jvman_i18n_puts("The jvman uninstaller was started.");
    return 0;
}

static int command_exec(const JvmanContext *context, const char *name, char **command) {
    JvmanRegistration registration;
    char bin[JVMAN_PATH_MAX];
    int result;
    if (!command || !command[0]) return print_error("exec requires a command");
    if (read_registration(context, name, &registration) != 0) {
        return print_error("unknown JDK version");
    }
    if (find_jdk_home(registration.home, registration.home, sizeof(registration.home)) != 0 ||
        jvman_path_join(bin, sizeof(bin), registration.home, "bin") != 0) {
        return print_error("registered path is not a valid JDK anymore");
    }
    if (platform_set_environment("JAVA_HOME", registration.home) != 0 ||
        platform_prepend_path(bin) != 0) {
        return print_platform_error("cannot construct child environment");
    }
    result = platform_spawn_wait(command);
    if (result < 0) return print_platform_error("cannot execute command");
    return result;
}

static int command_init_cmd(const JvmanContext *context) {
    const char *path = getenv("PATH");
    const char *updated_path;
    char current_bin[JVMAN_PATH_MAX];
    char *allocated = NULL;
    size_t current_length;
    size_t path_length = path ? strlen(path) : 0;
    size_t updated_length;
    if (!context ||
        jvman_path_join(current_bin, sizeof(current_bin), context->current_link,
                        "bin") != 0 ||
        strpbrk(context->current_link, "%!\"\r\n") != NULL ||
        (path && strpbrk(path, "%!\"\r\n") != NULL)) {
        return print_error(
            "the current shell environment cannot be activated safely in CMD");
    }
    current_length = strlen(current_bin);
    if (path && *path && shell_path_starts_with(current_bin)) {
        updated_path = path;
        updated_length = path_length;
    } else {
        if (current_length > (size_t)-1 - path_length - 2u) {
            return print_error("the current shell PATH is too long for CMD");
        }
        updated_length = current_length + (path_length ? path_length + 1u : 0u);
        allocated = (char *)malloc(updated_length + 1u);
        if (!allocated) return print_error("out of memory");
        memcpy(allocated, current_bin, current_length);
        if (path_length) {
            allocated[current_length] = ';';
            memcpy(allocated + current_length + 1u, path, path_length);
        }
        allocated[updated_length] = '\0';
        updated_path = allocated;
    }
    if (strlen(context->current_link) + sizeof("set \"JAVA_HOME=\"") + 1u >
            8191u ||
        updated_length + sizeof("set \"PATH=\"") + 1u > 8191u) {
        free(allocated);
        return print_error("the current shell PATH is too long for CMD");
    }
    printf("set \"JAVA_HOME=%s\"\n", context->current_link);
    printf("set \"PATH=%s\"\n", updated_path);
    free(allocated);
    return 0;
}

static int command_init(const JvmanContext *context, const char *shell) {
    char quoted[JVMAN_PATH_MAX * 4 + 3];
    if (!shell || !*shell) {
#if defined(_WIN32)
        shell = "powershell";
#else
        shell = "sh";
#endif
    }
    if (strcmp(shell, "powershell") == 0 || strcmp(shell, "pwsh") == 0) {
        jvman_shell_quote_powershell(context->current_link, quoted, sizeof(quoted));
        printf("$env:JAVA_HOME = %s\n", quoted);
        puts("$jvmanJavaBin = Join-Path $env:JAVA_HOME 'bin'");
        puts("$jvmanPathSeparator = [IO.Path]::PathSeparator");
        puts("$jvmanPathParts = @($env:Path -split [regex]::Escape([string]$jvmanPathSeparator) | Where-Object { $_ -and $_ -ne $jvmanJavaBin })");
        puts("$env:Path = (@($jvmanJavaBin) + $jvmanPathParts) -join $jvmanPathSeparator");
        puts("Remove-Variable jvmanJavaBin,jvmanPathSeparator,jvmanPathParts -ErrorAction SilentlyContinue");
        return 0;
    }
    if (strcmp(shell, "cmd") == 0) {
        return command_init_cmd(context);
    }
    if (strcmp(shell, "sh") == 0 || strcmp(shell, "bash") == 0 ||
        strcmp(shell, "zsh") == 0) {
        jvman_shell_quote_sh(context->current_link, quoted, sizeof(quoted));
        printf("export JAVA_HOME=%s\n", quoted);
        puts("if [ -n \"${PATH:-}\" ]; then case \"$PATH\" in \"$JAVA_HOME/bin\"|\"$JAVA_HOME/bin:\"*) ;; *) export PATH=\"$JAVA_HOME/bin:$PATH\" ;; esac; else export PATH=\"$JAVA_HOME/bin\"; fi");
        return 0;
    }
    return print_error("unsupported shell; use powershell, cmd, or sh");
}

static int command_doctor(const JvmanContext *context) {
    int warnings = 0;
    char active[JVMAN_NAME_MAX + 1] = {0};
    char expected_bin[JVMAN_PATH_MAX];
    char expected_java[JVMAN_PATH_MAX];
    char expected_java_canonical[JVMAN_PATH_MAX];
    char actual_java[JVMAN_PATH_MAX];
    const char *java_home = getenv("JAVA_HOME");
    printf("[ok]   data home: %s\n", context->root);
    if (current_name(context, active, sizeof(active)) == 0) {
        JvmanRegistration registration;
        char registered_home[JVMAN_PATH_MAX];
        char linked_home[JVMAN_PATH_MAX];
        if (read_registration(context, active, &registration) == 0 &&
            platform_absolute_path(registration.home, registered_home,
                                   sizeof(registered_home)) == 0 &&
            platform_absolute_path(context->current_link, linked_home,
                                   sizeof(linked_home)) == 0 &&
            platform_is_directory(registration.home) &&
            platform_is_directory(context->current_link) &&
            jvman_path_equal(registered_home, linked_home)) {
            printf("[ok]   current: %s -> %s\n", active, registration.home);
        } else {
            printf("[warn] current state and directory link do not match\n");
            ++warnings;
        }
    } else {
        printf("[warn] no current JDK selected\n");
        ++warnings;
    }
    if (java_home && jvman_path_equal(java_home, context->current_link)) {
        printf("[ok]   JAVA_HOME points to the stable current path\n");
    } else {
        printf("[warn] JAVA_HOME is %s; expected %s\n",
               java_home && *java_home ? java_home : jvman_i18n_text("not set"),
               context->current_link);
        ++warnings;
    }
    if (jvman_path_join(expected_bin, sizeof(expected_bin), context->current_link,
                        "bin") == 0 &&
        jvman_path_join(expected_java, sizeof(expected_java), expected_bin,
                        JVMAN_JAVA_EXE) == 0 &&
        platform_absolute_path(expected_java, expected_java_canonical,
                               sizeof(expected_java_canonical)) == 0 &&
        platform_find_executable(JVMAN_JAVA_EXE, actual_java, sizeof(actual_java)) == 0) {
        if (shell_path_starts_with(expected_bin) &&
            jvman_path_equal(actual_java, expected_java_canonical)) {
            printf("[ok]   PATH resolves java through jvman\n");
        } else {
            printf("[warn] PATH resolves java to %s\n", actual_java);
            ++warnings;
        }
    } else {
        printf("[warn] java is not available on PATH\n");
        ++warnings;
    }
#if defined(_WIN32)
    printf("[ok]   downloader: Windows WinHTTP\n");
    if (platform_find_trusted_executable("tar.exe", actual_java, sizeof(actual_java)) == 0)
        printf("[ok]   extractor: %s\n", actual_java);
    else { printf("[warn] tar.exe is required for installs\n"); ++warnings; }
#else
    if (platform_find_trusted_executable("curl", actual_java, sizeof(actual_java)) == 0)
        printf("[ok]   downloader: %s\n", actual_java);
    else { printf("[warn] curl is required for remote installs\n"); ++warnings; }
    if (platform_find_trusted_executable("tar", actual_java, sizeof(actual_java)) == 0)
        printf("[ok]   extractor: %s\n", actual_java);
    else { printf("[warn] tar is required for installs\n"); ++warnings; }
#endif
    if (warnings) {
        printf("%d warning%s found. Evaluate `jvman init` in your shell after selecting a JDK.\n",
               warnings,
               jvman_i18n_plural_suffix((unsigned long)warnings));
        return 1;
    }
    jvman_i18n_puts("No problems found.");
    return 0;
}

static int select_extracted_directory(const char *staging, char *out, size_t out_size) {
    char **entries = NULL;
    size_t count = 0;
    size_t i;
    size_t directories = 0;
    char candidate[JVMAN_PATH_MAX] = {0};
    if (platform_list_directory(staging, &entries, &count) != 0) return -1;
    for (i = 0; i < count; ++i) {
        char path[JVMAN_PATH_MAX];
        if (jvman_path_join(path, sizeof(path), staging, entries[i]) == 0 &&
            platform_is_directory(path)) {
            ++directories;
            if (strlen(path) < sizeof(candidate)) strcpy(candidate, path);
        }
    }
    platform_free_directory_list(entries, count);
    if (directories != 1 || !candidate[0] || strlen(candidate) + 1 > out_size) return -1;
    strcpy(out, candidate);
    return 0;
}

static int valid_sha256_text(const char *text) {
    size_t i;
    if (!text || strlen(text) != 64) return 0;
    for (i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}

static char *read_file_bounded(const char *path, size_t limit) {
    FILE *file;
    long raw_size;
    size_t size;
    char *data;
    int trailing;
    if (!path || limit == 0 || !(file = fopen(path, "rb"))) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (raw_size = ftell(file)) < 0 ||
        (unsigned long)raw_size > (unsigned long)limit ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    size = (size_t)raw_size;
    data = (char *)malloc(size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if ((size && fread(data, 1, size, file) != size) ||
        (trailing = fgetc(file)) != EOF || ferror(file) ||
        memchr(data, '\0', size) != NULL) {
        free(data);
        fclose(file);
        return NULL;
    }
    data[size] = '\0';
    fclose(file);
    return data;
}

static int source_entry_compare(const void *left, const void *right) {
    const char *const *left_name = (const char *const *)left;
    const char *const *right_name = (const char *const *)right;
    return strcmp(*left_name, *right_name);
}

static int custom_source_config_path(const JvmanContext *context,
                                     const char *name,
                                     char *out, size_t out_size) {
    char filename[JVMAN_NAME_MAX + 8];
    if (!context || !jvman_valid_name(name) ||
        snprintf(filename, sizeof(filename), "%s.conf", name) >=
            (int)sizeof(filename)) {
        return -1;
    }
    return jvman_path_join(out, out_size, context->sources, filename);
}

static int parse_custom_source_config(char *text,
                                      char *url_template,
                                      size_t template_size) {
    char *cursor = text;
    int found_type = 0;
    int found_url = 0;
    if (!text || !url_template || template_size == 0) return -1;
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        size_t length;
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }
        length = strlen(line);
        if (length && line[length - 1] == '\r') line[--length] = '\0';
        if (length == 0) return -1;
        if (strcmp(line, "type=adoptium") == 0) {
            if (found_type) return -1;
            found_type = 1;
        } else if (strncmp(line, "url=", 4) == 0) {
            const char *value = line + 4;
            if (found_url || strlen(value) >= template_size ||
                !jvman_download_source_valid_custom_template(value)) {
                return -1;
            }
            strcpy(url_template, value);
            found_url = 1;
        } else {
            return -1;
        }
    }
    return found_type && found_url ? 0 : -1;
}

static const JvmanDownloadSource *download_source_registry_find(
    const DownloadSourceRegistry *registry, const char *name);

static int download_source_registry_load(const JvmanContext *context,
                                         DownloadSourceRegistry *registry) {
    char **entries = NULL;
    size_t entry_count = 0;
    size_t i;
    if (!context || !registry) return -1;
    memset(registry, 0, sizeof(*registry));
    for (i = 0; i < jvman_download_source_count(); ++i) {
        const JvmanDownloadSource *source = jvman_download_source_at(i);
        if (!source || registry->count >= JVMAN_DOWNLOAD_SOURCE_LIMIT) return -1;
        registry->items[registry->count++] = source;
    }
    if (!platform_path_exists(context->sources)) return 0;
    if (!platform_is_directory(context->sources) ||
        platform_list_directory(context->sources, &entries,
                                &entry_count) != 0) {
        return -1;
    }
    qsort(entries, entry_count, sizeof(*entries), source_entry_compare);
    for (i = 0; i < entry_count; ++i) {
        char path[JVMAN_PATH_MAX];
        char name[JVMAN_NAME_MAX + 1];
        char *text;
        size_t filename_length;
        size_t name_length;
        size_t index;
        JvmanDownloadSource *source;
        if (!jvman_ends_with(entries[i], ".conf")) continue;
        filename_length = strlen(entries[i]);
        name_length = filename_length - 5;
        if (name_length == 0 || name_length > JVMAN_NAME_MAX) goto invalid;
        memcpy(name, entries[i], name_length);
        name[name_length] = '\0';
        if (!jvman_valid_name(name) ||
            download_source_registry_find(registry, name) != NULL ||
            registry->custom_count >= JVMAN_CUSTOM_SOURCE_LIMIT ||
            registry->count >= JVMAN_DOWNLOAD_SOURCE_LIMIT ||
            jvman_path_join(path, sizeof(path), context->sources,
                            entries[i]) != 0 ||
            !platform_is_file(path) ||
            !(text = read_file_bounded(path, JVMAN_SOURCE_TEMPLATE_MAX + 128u))) {
            goto invalid;
        }
        index = registry->custom_count;
        if (parse_custom_source_config(
                text, registry->templates[index],
                sizeof(registry->templates[index])) != 0) {
            free(text);
            goto invalid;
        }
        free(text);
        strcpy(registry->names[index], name);
        source = &registry->custom[index];
        source->name = registry->names[index];
        source->label = "Custom: %s";
        source->kind = JVMAN_DOWNLOAD_SOURCE_CUSTOM_ADOPTIUM;
        source->distribution = NULL;
        source->metadata_template = registry->templates[index];
        registry->items[registry->count++] = source;
        ++registry->custom_count;
    }
    platform_free_directory_list(entries, entry_count);
    return 0;
invalid:
    platform_free_directory_list(entries, entry_count);
    return -1;
}

static const JvmanDownloadSource *download_source_registry_find(
    const DownloadSourceRegistry *registry, const char *name) {
    size_t i;
    if (!registry || !name || !*name) return NULL;
    for (i = 0; i < registry->count; ++i) {
        if (strcmp(registry->items[i]->name, name) == 0) {
            return registry->items[i];
        }
    }
    return NULL;
}

static int read_download_source_name(const JvmanContext *context,
                                     char *out, size_t out_size) {
    char *name = NULL;
    size_t name_size;
    if (!context || !out || out_size == 0) return -1;
    if (!platform_path_exists(context->download_source)) {
        const char *default_name = jvman_download_source_default()->name;
        if (strlen(default_name) >= out_size) return -1;
        strcpy(out, default_name);
        return 0;
    }
    if (!platform_is_file(context->download_source) ||
        !(name = read_file_bounded(context->download_source, 64))) {
        return -1;
    }
    name_size = strlen(name);
    while (name_size && (name[name_size - 1] == '\r' ||
                         name[name_size - 1] == '\n')) {
        name[--name_size] = '\0';
    }
    if (name_size == 0 || name_size >= out_size || strchr(name, '\r') ||
        strchr(name, '\n')) {
        free(name);
        return -1;
    }
    strcpy(out, name);
    free(name);
    return 0;
}

static int read_download_source(const JvmanContext *context,
                                const DownloadSourceRegistry *registry,
                                const JvmanDownloadSource **source_out) {
    char name[JVMAN_NAME_MAX + 1];
    const JvmanDownloadSource *source;
    if (!context || !registry || !source_out) return -1;
    if (read_download_source_name(context, name, sizeof(name)) != 0) return -1;
    source = download_source_registry_find(registry, name);
    if (!source) return -1;
    *source_out = source;
    return 0;
}

static int command_source(const JvmanContext *context, int argc, char **argv) {
    DownloadSourceRegistry registry;
    const JvmanDownloadSource *active;
    const JvmanDownloadSource *selected;
    PlatformLock lock;
    size_t i;
    int result = 1;
    if (argc == 3 && strcmp(argv[2], "--reset") == 0) {
        char label[JVMAN_SOURCE_LABEL_MAX];
        selected = jvman_download_source_default();
        if (acquire_lock(context, &lock) != 0) {
            return print_platform_error("cannot lock state");
        }
        if (platform_path_exists(context->download_source) &&
            platform_remove_file(context->download_source) != 0) {
            print_platform_error("cannot reset download source");
            goto done;
        }
        printf("Download source: %s (%s)\n", selected->name,
               download_source_display_label(selected, label, sizeof(label)));
        result = 0;
        goto done;
    }
    if (argc == 4 && strcmp(argv[2], "remove") == 0) {
        char path[JVMAN_PATH_MAX];
        char active_name[JVMAN_NAME_MAX + 1];
        const char *name = argv[3];
        if (!jvman_valid_name(name) || jvman_download_source_find(name) ||
            custom_source_config_path(context, name, path, sizeof(path)) != 0 ||
            !platform_is_file(path)) {
            return print_error("only an existing custom source can be removed");
        }
        if (read_download_source_name(context, active_name,
                                      sizeof(active_name)) != 0) {
            return print_error("download source configuration is invalid");
        }
        if (strcmp(active_name, name) == 0) {
            return print_error("select another source before removing the active custom source");
        }
        if (acquire_lock(context, &lock) != 0) {
            return print_platform_error("cannot lock state");
        }
        if (platform_remove_file(path) != 0) {
            print_platform_error("cannot remove custom download source");
            goto done;
        }
        printf("Removed custom download source: %s\n", name);
        result = 0;
        goto done;
    }
    if (download_source_registry_load(context, &registry) != 0) {
        return print_error("custom download source configuration is invalid");
    }
    if (argc == 5 && strcmp(argv[2], "add") == 0) {
        char path[JVMAN_PATH_MAX];
        char *content;
        size_t content_size;
        const char *name = argv[3];
        const char *url_template = argv[4];
        if (!jvman_valid_name(name) ||
            download_source_registry_find(&registry, name) != NULL) {
            return print_error("custom source name is invalid or already exists");
        }
        if (registry.custom_count >= JVMAN_CUSTOM_SOURCE_LIMIT ||
            registry.count >= JVMAN_DOWNLOAD_SOURCE_LIMIT) {
            return print_error("custom download source limit reached");
        }
        if (!jvman_download_source_valid_custom_template(url_template)) {
            return print_error(
                "custom source URL must be HTTPS and include {major}; supported placeholders are {major}, {os}, {arch}, and {archive}");
        }
        content_size = strlen(url_template) + 32;
        content = (char *)malloc(content_size);
        if (!content ||
            snprintf(content, content_size, "type=adoptium\nurl=%s\n",
                     url_template) >= (int)content_size) {
            free(content);
            return print_error("custom source configuration is too large");
        }
        if (acquire_lock(context, &lock) != 0) {
            free(content);
            return print_platform_error("cannot lock state");
        }
        if (custom_source_config_path(context, name, path, sizeof(path)) != 0 ||
            platform_path_exists(path) ||
            platform_write_text_atomic(path, content) != 0) {
            free(content);
            print_platform_error("cannot save custom download source");
            goto done;
        }
        free(content);
        printf("Added custom download source: %s\n", name);
        result = 0;
        goto done;
    }
    if (argc == 2) {
        if (read_download_source(context, &registry, &active) != 0) {
            return print_error("download source configuration is invalid");
        }
        puts(active->name);
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "--list") == 0) {
        if (read_download_source(context, &registry, &active) != 0) {
            return print_error("download source configuration is invalid");
        }
        for (i = 0; i < registry.count; ++i) {
            char label[JVMAN_SOURCE_LABEL_MAX];
            const JvmanDownloadSource *item = registry.items[i];
            printf("%c %-12s %s\n", item == active ? '*' : ' ',
                   item->name,
                   download_source_display_label(item, label, sizeof(label)));
        }
        return 0;
    }
    if (argc != 3) {
        return print_error(
            "usage: jvman source [--list|--reset|<name>|add <name> <HTTPS-template>|remove <name>]");
    }
    selected = download_source_registry_find(&registry, argv[2]);
    if (!selected) return print_error("unknown download source; use `jvman source --list`");
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    {
        char content[80];
        if (snprintf(content, sizeof(content), "%s\n", selected->name) >=
                (int)sizeof(content) ||
            platform_write_text_atomic(context->download_source, content) != 0) {
            print_platform_error("cannot save download source");
            goto done;
        }
    }
    {
        char label[JVMAN_SOURCE_LABEL_MAX];
        printf("Download source: %s (%s)\n", selected->name,
               download_source_display_label(selected, label, sizeof(label)));
    }
    result = 0;
done:
    platform_lock_release(&lock);
    return result;
}

static int download_file(const char *url, const char *destination,
                         size_t limit, int show_progress);
static int download_file_timeout(const char *url, const char *destination,
                                 size_t limit, unsigned int timeout_seconds);

static int resolve_fastest_download_source(
    const DownloadSourceRegistry *registry, int major,
    const char *metadata_path,
    const JvmanDownloadSource **selected_out,
    char *download_url, size_t url_size,
    char *checksum, size_t checksum_size,
    char *exact_version, size_t version_size);

static int download_metadata(const JvmanDownloadSource *source, int major,
                             const char *metadata_path,
                             char *download_url, size_t url_size,
                             char *checksum, size_t checksum_size,
                             char *exact_version, size_t version_size) {
    char metadata_url[2048];
    char detail_url[2048];
    char original_url[2048];
    char *json = NULL;
    int result;
    if (jvman_download_source_build_metadata_url(
            source, major, platform_os_name(), platform_arch_name(),
            platform_archive_extension(), metadata_url,
            sizeof(metadata_url)) != 0 ||
        download_file_timeout(metadata_url, metadata_path,
                              JVMAN_METADATA_LIMIT, 15u) != 0 ||
        !(json = read_file_bounded(metadata_path, JVMAN_METADATA_LIMIT))) {
        return -1;
    }
    result = jvman_download_source_parse_catalog(
        source, json, major, detail_url, sizeof(detail_url),
        exact_version, version_size);
    if (result == 0 && detail_url[0]) {
        free(json);
        json = NULL;
        if (platform_remove_file(metadata_path) != 0 ||
            download_file_timeout(detail_url, metadata_path,
                                  JVMAN_METADATA_LIMIT, 15u) != 0 ||
            !(json = read_file_bounded(metadata_path,
                                       JVMAN_METADATA_LIMIT))) {
            return -1;
        }
    }
    if (result == 0) {
        result = jvman_download_source_parse_package(
            source, json, original_url, sizeof(original_url),
            checksum, checksum_size);
    }
    if (result == 0) {
        result = jvman_download_source_rewrite_package_url(
            source, major, platform_os_name(), platform_arch_name(),
            original_url, download_url, url_size);
    }
    free(json);
    return result;
}

static int download_file(const char *url, const char *destination,
                         size_t limit, int show_progress) {
    return platform_https_download(url, destination, limit, show_progress);
}

static int download_file_timeout(const char *url, const char *destination,
                                 size_t limit, unsigned int timeout_seconds) {
    return platform_https_download_timeout(url, destination, limit, 0,
                                           timeout_seconds);
}

static int resolve_fastest_download_source(
    const DownloadSourceRegistry *registry, int major,
    const char *metadata_path,
    const JvmanDownloadSource **selected_out,
    char *download_url, size_t url_size,
    char *checksum, size_t checksum_size,
    char *exact_version, size_t version_size) {
    typedef struct ResolvedSource {
        JvmanDownloadSourceProbe probe;
        char url[2048];
        char checksum[80];
        char version[128];
    } ResolvedSource;
    ResolvedSource *resolved_sources = NULL;
    JvmanDownloadSourceProbe *probes = NULL;
    const JvmanDownloadSource *best;
    ResolvedSource *best_result = NULL;
    size_t resolved_count = 0;
    size_t i;
    int result = -1;
    if (!registry || !metadata_path || !selected_out || !download_url ||
        url_size == 0 || !checksum || checksum_size == 0 || !exact_version ||
        version_size == 0) {
        return -1;
    }
    jvman_i18n_puts("Testing download sources...");
    fflush(stdout);
    if (registry->count > JVMAN_DOWNLOAD_SOURCE_LIMIT) return -1;
    resolved_sources = calloc(JVMAN_DOWNLOAD_SOURCE_LIMIT,
                              sizeof(*resolved_sources));
    probes = calloc(JVMAN_DOWNLOAD_SOURCE_LIMIT, sizeof(*probes));
    if (!resolved_sources || !probes) goto cleanup;
    for (i = 0; i < registry->count; ++i) {
        const JvmanDownloadSource *source = registry->items[i];
        ResolvedSource *candidate;
        uint64_t started;
        uint64_t finished;
        uint64_t elapsed;
        int resolved;
        if (!source || source->kind == JVMAN_DOWNLOAD_SOURCE_AUTO) continue;
        candidate = &resolved_sources[resolved_count++];
        candidate->probe.source = source;
        if ((platform_path_exists(metadata_path) &&
             platform_remove_file(metadata_path) != 0) ||
            platform_monotonic_millis(&started) != 0) {
            goto cleanup;
        }
        platform_clear_error();
        resolved = download_metadata(
            source, major, metadata_path, candidate->url,
            sizeof(candidate->url), candidate->checksum,
            sizeof(candidate->checksum), candidate->version,
            sizeof(candidate->version));
        if (resolved == 0) {
            resolved = platform_https_probe(candidate->url,
                                            JVMAN_DOWNLOAD_PROBE_SIZE,
                                            JVMAN_DOWNLOAD_PROBE_TIMEOUT);
        }
        if (platform_monotonic_millis(&finished) != 0 || finished < started) {
            goto cleanup;
        }
        elapsed = finished - started;
        candidate->probe.elapsed_millis = elapsed;
        candidate->probe.available = resolved == 0;
        probes[resolved_count - 1] = candidate->probe;
        if (resolved != 0) {
            const char *reason = platform_last_error();
            if (strcmp(reason, "unknown platform error") == 0) {
                reason = jvman_i18n_text("invalid metadata");
            }
            printf("  %-10s %" PRIu64 " ms, unavailable (%s)\n",
                   source->name, elapsed,
                   reason);
            fflush(stdout);
            continue;
        }
        printf("  %-10s %" PRIu64 " ms\n", source->name, elapsed);
        fflush(stdout);
    }
    best = jvman_download_source_select_fastest(
        probes, resolved_count);
    if (!best) goto cleanup;
    for (i = 0; i < resolved_count; ++i) {
        if (resolved_sources[i].probe.source == best) {
            best_result = &resolved_sources[i];
            break;
        }
    }
    if (!best_result || strlen(best_result->url) >= url_size ||
        strlen(best_result->checksum) >= checksum_size ||
        strlen(best_result->version) >= version_size) {
        goto cleanup;
    }
    strcpy(download_url, best_result->url);
    strcpy(checksum, best_result->checksum);
    strcpy(exact_version, best_result->version);
    *selected_out = best;
    printf("Selected download source: %s (%" PRIu64 " ms)\n",
           best->name, best_result->probe.elapsed_millis);
    fflush(stdout);
    result = 0;
cleanup:
    free(resolved_sources);
    free(probes);
    return result;
}

static int extract_archive(const char *archive, const char *destination) {
#if defined(_WIN32)
    char extractor_name[] = "tar.exe";
#else
    char extractor_name[] = "tar";
#endif
    char extractor[JVMAN_PATH_MAX];
    char *arguments[] = {extractor, "-xf", (char *)archive, "-C", (char *)destination, NULL};
    if (platform_find_trusted_executable(extractor_name, extractor,
                                         sizeof(extractor)) != 0) return -1;
    return platform_spawn_wait(arguments);
}

static int command_install(const JvmanContext *context, const char *version,
                           const char *name_option, const char *archive_option,
                           const char *checksum_option,
                           const char *source_option, int no_persist) {
    char name[JVMAN_NAME_MAX + 1];
    char metadata[JVMAN_PATH_MAX] = {0};
    char archive[JVMAN_PATH_MAX] = {0};
    char stage[JVMAN_PATH_MAX] = {0};
    char extracted[JVMAN_PATH_MAX] = {0};
    char detected_home[JVMAN_PATH_MAX] = {0};
    char install_dir[JVMAN_PATH_MAX] = {0};
    char final_home[JVMAN_PATH_MAX] = {0};
    char local_source[JVMAN_PATH_MAX] = {0};
    char url[2048] = {0};
    char checksum[80] = {0};
    char exact_version[128] = {0};
    char expected_extension[16];
    unsigned char digest[32];
    int major = 0;
    int remote = archive_option == NULL;
    int moved = 0;
    int result = 1;
    PlatformLock lock;
    JvmanRegistration registration;
    DownloadSourceRegistry source_registry;
    const JvmanDownloadSource *download_source = NULL;
    const char *archive_source = archive;

    if (!version || !jvman_valid_name(version)) return print_error("invalid version");
    if (name_option) {
        if (!jvman_valid_name(name_option)) return print_error("invalid --name value");
        strcpy(name, name_option);
    } else {
        strcpy(name, version);
    }
    if (remote && jvman_parse_major(version, &major) != 0) {
        return print_error("remote install currently accepts a Java major version, for example 17 or 21");
    }
    if (remote && (strcmp(platform_arch_name(), "unsupported") == 0 ||
                   strcmp(platform_os_name(), "unsupported") == 0)) {
        return print_error("remote install is not supported on this operating system or CPU architecture");
    }
    if (checksum_option && !valid_sha256_text(checksum_option)) {
        return print_error("--sha256 must contain 64 hexadecimal characters");
    }
    if (!remote && source_option) {
        return print_error("--source is only valid for remote installs");
    }
    if (remote) {
        if (download_source_registry_load(context, &source_registry) != 0) {
            return print_error("custom download source configuration is invalid");
        }
        if (source_option) {
            download_source = download_source_registry_find(
                &source_registry, source_option);
        } else if (read_download_source(context, &source_registry,
                                        &download_source) != 0) {
            return print_error("download source configuration is invalid");
        }
        if (!download_source) {
            return print_error("unknown download source; use `jvman source --list`");
        }
    }
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    if (registration_exists(context, name)) {
        print_error("that version name is already registered");
        goto cleanup;
    }
    if (jvman_path_join(install_dir, sizeof(install_dir), context->jdks, name) != 0 ||
        platform_path_exists(install_dir)) {
        print_error("managed install directory already exists");
        goto cleanup;
    }
    snprintf(expected_extension, sizeof(expected_extension), "%s", platform_archive_extension());
    if (snprintf(metadata, sizeof(metadata), "%s%c%s-%lu.json", context->cache,
                 JVMAN_DIR_SEP, name, platform_process_id()) >= (int)sizeof(metadata) ||
        snprintf(archive, sizeof(archive), "%s%c%s-%lu%s", context->cache,
                 JVMAN_DIR_SEP, name, platform_process_id(), expected_extension) >= (int)sizeof(archive) ||
        snprintf(stage, sizeof(stage), "%s%cinstall-%s-%lu", context->staging,
                 JVMAN_DIR_SEP, name, platform_process_id()) >= (int)sizeof(stage)) {
        print_error("installation path is too long");
        goto cleanup;
    }
    if (!remote &&
        (platform_absolute_path(archive_option, local_source, sizeof(local_source)) != 0 ||
         !platform_is_file(local_source))) {
        print_error("local archive does not exist");
        goto cleanup;
    }
    if (!remote && jvman_path_equal(local_source, archive)) {
        print_error("local archive conflicts with the temporary cache path");
        goto cleanup;
    }
    platform_remove_file(metadata);
    platform_remove_file(archive);
    if (platform_path_exists(stage) && platform_remove_tree(stage) != 0) {
        print_platform_error("cannot clear stale staging directory");
        goto cleanup;
    }
    if (platform_mkdirs(stage) != 0) {
        print_platform_error("cannot create staging directory");
        goto cleanup;
    }
    if (remote) {
        int automatic = download_source->kind == JVMAN_DOWNLOAD_SOURCE_AUTO;
        int resolve_result;
        if (automatic) {
            printf("Resolving Temurin %d for %s/%s automatically...\n",
                   major, platform_os_name(), platform_arch_name());
            fflush(stdout);
            resolve_result = resolve_fastest_download_source(
                &source_registry, major, metadata, &download_source,
                url, sizeof(url),
                checksum, sizeof(checksum), exact_version,
                sizeof(exact_version));
        } else {
            char label[JVMAN_SOURCE_LABEL_MAX];
            printf("Resolving Temurin %d for %s/%s via %s...\n", major,
                   platform_os_name(), platform_arch_name(),
                   download_source_display_label(download_source, label,
                                                 sizeof(label)));
            fflush(stdout);
            resolve_result = download_metadata(
                download_source, major, metadata, url, sizeof(url), checksum,
                sizeof(checksum), exact_version, sizeof(exact_version));
        }
        if (resolve_result != 0) {
            print_error(automatic
                            ? "could not resolve a Temurin package from any available source"
                            : "could not resolve a Temurin package from the selected source");
            goto cleanup;
        }
        printf("Downloading Temurin %s...\n", exact_version);
        fflush(stdout);
        platform_clear_error();
        if (download_file(url, archive, JVMAN_JDK_ARCHIVE_LIMIT, 1) != 0) {
            print_platform_error("download failed");
            goto cleanup;
        }
    } else {
        if (platform_copy_file(local_source, archive) != 0) {
            print_platform_error("cannot snapshot local archive");
            goto cleanup;
        }
        if (checksum_option) strcpy(checksum, checksum_option);
    }
    if (checksum[0]) {
        if (jvman_sha256_file(archive_source, digest) != 0 ||
            !jvman_hex_equal(digest, sizeof(digest), checksum)) {
            print_error("archive SHA-256 verification failed");
            goto cleanup;
        }
        if (remote && checksum_option &&
            !jvman_hex_equal(digest, sizeof(digest), checksum_option)) {
            print_error("archive does not match the SHA-256 pinned on the command line");
            goto cleanup;
        }
        jvman_i18n_puts("Archive checksum verified.");
    }
    jvman_i18n_puts("Extracting JDK...");
    if (extract_archive(archive_source, stage) != 0) {
        print_error("archive extraction failed");
        goto cleanup;
    }
    if (select_extracted_directory(stage, extracted, sizeof(extracted)) != 0) {
        print_error("archive must contain one top-level JDK directory");
        goto cleanup;
    }
    if (find_jdk_home(extracted, detected_home, sizeof(detected_home)) != 0) {
        print_error("archive does not contain a valid JDK");
        goto cleanup;
    }
    if (jvman_path_equal(extracted, detected_home)) {
        strcpy(final_home, install_dir);
    } else if (jvman_path_is_within(extracted, detected_home) &&
               snprintf(final_home, sizeof(final_home), "%s%s", install_dir,
                        detected_home + strlen(extracted)) < (int)sizeof(final_home)) {
        /* final_home now preserves macOS Contents/Home below the install root. */
    } else {
        print_error("invalid JDK home inside archive");
        goto cleanup;
    }
    if (platform_move(extracted, install_dir) != 0) {
        print_platform_error("cannot commit installed JDK");
        goto cleanup;
    }
    moved = 1;
    {
        char canonical_home[JVMAN_PATH_MAX];
        if (platform_absolute_path(final_home, canonical_home, sizeof(canonical_home)) != 0) {
            print_platform_error("cannot canonicalize installed JDK home");
            goto cleanup;
        }
        strcpy(final_home, canonical_home);
    }
    strcpy(registration.name, name);
    strcpy(registration.home, final_home);
    registration.managed = 1;
    if (write_registration(context, &registration) != 0) {
        print_platform_error("cannot save installed JDK registration");
        goto cleanup;
    }
    printf("Installed %s -> %s\n", name, final_home);
    printf("Run `jvman use %s` to activate it.\n", name);
    if (!no_persist) {
        JvmanPersistOptions persist_opts;
        persist_opts.ctx = context;
        persist_opts.replace_java_home = 0;
        persist_opts.quiet = 1;
        (void)jvman_persist_activate(&persist_opts);
    }
    result = 0;
cleanup:
    if (metadata[0]) platform_remove_file(metadata);
    if (archive[0]) platform_remove_file(archive);
    if (stage[0] && platform_path_exists(stage)) platform_remove_tree(stage);
    if (result != 0 && moved && install_dir[0]) platform_remove_tree(install_dir);
    platform_lock_release(&lock);
    return result;
}

static void print_usage(void) {
    jvman_i18n_puts("jvman " JVMAN_VERSION " - lightweight Java version manager");
    puts("");
    jvman_i18n_puts("Usage:");
    jvman_i18n_puts("  jvman install <major> [--name <name>] [--sha256 <hex>] [--source <name>] [--no-persist]");
    jvman_i18n_puts("  jvman install <name> --archive <file> [--sha256 <hex>] [--no-persist]");
    jvman_i18n_puts("  jvman add <name> <jdk-home>");
    jvman_i18n_puts("  jvman use <name> [--no-persist]");
    jvman_i18n_puts("  jvman activate [--replace-java-home]");
    jvman_i18n_puts("  jvman deactivate");
    jvman_i18n_puts("  jvman list");
    jvman_i18n_puts("  jvman discover [--register]");
    jvman_i18n_puts("  jvman source [--list|--reset|<name>|add <name> <HTTPS-template>|remove <name>]");
    jvman_i18n_puts("  jvman current");
    jvman_i18n_puts("  jvman which [name]");
    jvman_i18n_puts("  jvman remove <name>");
    jvman_i18n_puts("  jvman uninstall [<name>]");
    jvman_i18n_puts("  jvman exec <name> [--] <command> [args...]");
    jvman_i18n_puts("  jvman init [powershell|cmd|sh]");
    jvman_i18n_puts("  jvman doctor");
    jvman_i18n_puts("  jvman update [--check] [--version <version>] [--source <name>|--source-list]");
    jvman_i18n_puts("  jvman language [--list|en|zh-CN]");
    jvman_i18n_puts("  jvman home");
}

static int command_language(int argc, char **argv) {
    JvmanLanguage selected;
    int stored;
    if (argc == 2) {
        puts(jvman_i18n_language_tag());
        return 0;
    }
    if (argc != 3) {
        return print_error("usage: jvman language [--list|en|zh-CN]");
    }
    if (strcmp(argv[2], "--list") == 0) {
        jvman_i18n_puts("Languages:");
        printf("  %-7s %s\n", "en", jvman_i18n_text("English"));
        printf("  %-7s %s\n", "zh-CN",
               jvman_i18n_text("Simplified Chinese"));
        return 0;
    }
    if (jvman_i18n_parse_language(argv[2], &selected) != 0) {
        return print_error("unknown language; use `jvman language --list`");
    }
    stored = jvman_i18n_set_persistent(selected);
    if (stored == -2) {
        return print_error(
            "Persistent language selection is only supported on Windows; set JVMAN_LANG instead");
    }
    if (stored != 0) return print_error("cannot save language preference");
    printf("Language set to %s.\n", jvman_i18n_language_tag());
    return 0;
}

static int parse_install(const JvmanContext *context, int argc, char **argv) {
    const char *name = NULL;
    const char *archive = NULL;
    const char *checksum = NULL;
    const char *source = NULL;
    int no_persist = 0;
    int i;
    if (argc < 3) return print_error("install requires a version");
    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--archive") == 0 && i + 1 < argc) archive = argv[++i];
        else if (strcmp(argv[i], "--sha256") == 0 && i + 1 < argc) checksum = argv[++i];
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) source = argv[++i];
        else if (strcmp(argv[i], "--no-persist") == 0) no_persist = 1;
        else return print_error("unknown or incomplete install option");
    }
    return command_install(context, argv[2], name, archive, checksum, source,
                           no_persist);
}

static int parse_use(const JvmanContext *context, int argc, char **argv) {
    const char *name = NULL;
    int no_persist = 0;
    int i;
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--no-persist") == 0) no_persist = 1;
        else if (!name && argv[i][0] != '-') name = argv[i];
        else return print_error("usage: jvman use <name> [--no-persist]");
    }
    if (!name) return print_error("usage: jvman use <name> [--no-persist]");
    return command_use(context, name, no_persist);
}

static int command_activate(const JvmanContext *context, int argc, char **argv) {
    JvmanPersistOptions opts;
    PlatformLock lock;
    int result;
    int i;
    opts.ctx = context;
    opts.replace_java_home = 0;
    opts.quiet = 0;
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--replace-java-home") == 0) opts.replace_java_home = 1;
        else if (strcmp(argv[i], "--user") == 0) { /* accepted; default scope */ }
        else if (strcmp(argv[i], "--machine") == 0) {
            return print_error("machine-wide activation is not implemented in the CLI; use jvman-setup.exe");
        }
        else return print_error("usage: jvman activate [--replace-java-home]");
    }
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    result = jvman_persist_activate(&opts);
    platform_lock_release(&lock);
    if (result == 0) {
        int owned = 0;
        if (jvman_persist_is_owned(context, &owned) == 0 && owned) {
            jvman_i18n_puts("Persistent activation has been applied. New terminals and IDEs will pick up the change.");
        }
    }
    return result == 0 ? 0 : 1;
}

static int command_deactivate(const JvmanContext *context, int argc, char **argv) {
    PlatformLock lock;
    int result;
    (void)argv;
    if (argc != 2) return print_error("usage: jvman deactivate");
    if (acquire_lock(context, &lock) != 0) return print_platform_error("cannot lock state");
    result = jvman_persist_deactivate(context);
    platform_lock_release(&lock);
    return result == 0 ? 0 : 1;
}

int jvman_run(JvmanContext *context, int argc, char **argv) {
    const char *command;
    if (!context || argc < 2) {
        print_usage();
        return argc < 2 ? 0 : 1;
    }
    command = argv[1];
    if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0 ||
        strcmp(command, "-h") == 0) {
        print_usage(); return 0;
    }
    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0 ||
        strcmp(command, "-v") == 0) {
        puts(JVMAN_VERSION); return 0;
    }
    if (strcmp(command, "home") == 0) { puts(context->root); return 0; }
    if (strcmp(command, "language") == 0)
        return command_language(argc, argv);
    if (strcmp(command, "install") == 0) return parse_install(context, argc, argv);
    if (strcmp(command, "source") == 0) return command_source(context, argc, argv);
    if (strcmp(command, "add") == 0)
        return argc == 4 ? command_add(context, argv[2], argv[3]) :
               print_error("usage: jvman add <name> <jdk-home>");
    if (strcmp(command, "use") == 0 || strcmp(command, "default") == 0)
        return parse_use(context, argc, argv);
    if (strcmp(command, "activate") == 0)
        return command_activate(context, argc, argv);
    if (strcmp(command, "deactivate") == 0)
        return command_deactivate(context, argc, argv);
    if (strcmp(command, "list") == 0 || strcmp(command, "ls") == 0)
        return command_list(context);
    if (strcmp(command, "discover") == 0) {
        if (argc == 2) return command_discover(context, 0);
        if (argc == 3 && strcmp(argv[2], "--register") == 0) {
            return command_discover(context, 1);
        }
        return print_error("usage: jvman discover [--register]");
    }
    if (strcmp(command, "current") == 0) return command_current(context);
    if (strcmp(command, "which") == 0)
        return argc <= 3 ? command_which(context, argc == 3 ? argv[2] : NULL) :
               print_error("usage: jvman which [name]");
    if (strcmp(command, "remove") == 0)
        return argc == 3 ? command_remove(context, argv[2]) :
               print_error("usage: jvman remove <name>");
    if (strcmp(command, "uninstall") == 0) {
        if (argc == 2) return jvman_uninstall_run_cli();
        if (argc == 3) return command_remove(context, argv[2]);
        return print_error("usage: jvman uninstall [<name>]");
    }
    if (strcmp(command, "exec") == 0) {
        int command_index = 3;
        if (argc > 3 && strcmp(argv[3], "--") == 0) command_index = 4;
        return argc > command_index ? command_exec(context, argv[2], argv + command_index) :
               print_error("usage: jvman exec <name> [--] <command> [args...]");
    }
    if (strcmp(command, "init") == 0)
        return argc <= 3 ? command_init(context, argc == 3 ? argv[2] : NULL) :
               print_error("usage: jvman init [powershell|cmd|sh]");
    if (strcmp(command, "doctor") == 0) return command_doctor(context);
    if (strcmp(command, "update") == 0) return jvman_update_run_cli(argc, argv);
    print_usage();
    return print_error("unknown command");
}
