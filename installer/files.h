#ifndef JVMAN_INSTALLER_FILES_H
#define JVMAN_INSTALLER_FILES_H

/*
 * The installer file layer deliberately uses the wide Win32 API.  Keeping
 * paths in caller-owned, fixed-size buffers makes ownership explicit and
 * prevents an accidental unbounded allocation from becoming a path input.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <stdint.h>

#include "package.h"

#define JVMAN_INSTALL_PATH_CHARS ((size_t)4096)
#define JVMAN_INSTALL_MARKER_ID_CHARS ((size_t)128)
#define JVMAN_INSTALL_MARKER_NAME L"install.marker"
#define JVMAN_INSTALL_UNINSTALL_NAME L"uninstall.exe"
#define JVMAN_INSTALL_EXECUTABLE_NAME L"jvman.exe"

typedef enum JvmanInstallStatus {
    JVMAN_INSTALL_OK = 0,
    JVMAN_INSTALL_INVALID_ARGUMENT,
    JVMAN_INSTALL_INVALID_PATH,
    JVMAN_INSTALL_PATH_TOO_LONG,
    JVMAN_INSTALL_PATH_NOT_LOCAL,
    JVMAN_INSTALL_PATH_REPARSE,
    JVMAN_INSTALL_PATH_NOT_FOUND,
    JVMAN_INSTALL_IO_ERROR,
    JVMAN_INSTALL_FORMAT_ERROR,
    JVMAN_INSTALL_HASH_MISMATCH,
    JVMAN_INSTALL_MARKER_INVALID,
    JVMAN_INSTALL_NOT_FOUND,
    JVMAN_INSTALL_NO_MEMORY,
    JVMAN_INSTALL_SELF_CLEANUP_REQUIRED
} JvmanInstallStatus;

/* All members are NUL-terminated when a paths operation succeeds. */
typedef struct JvmanInstallPaths {
    wchar_t install_dir[JVMAN_INSTALL_PATH_CHARS];
    wchar_t data_home[JVMAN_INSTALL_PATH_CHARS];
    wchar_t jvman_path[JVMAN_INSTALL_PATH_CHARS];
    wchar_t marker_path[JVMAN_INSTALL_PATH_CHARS];
    wchar_t uninstall_path[JVMAN_INSTALL_PATH_CHARS];
} JvmanInstallPaths;

/* A payload handle remains open until jvman_setup_payload_close is called. */
typedef struct JvmanSetupPayload {
    HANDLE setup_handle;
    wchar_t setup_path[JVMAN_INSTALL_PATH_CHARS];
    uint64_t payload_offset;
    uint64_t payload_size;
    unsigned char digest[32];
} JvmanSetupPayload;

const wchar_t *jvman_install_status_message(JvmanInstallStatus status);

/*
 * Validate and compose installation paths.  Both paths must be absolute,
 * local, non-root paths and must not contain a reparse component.  The two
 * trees may not contain one another in either direction.
 */
JvmanInstallStatus jvman_install_paths_init(
    JvmanInstallPaths *paths,
    const wchar_t *install_dir,
    const wchar_t *data_home);

/* Uses %LOCALAPPDATA%\Programs\jvman and a valid JVMAN_HOME, or
 * %LOCALAPPDATA%\jvman when JVMAN_HOME is absent/invalid. */
JvmanInstallStatus jvman_install_paths_default(JvmanInstallPaths *paths);

/* Securely create missing components of the program tree.  The data tree is
 * intentionally left untouched until jvman itself needs it. */
JvmanInstallStatus jvman_install_paths_create(
    const JvmanInstallPaths *paths);

/* Read and verify the footer at EOF of a setup executable.  A NULL path uses
 * the current setup module.  No candidate executable is launched. */
JvmanInstallStatus jvman_setup_payload_open(
    const wchar_t *setup_path,
    JvmanSetupPayload *payload);
void jvman_setup_payload_close(JvmanSetupPayload *payload);

/* Stream the payload to target_path, verify its SHA-256, then atomically
 * publish it.  target_path must be in a previously validated directory. */
JvmanInstallStatus jvman_setup_payload_extract(
    const JvmanSetupPayload *payload,
    const wchar_t *target_path);

/* Atomically copy the running setup module to paths->uninstall_path. */
JvmanInstallStatus jvman_install_copy_self_uninstaller(
    const JvmanInstallPaths *paths);

/* Marker IDs are ASCII [A-Za-z0-9._-], one to 127 characters. */
JvmanInstallStatus jvman_install_marker_write(
    const JvmanInstallPaths *paths,
    const wchar_t *installation_id);
JvmanInstallStatus jvman_install_marker_read(
    const JvmanInstallPaths *paths,
    wchar_t *installation_id,
    size_t installation_id_capacity);

/* Remove only the install whitelist.  No recursive deletion is performed.
 * SELF_CLEANUP_REQUIRED means the running module is paths->uninstall_path;
 * jvman.exe is already removed, while the marker and uninstaller are kept for
 * a validated out-of-process cleanup step. */
JvmanInstallStatus jvman_install_uninstall(
    const JvmanInstallPaths *paths);

#endif /* JVMAN_INSTALLER_FILES_H */
