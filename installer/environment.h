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

typedef enum JvmanEnvironmentStatus {
    JVMAN_ENV_OK = 0,
    JVMAN_ENV_NOT_FOUND,
    JVMAN_ENV_INVALID_ARGUMENT,
    JVMAN_ENV_UNSUPPORTED_TYPE,
    JVMAN_ENV_TOO_LONG,
    JVMAN_ENV_NO_MEMORY,
    JVMAN_ENV_WIN32_ERROR,
    JVMAN_ENV_METADATA_INVALID,
    JVMAN_ENV_CONFLICT,
    JVMAN_ENV_UNSUPPORTED
} JvmanEnvironmentStatus;

/* Values persisted below HKCU\\Software\\jvman\\Installer. */
typedef struct JvmanInstallerMetadata {
    wchar_t *version;
    wchar_t *install_dir;
    wchar_t *install_id;
    wchar_t *data_home;

    /* Each PATH entry is tracked independently. */
    int app_path_owned;
    int java_path_owned;

    int java_home_owned;
    int java_home_prior_present;
    uint32_t java_home_prior_type;
    wchar_t *java_home_prior_value;
    wchar_t *java_home_managed_value;
} JvmanInstallerMetadata;

const wchar_t *jvman_environment_status_message(JvmanEnvironmentStatus status);

void jvman_installer_metadata_init(JvmanInstallerMetadata *metadata);
void jvman_installer_metadata_free(JvmanInstallerMetadata *metadata);

/* found_out is set to zero when the metadata key does not exist. */
JvmanEnvironmentStatus jvman_installer_metadata_load(
    JvmanInstallerMetadata *metadata, int *found_out);
JvmanEnvironmentStatus jvman_installer_metadata_save(
    const JvmanInstallerMetadata *metadata);
JvmanEnvironmentStatus jvman_installer_metadata_delete(void);

/*
 * Add/remove one absolute directory in the per-user PATH.  On add,
 * owned_out reports whether this installer invocation owns the entry: a
 * pre-existing entry retains prior_owned, while a newly added entry returns
 * one.  changed_out reports whether the registry value was written.
 */
JvmanEnvironmentStatus jvman_environment_add_path(
    const wchar_t *directory, int prior_owned, int *owned_out, int *changed_out);
JvmanEnvironmentStatus jvman_environment_remove_path(
    const wchar_t *directory, int owned, int *changed_out);

/* Configure and restore the per-user JAVA_HOME value. */
JvmanEnvironmentStatus jvman_environment_configure_java_home(
    const wchar_t *java_home, JvmanInstallerMetadata *metadata,
    int replace_existing, int *changed_out);
JvmanEnvironmentStatus jvman_environment_restore_java_home(
    const JvmanInstallerMetadata *metadata, int *changed_out);

/* Current-user Add/Remove Programs registration. */
JvmanEnvironmentStatus jvman_arp_write(
    const wchar_t *version, const wchar_t *install_dir,
    const wchar_t *uninstall_command);
JvmanEnvironmentStatus jvman_arp_delete(void);

/* Notify already-running processes that HKCU\\Environment changed. */
JvmanEnvironmentStatus jvman_environment_broadcast_change(void);

#endif
