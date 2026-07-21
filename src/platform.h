#ifndef JVMAN_PLATFORM_H
#define JVMAN_PLATFORM_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

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
void platform_clear_error(void);
/* 在进程启动期关闭当前目录参与可执行文件搜索，防御 PATH 注入。 */
void platform_init_secure_search_path(void);
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
int platform_create_temporary_file(char *out, size_t out_size);
int platform_read_file_limited(const char *path, size_t limit,
                               char **data_out, size_t *size_out);
int platform_https_download(const char *url, const char *destination,
                            size_t limit, int show_progress);
int platform_https_download_timeout(const char *url, const char *destination,
                                    size_t limit, int show_progress,
                                    unsigned int timeout_seconds);
int platform_https_probe(const char *url, size_t sample_size,
                         unsigned int timeout_seconds);
int platform_current_executable(char *out, size_t out_size);
/* Starts the registered Windows uninstaller without waiting for it. */
int platform_launch_jvman_uninstaller(void);
int platform_sha256_file(const char *path, unsigned char digest[32]);
int platform_validate_executable_image(const char *path);
int platform_stage_executable_update(const char *source, const char *target,
                                     char *staged_out, size_t staged_size);
int platform_publish_executable_update(const char *staged, const char *target,
                                       const char *expected_sha256,
                                       const char *expected_current_sha256,
                                       int *deferred_out);
/* Called before normal CLI initialization; handled_out is set for helper mode. */
int platform_handle_update_helper(int argc, char **argv, int *handled_out);
int platform_set_environment(const char *name, const char *value);
int platform_prepend_path(const char *directory);
const char *platform_os_name(void);
const char *platform_arch_name(void);
const char *platform_archive_extension(void);
unsigned long platform_process_id(void);
int platform_monotonic_millis(uint64_t *value_out);
int platform_lock_acquire(const char *path, PlatformLock *lock);
void platform_lock_release(PlatformLock *lock);

#endif
