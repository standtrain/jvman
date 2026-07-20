#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "environment.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include "pathlist.h"

#include <windows.h>

#define JVMAN_ENVIRONMENT_KEY L"Environment"
#define JVMAN_INSTALLER_KEY L"Software\\jvman\\Installer"
#define JVMAN_ARP_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\jvman"

#define JVMAN_VALUE_VERSION L"Version"
#define JVMAN_VALUE_INSTALL_DIR L"InstallDir"
#define JVMAN_VALUE_INSTALL_ID L"InstallId"
#define JVMAN_VALUE_DATA_HOME L"DataHome"
#define JVMAN_VALUE_APP_PATH_OWNED L"AppPathOwned"
#define JVMAN_VALUE_JAVA_PATH_OWNED L"JavaPathOwned"
#define JVMAN_VALUE_JAVA_HOME_OWNED L"JavaHomeOwned"
#define JVMAN_VALUE_JAVA_HOME_PRIOR_PRESENT L"JavaHomePriorPresent"
#define JVMAN_VALUE_JAVA_HOME_PRIOR_TYPE L"JavaHomePriorType"
#define JVMAN_VALUE_JAVA_HOME_PRIOR_VALUE L"JavaHomePriorValue"
#define JVMAN_VALUE_JAVA_HOME_MANAGED_VALUE L"JavaHomeManagedValue"

#define JVMAN_VALUE_DISPLAY_NAME L"DisplayName"
#define JVMAN_VALUE_DISPLAY_VERSION L"DisplayVersion"
#define JVMAN_VALUE_INSTALL_LOCATION L"InstallLocation"
#define JVMAN_VALUE_UNINSTALL_STRING L"UninstallString"
#define JVMAN_VALUE_PUBLISHER L"Publisher"
#define JVMAN_VALUE_NO_MODIFY L"NoModify"
#define JVMAN_VALUE_NO_REPAIR L"NoRepair"

typedef struct JvmanRegistryString {
    int present;
    DWORD type;
    wchar_t *value;
} JvmanRegistryString;

static void registry_string_init(JvmanRegistryString *value) {
    if (!value) return;
    value->present = 0;
    value->type = 0;
    value->value = NULL;
}

static void registry_string_free(JvmanRegistryString *value) {
    if (!value) return;
    free(value->value);
    registry_string_init(value);
}

static int registry_string_type(DWORD type) {
    return type == REG_SZ || type == REG_EXPAND_SZ;
}

static int valid_environment_type(uint32_t type) {
    return type == (uint32_t)REG_SZ || type == (uint32_t)REG_EXPAND_SZ;
}

static JvmanEnvironmentStatus map_registry_status(LSTATUS status) {
    if (status == ERROR_SUCCESS) return JVMAN_ENV_OK;
    if (status == ERROR_FILE_NOT_FOUND) return JVMAN_ENV_NOT_FOUND;
    if (status == ERROR_MORE_DATA) return JVMAN_ENV_TOO_LONG;
    return JVMAN_ENV_WIN32_ERROR;
}

/* Find a NUL without ever reading beyond the caller-supplied bound. */
static JvmanEnvironmentStatus bounded_length(const wchar_t *value,
                                             size_t maximum,
                                             size_t *length_out) {
    size_t index;
    if (!value || !length_out || maximum == 0u) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    for (index = 0u; index < maximum; ++index) {
        if (value[index] == L'\0') {
            *length_out = index;
            return JVMAN_ENV_OK;
        }
    }
    return JVMAN_ENV_TOO_LONG;
}

static JvmanEnvironmentStatus duplicate_bounded(const wchar_t *value,
                                                size_t maximum,
                                                wchar_t **copy_out) {
    size_t length;
    size_t bytes;
    wchar_t *copy;
    JvmanEnvironmentStatus status;
    if (!copy_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *copy_out = NULL;
    status = bounded_length(value, maximum, &length);
    if (status != JVMAN_ENV_OK) return status;
    if (length > (SIZE_MAX / sizeof(wchar_t)) - 1u) {
        return JVMAN_ENV_TOO_LONG;
    }
    bytes = (length + 1u) * sizeof(wchar_t);
    copy = (wchar_t *)malloc(bytes);
    if (!copy) return JVMAN_ENV_NO_MEMORY;
    if (length != 0u) memcpy(copy, value, length * sizeof(wchar_t));
    copy[length] = L'\0';
    *copy_out = copy;
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus validate_string(const wchar_t *value,
                                              size_t maximum) {
    size_t ignored_length;
    if (!value) return JVMAN_ENV_INVALID_ARGUMENT;
    return bounded_length(value, maximum, &ignored_length);
}

/* Installer metadata and ARP strings must not contain line/control characters. */
static JvmanEnvironmentStatus validate_metadata_text(const wchar_t *value,
                                                     size_t maximum) {
    size_t index;
    JvmanEnvironmentStatus status;
    status = bounded_length(value, maximum, &index);
    if (status != JVMAN_ENV_OK) return status;
    for (index = 0u; value[index] != L'\0'; ++index) {
        unsigned int character = (unsigned int)value[index];
        if (character < 0x20u || character == 0x7fu) {
            return JVMAN_ENV_INVALID_ARGUMENT;
        }
    }
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus validate_environment_text(const wchar_t *value,
                                                        size_t maximum) {
    size_t index;
    JvmanEnvironmentStatus status;
    status = bounded_length(value, maximum, &index);
    if (status != JVMAN_ENV_OK) return status;
    for (index = 0u; value[index] != L'\0'; ++index) {
        unsigned int character = (unsigned int)value[index];
        if (character < 0x20u || character == 0x7fu || character == (unsigned int)L';' ||
            character == (unsigned int)L'"') {
            return JVMAN_ENV_INVALID_ARGUMENT;
        }
    }
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus query_registry_string(
    HKEY key, const wchar_t *name, size_t maximum, JvmanRegistryString *out) {
    DWORD type = 0;
    DWORD byte_count = 0;
    DWORD actual_count;
    size_t wchar_count;
    size_t index;
    wchar_t *buffer = NULL;
    JvmanEnvironmentStatus status;
    LSTATUS result;

    if (!key || !name || !out || maximum == 0u) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    registry_string_free(out);
    result = RegQueryValueExW(key, name, NULL, &type, NULL, &byte_count);
    if (result == ERROR_FILE_NOT_FOUND) return JVMAN_ENV_OK;
    if (result != ERROR_SUCCESS) return map_registry_status(result);
    if (!registry_string_type(type)) return JVMAN_ENV_UNSUPPORTED_TYPE;
    if ((byte_count % sizeof(wchar_t)) != 0u ||
        (size_t)byte_count > maximum * sizeof(wchar_t)) {
        return JVMAN_ENV_TOO_LONG;
    }
    wchar_count = (size_t)byte_count / sizeof(wchar_t);
    if (wchar_count > SIZE_MAX / sizeof(wchar_t) - 1u) {
        return JVMAN_ENV_TOO_LONG;
    }
    buffer = (wchar_t *)calloc(wchar_count + 1u, sizeof(wchar_t));
    if (!buffer) return JVMAN_ENV_NO_MEMORY;
    actual_count = byte_count;
    result = RegQueryValueExW(key, name, NULL, &type,
                              (LPBYTE)buffer, &actual_count);
    if (result != ERROR_SUCCESS) {
        free(buffer);
        return map_registry_status(result);
    }
    if ((actual_count % sizeof(wchar_t)) != 0u ||
        (size_t)actual_count > maximum * sizeof(wchar_t)) {
        free(buffer);
        return JVMAN_ENV_TOO_LONG;
    }
    wchar_count = (size_t)actual_count / sizeof(wchar_t);
    buffer[wchar_count] = L'\0';
    /* REG_SZ values should be terminated.  Reject malformed data rather than
     * guessing where a potentially attacker-controlled value ends. */
    for (index = 0u; index < wchar_count; ++index) {
        if (buffer[index] == L'\0') break;
    }
    if (index == wchar_count) {
        free(buffer);
        return JVMAN_ENV_METADATA_INVALID;
    }
    for (++index; index < wchar_count; ++index) {
        if (buffer[index] != L'\0') {
            free(buffer);
            return JVMAN_ENV_METADATA_INVALID;
        }
    }
    status = duplicate_bounded(buffer, maximum, &out->value);
    free(buffer);
    if (status != JVMAN_ENV_OK) return status;
    out->present = 1;
    out->type = type;
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus query_registry_dword(HKEY key,
                                                   const wchar_t *name,
                                                   int required,
                                                   uint32_t *value_out,
                                                   int *present_out) {
    DWORD type = 0;
    DWORD byte_count = sizeof(DWORD);
    DWORD value = 0;
    LSTATUS result;
    if (!key || !name || !value_out || !present_out) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *value_out = 0u;
    *present_out = 0;
    result = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)&value,
                              &byte_count);
    if (result == ERROR_FILE_NOT_FOUND && !required) return JVMAN_ENV_OK;
    if (result != ERROR_SUCCESS) {
        if (result == ERROR_FILE_NOT_FOUND && required) {
            return JVMAN_ENV_METADATA_INVALID;
        }
        return map_registry_status(result);
    }
    if (type != REG_DWORD || byte_count != sizeof(DWORD)) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    *value_out = (uint32_t)value;
    *present_out = 1;
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus open_environment_key(DWORD access, int create,
                                                    HKEY *key_out) {
    LSTATUS result;
    if (!key_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *key_out = NULL;
    if (create) {
        result = RegCreateKeyExW(HKEY_CURRENT_USER, JVMAN_ENVIRONMENT_KEY, 0,
                                 NULL, REG_OPTION_NON_VOLATILE,
                                 access | KEY_CREATE_SUB_KEY, NULL, key_out,
                                 NULL);
    } else {
        result = RegOpenKeyExW(HKEY_CURRENT_USER, JVMAN_ENVIRONMENT_KEY, 0,
                               access, key_out);
    }
    return map_registry_status(result);
}

static JvmanEnvironmentStatus open_installer_key(DWORD access, int create,
                                                  HKEY *key_out) {
    LSTATUS result;
    if (!key_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *key_out = NULL;
    if (create) {
        result = RegCreateKeyExW(HKEY_CURRENT_USER, JVMAN_INSTALLER_KEY, 0,
                                 NULL, REG_OPTION_NON_VOLATILE,
                                 access | KEY_CREATE_SUB_KEY, NULL, key_out,
                                 NULL);
    } else {
        result = RegOpenKeyExW(HKEY_CURRENT_USER, JVMAN_INSTALLER_KEY, 0,
                               access, key_out);
    }
    return map_registry_status(result);
}

static JvmanEnvironmentStatus open_arp_key(DWORD access, HKEY *key_out) {
    LSTATUS result;
    if (!key_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *key_out = NULL;
    result = RegCreateKeyExW(HKEY_CURRENT_USER, JVMAN_ARP_KEY, 0, NULL,
                             REG_OPTION_NON_VOLATILE,
                             access | KEY_CREATE_SUB_KEY, NULL, key_out,
                             NULL);
    return map_registry_status(result);
}

static JvmanEnvironmentStatus write_registry_string(HKEY key,
                                                    const wchar_t *name,
                                                    const wchar_t *value,
                                                    DWORD type,
                                                    size_t maximum) {
    size_t length;
    size_t bytes;
    LSTATUS result;
    JvmanEnvironmentStatus status;
    if (!key || !name || !value || !registry_string_type(type)) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    status = bounded_length(value, maximum, &length);
    if (status != JVMAN_ENV_OK) return status;
    if (length >= SIZE_MAX / sizeof(wchar_t)) return JVMAN_ENV_TOO_LONG;
    bytes = (length + 1u) * sizeof(wchar_t);
    if (bytes > (size_t)UINT32_MAX) return JVMAN_ENV_TOO_LONG;
    result = RegSetValueExW(key, name, 0, type, (const BYTE *)value,
                            (DWORD)bytes);
    return map_registry_status(result);
}

static JvmanEnvironmentStatus write_registry_dword(HKEY key,
                                                   const wchar_t *name,
                                                   uint32_t value) {
    DWORD native_value;
    LSTATUS result;
    if (!key || !name) return JVMAN_ENV_INVALID_ARGUMENT;
    native_value = (DWORD)value;
    result = RegSetValueExW(key, name, 0, REG_DWORD,
                            (const BYTE *)&native_value,
                            (DWORD)sizeof(native_value));
    return map_registry_status(result);
}

static JvmanEnvironmentStatus delete_registry_value(HKEY key,
                                                    const wchar_t *name) {
    LSTATUS result;
    if (!key || !name) return JVMAN_ENV_INVALID_ARGUMENT;
    result = RegDeleteValueW(key, name);
    if (result == ERROR_FILE_NOT_FOUND) return JVMAN_ENV_OK;
    return map_registry_status(result);
}

static JvmanEnvironmentStatus read_environment_value(
    const wchar_t *name, JvmanRegistryString *value_out) {
    HKEY key = NULL;
    JvmanEnvironmentStatus status;
    if (!name || !value_out) return JVMAN_ENV_INVALID_ARGUMENT;
    registry_string_init(value_out);
    status = open_environment_key(KEY_QUERY_VALUE, 0, &key);
    if (status == JVMAN_ENV_NOT_FOUND) return JVMAN_ENV_OK;
    if (status != JVMAN_ENV_OK) return status;
    status = query_registry_string(key, name, JVMAN_ENV_VALUE_MAX_CHARS,
                                   value_out);
    RegCloseKey(key);
    return status;
}

static JvmanEnvironmentStatus write_environment_value(
    const wchar_t *name, const wchar_t *value, DWORD type) {
    HKEY key = NULL;
    JvmanEnvironmentStatus status;
    if (!name || !value) return JVMAN_ENV_INVALID_ARGUMENT;
    status = open_environment_key(KEY_SET_VALUE, 1, &key);
    if (status != JVMAN_ENV_OK) return status;
    status = write_registry_string(key, name, value, type,
                                   JVMAN_ENV_VALUE_MAX_CHARS);
    RegCloseKey(key);
    return status;
}

static JvmanEnvironmentStatus delete_environment_value(const wchar_t *name) {
    HKEY key = NULL;
    JvmanEnvironmentStatus status;
    if (!name) return JVMAN_ENV_INVALID_ARGUMENT;
    status = open_environment_key(KEY_SET_VALUE, 0, &key);
    if (status == JVMAN_ENV_NOT_FOUND) return JVMAN_ENV_OK;
    if (status != JVMAN_ENV_OK) return status;
    status = delete_registry_value(key, name);
    RegCloseKey(key);
    return status;
}

static JvmanEnvironmentStatus map_pathlist_status(JvmanPathListStatus status) {
    switch (status) {
        case JVMAN_PATHLIST_OK: return JVMAN_ENV_OK;
        case JVMAN_PATHLIST_INVALID_ARGUMENT:
        case JVMAN_PATHLIST_INVALID_TARGET: return JVMAN_ENV_INVALID_ARGUMENT;
        case JVMAN_PATHLIST_TOO_LONG: return JVMAN_ENV_TOO_LONG;
        case JVMAN_PATHLIST_NO_MEMORY: return JVMAN_ENV_NO_MEMORY;
        case JVMAN_PATHLIST_WIN32_ERROR: return JVMAN_ENV_WIN32_ERROR;
        default: return JVMAN_ENV_WIN32_ERROR;
    }
}

static JvmanEnvironmentStatus metadata_validate(
    const JvmanInstallerMetadata *metadata) {
    JvmanEnvironmentStatus status;
    if (!metadata || !metadata->version || !metadata->install_dir ||
        !metadata->install_id || !metadata->data_home) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    status = validate_metadata_text(metadata->version,
                                    JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    status = validate_metadata_text(metadata->install_dir,
                                    JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    status = validate_metadata_text(metadata->install_id,
                                    JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    status = validate_metadata_text(metadata->data_home,
                                    JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    if ((metadata->app_path_owned != 0 && metadata->app_path_owned != 1) ||
        (metadata->java_path_owned != 0 && metadata->java_path_owned != 1) ||
        (metadata->java_home_owned != 0 && metadata->java_home_owned != 1) ||
        (metadata->java_home_prior_present != 0 &&
         metadata->java_home_prior_present != 1)) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    if (metadata->java_home_prior_present &&
        !valid_environment_type(metadata->java_home_prior_type)) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    if (!metadata->java_home_prior_present &&
        metadata->java_home_prior_type != 0u) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    if (metadata->java_home_prior_value) {
        status = validate_environment_text(metadata->java_home_prior_value,
                                           JVMAN_ENV_VALUE_MAX_CHARS);
        if (status != JVMAN_ENV_OK) return status;
    } else if (metadata->java_home_prior_present) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    if (metadata->java_home_managed_value) {
        status = validate_environment_text(metadata->java_home_managed_value,
                                           JVMAN_ENV_VALUE_MAX_CHARS);
        if (status != JVMAN_ENV_OK) return status;
    }
    if (metadata->java_home_owned && !metadata->java_home_managed_value) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    return JVMAN_ENV_OK;
}

static JvmanEnvironmentStatus metadata_query_string(
    HKEY key, const wchar_t *name, size_t maximum, int required,
    wchar_t **value_out, int *present_out) {
    JvmanRegistryString value;
    JvmanEnvironmentStatus status;
    if (!key || !name || !value_out || !present_out) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *value_out = NULL;
    *present_out = 0;
    registry_string_init(&value);
    status = query_registry_string(key, name, maximum, &value);
    if (status != JVMAN_ENV_OK) {
        registry_string_free(&value);
        return status == JVMAN_ENV_NOT_FOUND ? JVMAN_ENV_METADATA_INVALID : status;
    }
    if (!value.present) {
        if (required) {
            registry_string_free(&value);
            return JVMAN_ENV_METADATA_INVALID;
        }
        return JVMAN_ENV_OK;
    }
    *value_out = value.value;
    value.value = NULL;
    *present_out = 1;
    registry_string_free(&value);
    return JVMAN_ENV_OK;
}

#endif /* _WIN32 */

const wchar_t *jvman_environment_status_message(JvmanEnvironmentStatus status) {
    switch (status) {
        case JVMAN_ENV_OK: return L"ok";
        case JVMAN_ENV_NOT_FOUND: return L"not found";
        case JVMAN_ENV_INVALID_ARGUMENT: return L"invalid argument";
        case JVMAN_ENV_UNSUPPORTED_TYPE: return L"unsupported registry value type";
        case JVMAN_ENV_TOO_LONG: return L"registry value is too long";
        case JVMAN_ENV_NO_MEMORY: return L"out of memory";
        case JVMAN_ENV_WIN32_ERROR: return L"Windows registry operation failed";
        case JVMAN_ENV_METADATA_INVALID: return L"installer metadata is malformed";
        case JVMAN_ENV_CONFLICT: return L"JAVA_HOME was changed by the user";
        case JVMAN_ENV_UNSUPPORTED: return L"installer environment backend is unsupported";
        default: return L"unknown environment error";
    }
}

void jvman_installer_metadata_init(JvmanInstallerMetadata *metadata) {
    if (!metadata) return;
    memset(metadata, 0, sizeof(*metadata));
}

void jvman_installer_metadata_free(JvmanInstallerMetadata *metadata) {
    if (!metadata) return;
    free(metadata->version);
    free(metadata->install_dir);
    free(metadata->install_id);
    free(metadata->data_home);
    free(metadata->java_home_prior_value);
    free(metadata->java_home_managed_value);
    jvman_installer_metadata_init(metadata);
}

#if defined(_WIN32)

JvmanEnvironmentStatus jvman_installer_metadata_load(
    JvmanInstallerMetadata *metadata, int *found_out) {
    JvmanInstallerMetadata loaded;
    HKEY key = NULL;
    JvmanEnvironmentStatus status;
    int present;
    uint32_t value;

    if (!metadata || !found_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *found_out = 0;
    jvman_installer_metadata_init(&loaded);
    status = open_installer_key(KEY_QUERY_VALUE, 0, &key);
    if (status == JVMAN_ENV_NOT_FOUND) {
        jvman_installer_metadata_free(metadata);
        return JVMAN_ENV_OK;
    }
    if (status != JVMAN_ENV_OK) return status;

    status = metadata_query_string(key, JVMAN_VALUE_VERSION,
                                   JVMAN_METADATA_TEXT_MAX_CHARS, 1,
                                   &loaded.version, &present);
    if (status == JVMAN_ENV_OK) {
        status = metadata_query_string(key, JVMAN_VALUE_INSTALL_DIR,
                                       JVMAN_METADATA_TEXT_MAX_CHARS, 1,
                                       &loaded.install_dir, &present);
    }
    if (status == JVMAN_ENV_OK) {
        status = metadata_query_string(key, JVMAN_VALUE_INSTALL_ID,
                                       JVMAN_METADATA_TEXT_MAX_CHARS, 1,
                                       &loaded.install_id, &present);
    }
    if (status == JVMAN_ENV_OK) {
        status = metadata_query_string(key, JVMAN_VALUE_DATA_HOME,
                                       JVMAN_METADATA_TEXT_MAX_CHARS, 1,
                                       &loaded.data_home, &present);
    }

    if (status == JVMAN_ENV_OK) {
        status = query_registry_dword(key, JVMAN_VALUE_APP_PATH_OWNED, 1,
                                      &value, &present);
        if (status == JVMAN_ENV_OK) {
            if (value > 1u) status = JVMAN_ENV_METADATA_INVALID;
            else loaded.app_path_owned = (int)value;
        }
    }
    if (status == JVMAN_ENV_OK) {
        status = query_registry_dword(key, JVMAN_VALUE_JAVA_PATH_OWNED, 1,
                                      &value, &present);
        if (status == JVMAN_ENV_OK) {
            if (value > 1u) status = JVMAN_ENV_METADATA_INVALID;
            else loaded.java_path_owned = (int)value;
        }
    }
    if (status == JVMAN_ENV_OK) {
        status = query_registry_dword(key, JVMAN_VALUE_JAVA_HOME_OWNED, 1,
                                      &value, &present);
        if (status == JVMAN_ENV_OK) {
            if (value > 1u) status = JVMAN_ENV_METADATA_INVALID;
            else loaded.java_home_owned = (int)value;
        }
    }
    if (status == JVMAN_ENV_OK) {
        status = query_registry_dword(key, JVMAN_VALUE_JAVA_HOME_PRIOR_PRESENT,
                                      1, &value, &present);
        if (status == JVMAN_ENV_OK) {
            if (value > 1u) status = JVMAN_ENV_METADATA_INVALID;
            else loaded.java_home_prior_present = (int)value;
        }
    }
    if (status == JVMAN_ENV_OK) {
        status = query_registry_dword(key, JVMAN_VALUE_JAVA_HOME_PRIOR_TYPE,
                                      1, &value, &present);
        if (status == JVMAN_ENV_OK) loaded.java_home_prior_type = value;
    }
    if (status == JVMAN_ENV_OK) {
        status = metadata_query_string(
            key, JVMAN_VALUE_JAVA_HOME_PRIOR_VALUE,
            JVMAN_ENV_VALUE_MAX_CHARS, 0, &loaded.java_home_prior_value,
            &present);
    }
    if (status == JVMAN_ENV_OK) {
        status = metadata_query_string(
            key, JVMAN_VALUE_JAVA_HOME_MANAGED_VALUE,
            JVMAN_ENV_VALUE_MAX_CHARS, loaded.java_home_owned,
            &loaded.java_home_managed_value, &present);
    }
    RegCloseKey(key);
    if (status != JVMAN_ENV_OK) {
        jvman_installer_metadata_free(&loaded);
        return status == JVMAN_ENV_UNSUPPORTED_TYPE
                   ? JVMAN_ENV_METADATA_INVALID
                   : status;
    }
    if (loaded.java_home_prior_present && !loaded.java_home_prior_value) {
        jvman_installer_metadata_free(&loaded);
        return JVMAN_ENV_METADATA_INVALID;
    }
    status = metadata_validate(&loaded);
    if (status != JVMAN_ENV_OK) {
        jvman_installer_metadata_free(&loaded);
        return status == JVMAN_ENV_INVALID_ARGUMENT
                   ? JVMAN_ENV_METADATA_INVALID
                   : status;
    }
    jvman_installer_metadata_free(metadata);
    *metadata = loaded;
    *found_out = 1;
    return JVMAN_ENV_OK;
}

JvmanEnvironmentStatus jvman_installer_metadata_save(
    const JvmanInstallerMetadata *metadata) {
    HKEY key = NULL;
    JvmanEnvironmentStatus status;
    uint32_t prior_type;
    if (!metadata) return JVMAN_ENV_INVALID_ARGUMENT;
    status = metadata_validate(metadata);
    if (status != JVMAN_ENV_OK) return status;
    status = open_installer_key(KEY_SET_VALUE, 1, &key);
    if (status != JVMAN_ENV_OK) return status;

    status = write_registry_string(key, JVMAN_VALUE_VERSION, metadata->version,
                                   REG_SZ, JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_INSTALL_DIR,
                                       metadata->install_dir, REG_SZ,
                                       JVMAN_METADATA_TEXT_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_INSTALL_ID,
                                       metadata->install_id, REG_SZ,
                                       JVMAN_METADATA_TEXT_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_DATA_HOME,
                                       metadata->data_home, REG_SZ,
                                       JVMAN_METADATA_TEXT_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(key, JVMAN_VALUE_APP_PATH_OWNED,
                                      (uint32_t)metadata->app_path_owned);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(key, JVMAN_VALUE_JAVA_PATH_OWNED,
                                      (uint32_t)metadata->java_path_owned);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(key, JVMAN_VALUE_JAVA_HOME_OWNED,
                                      (uint32_t)metadata->java_home_owned);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(
            key, JVMAN_VALUE_JAVA_HOME_PRIOR_PRESENT,
            metadata->java_home_owned
                ? (uint32_t)metadata->java_home_prior_present
                : 0u);
    }
    prior_type = metadata->java_home_owned && metadata->java_home_prior_present
                     ? metadata->java_home_prior_type
                     : 0u;
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(key, JVMAN_VALUE_JAVA_HOME_PRIOR_TYPE,
                                      prior_type);
    }
    if (status == JVMAN_ENV_OK && metadata->java_home_owned) {
        status = write_registry_string(
            key, JVMAN_VALUE_JAVA_HOME_MANAGED_VALUE,
            metadata->java_home_managed_value, REG_SZ,
            JVMAN_ENV_VALUE_MAX_CHARS);
        if (status == JVMAN_ENV_OK && metadata->java_home_prior_present) {
            status = write_registry_string(
                key, JVMAN_VALUE_JAVA_HOME_PRIOR_VALUE,
                metadata->java_home_prior_value, REG_SZ,
                JVMAN_ENV_VALUE_MAX_CHARS);
        } else if (status == JVMAN_ENV_OK) {
            status = delete_registry_value(key, JVMAN_VALUE_JAVA_HOME_PRIOR_VALUE);
        }
    } else if (status == JVMAN_ENV_OK) {
        status = delete_registry_value(key, JVMAN_VALUE_JAVA_HOME_MANAGED_VALUE);
        if (status == JVMAN_ENV_OK) {
            status = delete_registry_value(key, JVMAN_VALUE_JAVA_HOME_PRIOR_VALUE);
        }
    }
    RegCloseKey(key);
    return status;
}

JvmanEnvironmentStatus jvman_installer_metadata_delete(void) {
    LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, JVMAN_INSTALLER_KEY);
    if (result == ERROR_FILE_NOT_FOUND) return JVMAN_ENV_OK;
    return map_registry_status(result);
}

JvmanEnvironmentStatus jvman_environment_add_path(
    const wchar_t *directory, int prior_owned, int *owned_out, int *changed_out) {
    JvmanRegistryString current;
    wchar_t *updated = NULL;
    JvmanPathListStatus path_status;
    JvmanEnvironmentStatus status;
    int changed = 0;
    DWORD type;
    if (!directory || !owned_out || !changed_out ||
        (prior_owned != 0 && prior_owned != 1)) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *owned_out = prior_owned;
    *changed_out = 0;
    registry_string_init(&current);
    status = read_environment_value(L"Path", &current);
    if (status != JVMAN_ENV_OK) return status;
    path_status = jvman_pathlist_add(
        current.present ? current.value : L"", directory, &updated, &changed);
    status = map_pathlist_status(path_status);
    if (status != JVMAN_ENV_OK) {
        registry_string_free(&current);
        free(updated);
        return status;
    }
    if (!changed) {
        *owned_out = prior_owned;
        registry_string_free(&current);
        free(updated);
        return JVMAN_ENV_OK;
    }
    type = current.present ? current.type : REG_EXPAND_SZ;
    status = write_environment_value(L"Path", updated, type);
    free(updated);
    registry_string_free(&current);
    if (status != JVMAN_ENV_OK) return status;
    *owned_out = 1;
    *changed_out = 1;
    (void)jvman_environment_broadcast_change();
    return JVMAN_ENV_OK;
}

JvmanEnvironmentStatus jvman_environment_remove_path(
    const wchar_t *directory, int owned, int *changed_out) {
    JvmanRegistryString current;
    wchar_t *updated = NULL;
    JvmanPathListStatus path_status;
    JvmanEnvironmentStatus status;
    int changed = 0;
    if (!directory || !changed_out || (owned != 0 && owned != 1)) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *changed_out = 0;
    if (!owned) return JVMAN_ENV_OK;
    registry_string_init(&current);
    status = read_environment_value(L"Path", &current);
    if (status != JVMAN_ENV_OK) return status;
    if (!current.present) {
        registry_string_free(&current);
        return JVMAN_ENV_OK;
    }
    path_status = jvman_pathlist_remove(current.value, directory, &updated,
                                        &changed);
    status = map_pathlist_status(path_status);
    if (status != JVMAN_ENV_OK) {
        registry_string_free(&current);
        free(updated);
        return status;
    }
    if (!changed) {
        registry_string_free(&current);
        free(updated);
        return JVMAN_ENV_OK;
    }
    if (updated[0] == L'\0') status = delete_environment_value(L"Path");
    else status = write_environment_value(L"Path", updated, current.type);
    free(updated);
    registry_string_free(&current);
    if (status != JVMAN_ENV_OK) return status;
    *changed_out = 1;
    (void)jvman_environment_broadcast_change();
    return JVMAN_ENV_OK;
}

JvmanEnvironmentStatus jvman_environment_configure_java_home(
    const wchar_t *java_home, JvmanInstallerMetadata *metadata,
    int replace_existing, int *changed_out) {
    JvmanRegistryString current;
    wchar_t *new_prior_value = NULL;
    wchar_t *new_managed_value = NULL;
    JvmanEnvironmentStatus status;
    DWORD type;
    int changed = 0;
    int first_configuration;
    if (!java_home || !metadata || !changed_out ||
        (replace_existing != 0 && replace_existing != 1)) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    *changed_out = 0;
    status = validate_environment_text(java_home, JVMAN_ENV_VALUE_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    if (metadata->java_home_owned) {
        if (!metadata->java_home_managed_value ||
            (metadata->java_home_prior_present &&
             (!metadata->java_home_prior_value ||
              !valid_environment_type(metadata->java_home_prior_type)))) {
            return JVMAN_ENV_METADATA_INVALID;
        }
    }
    first_configuration = !metadata->java_home_owned;
    registry_string_init(&current);
    status = read_environment_value(L"JAVA_HOME", &current);
    if (status != JVMAN_ENV_OK) return status;
    if (current.present && wcscmp(current.value, java_home) != 0 &&
        !replace_existing) {
        registry_string_free(&current);
        return JVMAN_ENV_CONFLICT;
    }
    if (first_configuration) {
        if (current.present) {
            status = duplicate_bounded(current.value,
                                       JVMAN_ENV_VALUE_MAX_CHARS,
                                       &new_prior_value);
            if (status != JVMAN_ENV_OK) {
                registry_string_free(&current);
                return status;
            }
        }
        status = duplicate_bounded(java_home, JVMAN_ENV_VALUE_MAX_CHARS,
                                   &new_managed_value);
        if (status != JVMAN_ENV_OK) {
            free(new_prior_value);
            registry_string_free(&current);
            return status;
        }
    } else {
        status = duplicate_bounded(java_home, JVMAN_ENV_VALUE_MAX_CHARS,
                                   &new_managed_value);
        if (status != JVMAN_ENV_OK) {
            registry_string_free(&current);
            return status;
        }
    }
    type = current.present ? current.type : REG_EXPAND_SZ;
    if (!current.present || wcscmp(current.value, java_home) != 0) {
        status = write_environment_value(L"JAVA_HOME", java_home, type);
        if (status == JVMAN_ENV_OK) changed = 1;
    }
    registry_string_free(&current);
    if (status != JVMAN_ENV_OK) {
        free(new_prior_value);
        free(new_managed_value);
        return status;
    }
    if (first_configuration) {
        free(metadata->java_home_prior_value);
        metadata->java_home_prior_value = new_prior_value;
        new_prior_value = NULL;
        metadata->java_home_prior_present = (metadata->java_home_prior_value != NULL);
        metadata->java_home_prior_type = metadata->java_home_prior_present
                                            ? (uint32_t)type
                                            : 0u;
    }
    free(metadata->java_home_managed_value);
    metadata->java_home_managed_value = new_managed_value;
    new_managed_value = NULL;
    metadata->java_home_owned = 1;
    *changed_out = changed;
    free(new_prior_value);
    free(new_managed_value);
    if (changed) (void)jvman_environment_broadcast_change();
    return JVMAN_ENV_OK;
}

JvmanEnvironmentStatus jvman_environment_restore_java_home(
    const JvmanInstallerMetadata *metadata, int *changed_out) {
    JvmanRegistryString current;
    JvmanEnvironmentStatus status;
    if (!metadata || !changed_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *changed_out = 0;
    if (!metadata->java_home_owned) return JVMAN_ENV_OK;
    if (!metadata->java_home_managed_value ||
        (metadata->java_home_prior_present &&
         (!metadata->java_home_prior_value ||
          !valid_environment_type(metadata->java_home_prior_type)))) {
        return JVMAN_ENV_METADATA_INVALID;
    }
    registry_string_init(&current);
    status = read_environment_value(L"JAVA_HOME", &current);
    if (status != JVMAN_ENV_OK) return status;
    if (!current.present || wcscmp(current.value,
                                   metadata->java_home_managed_value) != 0) {
        registry_string_free(&current);
        return JVMAN_ENV_OK;
    }
    if (metadata->java_home_prior_present) {
        status = write_environment_value(
            L"JAVA_HOME", metadata->java_home_prior_value,
            (DWORD)metadata->java_home_prior_type);
    } else {
        status = delete_environment_value(L"JAVA_HOME");
    }
    registry_string_free(&current);
    if (status != JVMAN_ENV_OK) return status;
    *changed_out = 1;
    (void)jvman_environment_broadcast_change();
    return JVMAN_ENV_OK;
}

JvmanEnvironmentStatus jvman_arp_write(
    const wchar_t *version, const wchar_t *install_dir,
    const wchar_t *uninstall_command) {
    HKEY key = NULL;
    JvmanEnvironmentStatus status;
    if (!version || !install_dir || !uninstall_command) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    status = validate_string(version, JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    status = validate_string(install_dir, JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    status = validate_string(uninstall_command, JVMAN_ENV_VALUE_MAX_CHARS);
    if (status != JVMAN_ENV_OK) return status;
    status = open_arp_key(KEY_SET_VALUE, &key);
    if (status != JVMAN_ENV_OK) return status;
    status = write_registry_string(key, JVMAN_VALUE_DISPLAY_NAME, L"jvman",
                                   REG_SZ, JVMAN_METADATA_TEXT_MAX_CHARS);
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_DISPLAY_VERSION,
                                       version, REG_SZ,
                                       JVMAN_METADATA_TEXT_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_INSTALL_LOCATION,
                                       install_dir, REG_SZ,
                                       JVMAN_METADATA_TEXT_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_UNINSTALL_STRING,
                                       uninstall_command, REG_SZ,
                                       JVMAN_ENV_VALUE_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_string(key, JVMAN_VALUE_PUBLISHER,
                                       L"jvman contributors", REG_SZ,
                                       JVMAN_METADATA_TEXT_MAX_CHARS);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(key, JVMAN_VALUE_NO_MODIFY, 1u);
    }
    if (status == JVMAN_ENV_OK) {
        status = write_registry_dword(key, JVMAN_VALUE_NO_REPAIR, 1u);
    }
    RegCloseKey(key);
    return status;
}

JvmanEnvironmentStatus jvman_arp_delete(void) {
    LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, JVMAN_ARP_KEY);
    if (result == ERROR_FILE_NOT_FOUND) return JVMAN_ENV_OK;
    return map_registry_status(result);
}

JvmanEnvironmentStatus jvman_environment_broadcast_change(void) {
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                             (LPARAM)L"Environment",
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result)) {
        return JVMAN_ENV_WIN32_ERROR;
    }
    return JVMAN_ENV_OK;
}

#else /* !_WIN32: the installer backend is intentionally Windows-only. */

JvmanEnvironmentStatus jvman_installer_metadata_load(
    JvmanInstallerMetadata *metadata, int *found_out) {
    if (!metadata || !found_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *found_out = 0;
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_installer_metadata_save(
    const JvmanInstallerMetadata *metadata) {
    return metadata ? JVMAN_ENV_UNSUPPORTED : JVMAN_ENV_INVALID_ARGUMENT;
}

JvmanEnvironmentStatus jvman_installer_metadata_delete(void) {
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_environment_add_path(
    const wchar_t *directory, int prior_owned, int *owned_out, int *changed_out) {
    if (!directory || !owned_out || !changed_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *owned_out = prior_owned;
    *changed_out = 0;
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_environment_remove_path(
    const wchar_t *directory, int owned, int *changed_out) {
    if (!directory || !changed_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *changed_out = 0;
    (void)owned;
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_environment_configure_java_home(
    const wchar_t *java_home, JvmanInstallerMetadata *metadata,
    int replace_existing, int *changed_out) {
    if (!java_home || !metadata || !changed_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *changed_out = 0;
    (void)replace_existing;
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_environment_restore_java_home(
    const JvmanInstallerMetadata *metadata, int *changed_out) {
    if (!metadata || !changed_out) return JVMAN_ENV_INVALID_ARGUMENT;
    *changed_out = 0;
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_arp_write(
    const wchar_t *version, const wchar_t *install_dir,
    const wchar_t *uninstall_command) {
    if (!version || !install_dir || !uninstall_command) {
        return JVMAN_ENV_INVALID_ARGUMENT;
    }
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_arp_delete(void) {
    return JVMAN_ENV_UNSUPPORTED;
}

JvmanEnvironmentStatus jvman_environment_broadcast_change(void) {
    return JVMAN_ENV_UNSUPPORTED;
}

#endif
