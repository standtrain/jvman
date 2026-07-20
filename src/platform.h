#ifndef JVMAN_PLATFORM_H
#define JVMAN_PLATFORM_H

#include "common.h"

#include <stddef.h>

typedef struct PlatformLock {
#if defined(_WIN32)
    void *handle;
#else
    int fd;
#endif
} PlatformLock;

typedef int (*PlatformPathVisitor)(const char *path, const char *vendor_hint,
                                   void *userdata);

const char *platform_last_error(void);
int platform_default_root(char *out, size_t out_size);
int platform_absolute_path(const char *path, char *out, size_t out_size);
int platform_mkdirs(const char *path);
int platform_is_directory(const char *path);
int platform_is_file(const char *path);
int platform_path_exists(const char *path);
int platform_remove_file(const char *path);
int platform_copy_file(const char *source, const char *destination);
int platform_remove_tree(const char *path);
int platform_move(const char *source, const char *destination);
int platform_write_text_atomic(const char *path, const char *text);
int platform_read_line(const char *path, char *out, size_t out_size);
int platform_list_directory(const char *path, char ***names_out, size_t *count_out);
void platform_free_directory_list(char **names, size_t count);
int platform_visit_java_registry_homes(PlatformPathVisitor visitor, void *userdata);
int platform_replace_directory_link(const char *link_path, const char *target_path);
int platform_is_directory_link(const char *path);
int platform_spawn_wait(char *const argv[]);
int platform_find_executable(const char *name, char *out, size_t out_size);
int platform_find_trusted_executable(const char *name, char *out, size_t out_size);
int platform_set_environment(const char *name, const char *value);
int platform_prepend_path(const char *directory);
const char *platform_os_name(void);
const char *platform_arch_name(void);
const char *platform_archive_extension(void);
unsigned long platform_process_id(void);
int platform_lock_acquire(const char *path, PlatformLock *lock);
void platform_lock_release(PlatformLock *lock);

#endif
