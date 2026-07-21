#ifndef JVMAN_INSTALLER_ENVIRONMENT_H
#define JVMAN_INSTALLER_ENVIRONMENT_H

/*
 * The installer deliberately keeps this interface in UTF-16.  Windows
 * registry values are UTF-16 and using the wide API throughout avoids lossy
 * conversions for non-ASCII installation paths.
 */
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define JVMAN_ENV_VALUE_MAX_CHARS ((size_t)32767)
#define JVMAN_METADATA_TEXT_MAX_CHARS ((size_t)4096)
#define JVMAN_LANGUAGE_TAG_MAX_CHARS ((size_t)32)

typedef enum JvmanEnvironmentStatus {
    JVMAN_ENV_OK = 0,
    JVMAN_ENV_NOT_FOUND,
    JVMAN_ENV_INVALID_ARGUMENT,
    JVMAN_ENV_UNSUPPORTED_TYPE,
    JVMAN_ENV_TOO_LONG,
    JVMAN_ENV_NO_MEMORY,
    JVMAN_ENV_WIN32_ERROR,
    JVMAN_ENV_ACCESS_DENIED,
    JVMAN_ENV_METADATA_INVALID,
    JVMAN_ENV_CONFLICT,
    JVMAN_ENV_UNSUPPORTED
} JvmanEnvironmentStatus;

typedef enum JvmanEnvironmentScope {
    JVMAN_ENV_SCOPE_USER = 0,
    JVMAN_ENV_SCOPE_MACHINE = 1
} JvmanEnvironmentScope;

typedef struct JvmanEnvironmentPathSnapshot {
    int valid;
    int present;
    uint32_t type;
    uint32_t byte_count;
    wchar_t *value;
} JvmanEnvironmentPathSnapshot;

typedef JvmanEnvironmentPathSnapshot JvmanEnvironmentValueSnapshot;

/*
 * Values persisted below Software\\jvman\\Installer in the registry hive
 * selected by the metadata API.
 */
typedef struct JvmanInstallerMetadata {
    wchar_t *version;
    wchar_t *install_dir;
    wchar_t *install_id;
    wchar_t *data_home;
    /* Optional CLI language tag retained across installer operations. */
    wchar_t *language;

    /* Each PATH entry is tracked independently. */
    int app_path_owned;
    uint32_t app_path_scope;
    int java_path_owned;
    uint32_t java_path_scope;

    int java_home_owned;
    int java_home_prior_present;
    uint32_t java_home_prior_type;
    wchar_t *java_home_prior_value;
    wchar_t *java_home_managed_value;
} JvmanInstallerMetadata;

const wchar_t *jvman_environment_status_message(JvmanEnvironmentStatus status);

void jvman_installer_metadata_init(JvmanInstallerMetadata *metadata);
void jvman_installer_metadata_free(JvmanInstallerMetadata *metadata);
void jvman_environment_path_snapshot_init(
    JvmanEnvironmentPathSnapshot *snapshot);
void jvman_environment_path_snapshot_free(
    JvmanEnvironmentPathSnapshot *snapshot);
void jvman_environment_value_snapshot_init(
    JvmanEnvironmentValueSnapshot *snapshot);
void jvman_environment_value_snapshot_free(
    JvmanEnvironmentValueSnapshot *snapshot);

/*
 * Scoped metadata operations accept only USER or MACHINE.  found_out is set
 * to zero when the selected hive does not contain the metadata key.
 */
JvmanEnvironmentStatus jvman_installer_metadata_load_scoped(
    JvmanEnvironmentScope scope, JvmanInstallerMetadata *metadata,
    int *found_out);
JvmanEnvironmentStatus jvman_installer_metadata_save_scoped(
    JvmanEnvironmentScope scope, const JvmanInstallerMetadata *metadata);
JvmanEnvironmentStatus jvman_installer_metadata_delete_scoped(
    JvmanEnvironmentScope scope);

/* Compatibility wrappers that operate on current-user metadata. */
JvmanEnvironmentStatus jvman_installer_metadata_load(
    JvmanInstallerMetadata *metadata, int *found_out);
JvmanEnvironmentStatus jvman_installer_metadata_save(
    const JvmanInstallerMetadata *metadata);
JvmanEnvironmentStatus jvman_installer_metadata_delete(void);

/*
 * Add/remove one absolute directory in the PATH selected by scope.  On add,
 * owned_out reports whether this installer invocation owns the entry: a
 * pre-existing entry retains prior_owned, while a newly added entry returns
 * one.  changed_out reports whether the registry value was written.  Scope
 * accepts only USER or MACHINE.
 */
JvmanEnvironmentStatus jvman_environment_add_path(
    JvmanEnvironmentScope scope, const wchar_t *directory, int prior_owned,
    int *owned_out, int *changed_out,
    JvmanEnvironmentPathSnapshot *written_out);
JvmanEnvironmentStatus jvman_environment_remove_path(
    JvmanEnvironmentScope scope, const wchar_t *directory, int owned,
    int *changed_out, JvmanEnvironmentPathSnapshot *written_out);
/* Capture PATH and restore after verifying that current still matches expected. */
JvmanEnvironmentStatus jvman_environment_path_snapshot_capture(
    JvmanEnvironmentScope scope, JvmanEnvironmentPathSnapshot *snapshot);
JvmanEnvironmentStatus jvman_environment_path_snapshot_restore(
    JvmanEnvironmentScope scope,
    const JvmanEnvironmentPathSnapshot *snapshot,
    const JvmanEnvironmentPathSnapshot *expected_current,
    int *changed_out);

/* Configure and restore the per-user JAVA_HOME value. */
JvmanEnvironmentStatus jvman_environment_configure_java_home(
    const wchar_t *java_home, JvmanInstallerMetadata *metadata,
    int replace_existing, int *changed_out,
    JvmanEnvironmentValueSnapshot *written_out);
JvmanEnvironmentStatus jvman_environment_restore_java_home(
    const JvmanInstallerMetadata *metadata, int *changed_out,
    JvmanEnvironmentValueSnapshot *written_out);
JvmanEnvironmentStatus jvman_environment_java_home_snapshot_capture(
    JvmanEnvironmentValueSnapshot *snapshot);
JvmanEnvironmentStatus jvman_environment_java_home_snapshot_restore(
    const JvmanEnvironmentValueSnapshot *snapshot,
    const JvmanEnvironmentValueSnapshot *expected_current,
    int *changed_out);

/*
 * Add/Remove Programs registration in the registry hive selected by scope.
 * Scope accepts only USER or MACHINE.
 */
JvmanEnvironmentStatus jvman_arp_write_scoped(
    JvmanEnvironmentScope scope, const wchar_t *version,
    const wchar_t *install_dir, const wchar_t *uninstall_command);
JvmanEnvironmentStatus jvman_arp_delete_scoped(JvmanEnvironmentScope scope);

/* Compatibility wrappers that register the application for the current user. */
JvmanEnvironmentStatus jvman_arp_write(
    const wchar_t *version, const wchar_t *install_dir,
    const wchar_t *uninstall_command);
JvmanEnvironmentStatus jvman_arp_delete(void);

/* Notify already-running processes that an environment setting changed. */
JvmanEnvironmentStatus jvman_environment_broadcast_change(void);

#endif
