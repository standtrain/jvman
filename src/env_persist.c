#include "env_persist.h"

#include "common.h"
#include "i18n.h"
#include "platform.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include "environment.h"
#endif

#if !defined(_WIN32)
#include "rc_writer.h"
#endif

#define JVMAN_ENV_PERSIST_DISABLE_ENV "JVMAN_NO_PERSIST"

/* 直接调 i18n fprintf 的封装宏，保持 __VA_ARGS__ 一致的 printf 签名。 */
#define persist_warn(...) jvman_i18n_fprintf(stderr, __VA_ARGS__)

static int persist_disabled(void) {
    const char *value = getenv(JVMAN_ENV_PERSIST_DISABLE_ENV);
    return value && value[0] && strcmp(value, "0") != 0;
}

#if defined(_WIN32)

/* --------- Windows: HKCU\Environment + installer metadata --------- */

static int utf8_to_wide(const char *utf8, wchar_t **out) {
    int required;
    wchar_t *wide;
    if (!utf8 || !out) return -1;
    required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                   NULL, 0);
    if (required <= 0) return -1;
    wide = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
    if (!wide) return -1;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide,
                            required) != required) {
        free(wide);
        return -1;
    }
    *out = wide;
    return 0;
}

static wchar_t *wide_dup(const wchar_t *source) {
    size_t bytes;
    wchar_t *copy;
    if (!source) return NULL;
    bytes = (wcslen(source) + 1u) * sizeof(wchar_t);
    copy = (wchar_t *)malloc(bytes);
    if (!copy) return NULL;
    memcpy(copy, source, bytes);
    return copy;
}

static int ensure_metadata_defaults(JvmanInstallerMetadata *metadata,
                                    const wchar_t *data_home_wide) {
    /* 已存在则保留全部字段；不存在则填最小可行值。 */
    if (!metadata->version) metadata->version = wide_dup(L"cli-" JVMAN_VERSION_W);
    if (!metadata->install_id) metadata->install_id = wide_dup(L"cli-managed");
    if (!metadata->data_home) metadata->data_home = wide_dup(data_home_wide);
    /* install_dir 保留空串，避免 setup /UNINSTALL 误把 CLI 数据目录当作程序目录清理。
     * environment.c 的 write_registry_string 允许空串写入 REG_SZ。 */
    if (!metadata->install_dir) metadata->install_dir = wide_dup(L"");
    if (!metadata->version || !metadata->install_id || !metadata->data_home ||
        !metadata->install_dir) {
        return -1;
    }
    return 0;
}

static void restore_path_snapshot(JvmanEnvironmentPathSnapshot *baseline,
                                  JvmanEnvironmentPathSnapshot *written) {
    int changed = 0;
    /* 用 written（本次实际写入的值）作为 expected_current 做 CAS 恢复。 */
    if (written && written->valid) {
        (void)jvman_environment_path_snapshot_restore(
            JVMAN_ENV_SCOPE_USER, baseline, written, &changed);
    } else {
        /* 未真正写入，无需回滚。 */
        (void)changed;
    }
}

static void restore_java_home_snapshot(JvmanEnvironmentValueSnapshot *baseline,
                                       JvmanEnvironmentValueSnapshot *written) {
    int changed = 0;
    if (written && written->valid) {
        (void)jvman_environment_java_home_snapshot_restore(baseline, written,
                                                            &changed);
    } else {
        (void)changed;
    }
}

static int windows_activate(const JvmanPersistOptions *opts) {
    JvmanInstallerMetadata metadata;
    JvmanEnvironmentPathSnapshot path_baseline;
    JvmanEnvironmentValueSnapshot jh_baseline;
    JvmanEnvironmentPathSnapshot path_written;
    JvmanEnvironmentValueSnapshot jh_written;
    wchar_t *current_wide = NULL;
    wchar_t *current_bin_wide = NULL;
    wchar_t *data_home_wide = NULL;
    char current_bin[JVMAN_PATH_MAX];
    int found = 0;
    int owned = 0;
    int owned_after = 0;
    int changed_path = 0;
    int changed_home = 0;
    JvmanEnvironmentStatus st;
    int result = -1;

    jvman_installer_metadata_init(&metadata);
    jvman_environment_path_snapshot_init(&path_baseline);
    jvman_environment_value_snapshot_init(&jh_baseline);
    jvman_environment_path_snapshot_init(&path_written);
    jvman_environment_value_snapshot_init(&jh_written);

    if (jvman_path_join(current_bin, sizeof(current_bin),
                        opts->ctx->current_link, "bin") != 0) {
        if (!opts->quiet) {
            persist_warn("jvman: cannot compute current/bin path\n");
        }
        goto cleanup;
    }

    if (utf8_to_wide(opts->ctx->current_link, &current_wide) != 0 ||
        utf8_to_wide(current_bin, &current_bin_wide) != 0 ||
        utf8_to_wide(opts->ctx->root, &data_home_wide) != 0) {
        if (!opts->quiet) {
            persist_warn("jvman: cannot convert install paths to UTF-16 for HKCU\n");
        }
        goto cleanup;
    }

    st = jvman_installer_metadata_load_scoped(JVMAN_ENV_SCOPE_USER, &metadata,
                                              &found);
    if (st != JVMAN_ENV_OK && st != JVMAN_ENV_NOT_FOUND) {
        if (!opts->quiet) {
            persist_warn("jvman: cannot read installer metadata (status %d)\n",
                         (int)st);
        }
        goto cleanup;
    }
    /* 无 metadata 时补足最小字段，供 save 写入。 */
    if (ensure_metadata_defaults(&metadata, data_home_wide) != 0) {
        if (!opts->quiet) persist_warn("jvman: out of memory preparing metadata\n");
        goto cleanup;
    }
    owned = metadata.java_path_owned && metadata.java_home_owned;
    if (owned) {
        /* 已由 setup 或先前的 CLI 接管，只需保证 managed value 与 current_link 一致。 */
        if (metadata.java_home_managed_value &&
            wcscmp(metadata.java_home_managed_value, current_wide) == 0) {
            result = 0;
            goto cleanup;
        }
        /* 走一次 configure_java_home 更新 managed 指向 —— 极少见分支。 */
    }

    /* 先备份现值，写入失败时回滚。 */
    (void)jvman_environment_path_snapshot_capture(JVMAN_ENV_SCOPE_USER,
                                                  &path_baseline);
    (void)jvman_environment_java_home_snapshot_capture(&jh_baseline);

    /* 前置 <root>\current\bin 到 HKCU Path。 */
    st = jvman_environment_add_path(JVMAN_ENV_SCOPE_USER, current_bin_wide,
                                    metadata.java_path_owned, &owned_after,
                                    &changed_path, &path_written);
    if (st != JVMAN_ENV_OK) {
        if (!opts->quiet) {
            persist_warn("jvman: cannot update HKCU Path (status %d)\n",
                         (int)st);
        }
        goto rollback;
    }
    metadata.java_path_owned = owned_after ? 1 : metadata.java_path_owned;
    metadata.java_path_scope = (uint32_t)JVMAN_ENV_SCOPE_USER;

    /* 配置 HKCU JAVA_HOME 指向 <root>\current。 */
    st = jvman_environment_configure_java_home(current_wide, &metadata,
                                               opts->replace_java_home,
                                               &changed_home, &jh_written);
    if (st == JVMAN_ENV_CONFLICT) {
        if (!opts->quiet) {
            jvman_i18n_fprintf(
                stderr,
                "jvman: JAVA_HOME already set to a different value; pass --replace-java-home to override or run `jvman deactivate` first.\n");
        }
        goto rollback;
    }
    if (st != JVMAN_ENV_OK) {
        if (!opts->quiet) {
            persist_warn("jvman: cannot update HKCU JAVA_HOME (status %d)\n",
                         (int)st);
        }
        goto rollback;
    }

    /* 保存 metadata。失败则回滚已写的环境变量。 */
    st = jvman_installer_metadata_save_scoped(JVMAN_ENV_SCOPE_USER, &metadata);
    if (st != JVMAN_ENV_OK) {
        if (!opts->quiet) {
            persist_warn("jvman: cannot persist installer metadata (status %d)\n",
                         (int)st);
        }
        goto rollback;
    }

    /* configure_java_home 与 add_path 内部已在写入时广播 WM_SETTINGCHANGE；
     * 保险起见再广播一次，忽略失败。 */
    (void)jvman_environment_broadcast_change();
    result = 0;
    goto cleanup;

rollback:
    restore_java_home_snapshot(&jh_baseline, &jh_written);
    restore_path_snapshot(&path_baseline, &path_written);
cleanup:
    free(current_wide);
    free(current_bin_wide);
    free(data_home_wide);
    jvman_environment_path_snapshot_free(&path_baseline);
    jvman_environment_value_snapshot_free(&jh_baseline);
    jvman_environment_path_snapshot_free(&path_written);
    jvman_environment_value_snapshot_free(&jh_written);
    jvman_installer_metadata_free(&metadata);
    return result;
}

static int windows_deactivate(const JvmanContext *ctx) {
    JvmanInstallerMetadata metadata;
    JvmanEnvironmentPathSnapshot path_written;
    JvmanEnvironmentValueSnapshot jh_written;
    wchar_t *current_bin_wide = NULL;
    char current_bin[JVMAN_PATH_MAX];
    int found = 0;
    int changed_path = 0;
    int changed_home = 0;
    JvmanEnvironmentStatus st;
    int result = -1;

    jvman_installer_metadata_init(&metadata);
    jvman_environment_path_snapshot_init(&path_written);
    jvman_environment_value_snapshot_init(&jh_written);

    st = jvman_installer_metadata_load_scoped(JVMAN_ENV_SCOPE_USER, &metadata,
                                              &found);
    if (st != JVMAN_ENV_OK && st != JVMAN_ENV_NOT_FOUND) {
        persist_warn("jvman: cannot read installer metadata (status %d)\n",
                     (int)st);
        goto cleanup;
    }
    if (!found ||
        (!metadata.java_path_owned && !metadata.java_home_owned)) {
        jvman_i18n_puts("Persistent activation is not managed by jvman; nothing to remove.");
        result = 0;
        goto cleanup;
    }

    if (jvman_path_join(current_bin, sizeof(current_bin), ctx->current_link,
                        "bin") != 0 ||
        utf8_to_wide(current_bin, &current_bin_wide) != 0) {
        persist_warn("jvman: cannot convert current/bin path\n");
        goto cleanup;
    }

    /* 恢复 JAVA_HOME 到 prior 值。 */
    if (metadata.java_home_owned) {
        st = jvman_environment_restore_java_home(&metadata, &changed_home,
                                                 &jh_written);
        if (st != JVMAN_ENV_OK && st != JVMAN_ENV_CONFLICT) {
            persist_warn("jvman: cannot restore HKCU JAVA_HOME (status %d)\n",
                         (int)st);
            goto cleanup;
        }
        metadata.java_home_owned = 0;
        metadata.java_home_prior_present = 0;
        free(metadata.java_home_managed_value);
        metadata.java_home_managed_value = NULL;
        free(metadata.java_home_prior_value);
        metadata.java_home_prior_value = NULL;
    }
    /* 移除 PATH 里 <root>\current\bin。 */
    if (metadata.java_path_owned) {
        st = jvman_environment_remove_path(JVMAN_ENV_SCOPE_USER,
                                           current_bin_wide, 1, &changed_path,
                                           &path_written);
        if (st != JVMAN_ENV_OK) {
            persist_warn("jvman: cannot remove <current>/bin from HKCU Path (status %d)\n",
                         (int)st);
            goto cleanup;
        }
        metadata.java_path_owned = 0;
    }
    /* 保存清空后的 metadata。 */
    st = jvman_installer_metadata_save_scoped(JVMAN_ENV_SCOPE_USER, &metadata);
    if (st != JVMAN_ENV_OK) {
        persist_warn("jvman: cannot update installer metadata (status %d)\n",
                     (int)st);
        goto cleanup;
    }
    (void)jvman_environment_broadcast_change();
    jvman_i18n_puts("Removed persistent activation.");
    result = 0;
cleanup:
    free(current_bin_wide);
    jvman_environment_path_snapshot_free(&path_written);
    jvman_environment_value_snapshot_free(&jh_written);
    jvman_installer_metadata_free(&metadata);
    return result;
}

static int windows_is_owned(int *out) {
    JvmanInstallerMetadata metadata;
    int found = 0;
    JvmanEnvironmentStatus st;
    jvman_installer_metadata_init(&metadata);
    st = jvman_installer_metadata_load_scoped(JVMAN_ENV_SCOPE_USER, &metadata,
                                              &found);
    *out = 0;
    if (st == JVMAN_ENV_OK && found &&
        metadata.java_path_owned && metadata.java_home_owned) {
        *out = 1;
    }
    jvman_installer_metadata_free(&metadata);
    return 0;
}

#else /* !_WIN32 */

/* --------- POSIX: 追加/更新 shell rc marker 块 --------- */

typedef struct RcTarget {
    const char *env_key;      /* NULL 表示直接用相对 $HOME 路径 */
    const char *relative;
} RcTarget;

static const RcTarget kRcTargets[] = {
    {NULL, ".bashrc"},
    {NULL, ".zshrc"},
};

static const size_t kRcTargetCount = sizeof(kRcTargets) / sizeof(kRcTargets[0]);

static int build_rc_path(const RcTarget *target, char *out, size_t out_size) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    if ((size_t)snprintf(out, out_size, "%s/%s", home, target->relative)
        >= out_size) {
        return -1;
    }
    return 0;
}

static int build_state_path(const JvmanContext *ctx, char *out, size_t out_size) {
    return jvman_path_join(out, out_size, ctx->root, "rc.state");
}

/* rc.state 是简单的每行一个绝对路径的文本文件，记录已写过的 rc 文件。 */
static int append_state(const JvmanContext *ctx, const char *rc_path) {
    char state_path[JVMAN_PATH_MAX];
    FILE *file;
    if (build_state_path(ctx, state_path, sizeof(state_path)) != 0) return -1;
    file = fopen(state_path, "ab");
    if (!file) return -1;
    fprintf(file, "%s\n", rc_path);
    fclose(file);
    return 0;
}

static int posix_activate(const JvmanPersistOptions *opts) {
    static const char *kPayload =
        "# Managed by jvman; edit via `jvman activate` / `jvman deactivate`.\n"
        "if command -v jvman >/dev/null 2>&1; then\n"
        "  eval \"$(jvman init sh)\"\n"
        "fi";
    size_t i;
    int written_any = 0;
    for (i = 0; i < kRcTargetCount; ++i) {
        char rc_path[JVMAN_PATH_MAX];
        if (build_rc_path(&kRcTargets[i], rc_path, sizeof(rc_path)) != 0) continue;
        if (jvman_rc_writer_apply(rc_path, kPayload) != 0) {
            if (!opts->quiet) {
                persist_warn("jvman: cannot update %s\n", rc_path);
            }
            continue;
        }
        (void)append_state(opts->ctx, rc_path);
        written_any = 1;
    }
    if (!written_any && !opts->quiet) {
        persist_warn("jvman: no shell rc files were updated (missing $HOME or targets)\n");
        return -1;
    }
    return 0;
}

static int posix_deactivate(const JvmanContext *ctx) {
    size_t i;
    for (i = 0; i < kRcTargetCount; ++i) {
        char rc_path[JVMAN_PATH_MAX];
        if (build_rc_path(&kRcTargets[i], rc_path, sizeof(rc_path)) != 0) continue;
        (void)jvman_rc_writer_remove(rc_path);
    }
    {
        char state_path[JVMAN_PATH_MAX];
        if (build_state_path(ctx, state_path, sizeof(state_path)) == 0) {
            (void)platform_remove_file(state_path);
        }
    }
    jvman_i18n_puts("Removed persistent activation.");
    return 0;
}

static int posix_is_owned(const JvmanContext *ctx, int *out) {
    size_t i;
    *out = 0;
    for (i = 0; i < kRcTargetCount; ++i) {
        char rc_path[JVMAN_PATH_MAX];
        int present = 0;
        if (build_rc_path(&kRcTargets[i], rc_path, sizeof(rc_path)) != 0) continue;
        if (jvman_rc_writer_probe(rc_path, &present) == 0 && present) {
            *out = 1;
            return 0;
        }
    }
    (void)ctx;
    return 0;
}

#endif /* _WIN32 */

int jvman_persist_activate(const JvmanPersistOptions *opts) {
    if (!opts || !opts->ctx) return -1;
    if (persist_disabled()) return 0;
#if defined(_WIN32)
    return windows_activate(opts);
#else
    return posix_activate(opts);
#endif
}

int jvman_persist_deactivate(const JvmanContext *ctx) {
    if (!ctx) return -1;
#if defined(_WIN32)
    return windows_deactivate(ctx);
#else
    return posix_deactivate(ctx);
#endif
}

int jvman_persist_is_owned(const JvmanContext *ctx, int *out) {
    if (!ctx || !out) return -1;
    *out = 0;
#if defined(_WIN32)
    (void)ctx;
    return windows_is_owned(out);
#else
    return posix_is_owned(ctx, out);
#endif
}
