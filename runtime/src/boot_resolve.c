#include "boot_resolve.h"

#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_folders.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "cache_refusal.h"
#include "node_json_file.h"
#include "spisor/file.h"

#define WINDOWS_PLATFORM_PREFIX "win32-"
#define WINDOWS_EXECUTABLE_EXTENSION ".exe"

static cJSON *refused(const char *const *parts, size_t count) {
    return runtime_cache_refusal(parts, count, RUNTIME_REINSTALL_FIX);
}

/** The version the hint names, a copy freed with free; NULL with *refusal set when there is none. */
static char *hinted_version(const char *cache, const char *host_id, cJSON **refusal) {
    cJSON *hint = runtime_read_boot_hint(cache, host_id);
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(hint, "host");
    const char *parts[1] = {cJSON_IsString(host) ? host->valuestring : NULL};
    char *version = parts[0] == NULL ? NULL : runtime_cache_text(parts, 1);
    cJSON_Delete(hint);
    if (parts[0] == NULL) {
        const char *message[4] = {"no host version is recorded for ", host_id, " in ", cache};
        *refusal = refused(message, 4);
    }
    return version;
}

static char *version_directory(const char *cache, const char *host_id, const char *version, cJSON **refusal) {
    const char *versions_segments[3] = {cache, "bundles", host_id};
    char *versions = runtime_cache_joined(versions_segments, 3);
    char *directory = versions == NULL ? NULL : runtime_cache_resolve(versions, &version, 1);
    if (directory != NULL && (version[0] == 0 || !runtime_cache_is_strictly_inside(versions, directory))) {
        cJSON *name = cJSON_CreateString(version);
        char *quoted = name == NULL ? NULL : cJSON_PrintUnformatted(name);
        cJSON_Delete(name);
        const char *message[5] = {"the host version ", quoted == NULL ? version : quoted, " recorded for ", host_id,
                                  " is not a version"};
        *refusal = refused(message, 5);
        cJSON_free(quoted);
        free(directory);
        directory = NULL;
    }
    free(versions);
    return directory;
}

static int is_verified(const char *host_id, const char *directory, cJSON **refusal) {
    cJSON *check = runtime_cache_check_version_sizes(directory);
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(check, "kind");
    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(check, "detail");
    int intact = cJSON_IsString(kind) && strcmp(kind->valuestring, "intact") == 0;
    if (!intact && cJSON_IsString(kind) && strcmp(kind->valuestring, "absent") == 0) {
        const char *message[3] = {host_id, " is not installed in ", directory};
        *refusal = refused(message, 3);
    } else if (!intact && cJSON_IsString(detail)) {
        const char *message[3] = {host_id, " was not started: ", detail->valuestring};
        *refusal = refused(message, 3);
    }
    cJSON_Delete(check);
    return intact;
}

/** The executable's file key inside its version directory: <platform>/<host id>, with .exe on Windows. */
static char *executable_key(const char *host_id, const char *platform) {
    int windows = strncmp(platform, WINDOWS_PLATFORM_PREFIX, strlen(WINDOWS_PLATFORM_PREFIX)) == 0;
    const char *parts[4] = {platform, "/", host_id, windows ? WINDOWS_EXECUTABLE_EXTENSION : ""};
    return runtime_cache_text(parts, 4);
}

static int marker_lists(const char *directory, const char *key) {
    const char *segments[2] = {directory, RUNTIME_INSTALL_MARKER};
    char *path = runtime_cache_joined(segments, 2);
    cJSON *marker = path == NULL ? NULL : runtime_read_node_json(path);
    int listed = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(marker, "files"), key) != NULL;
    cJSON_Delete(marker);
    free(path);
    return listed;
}

static char *executable_in(const char *directory, const char *host_id, const char *platform, cJSON **refusal) {
    char *key = executable_key(host_id, platform);
    char *executable = key == NULL ? NULL : runtime_cache_resolve(directory, (const char *const *)&key, 1);
    if (executable != NULL && (!marker_lists(directory, key) || !spisor_file_exists(executable))) {
        const char *message[6] = {host_id, " in ", directory, " carries no executable for ", platform,
                                  ", so it cannot be started"};
        *refusal = refused(message, 6);
        free(executable);
        executable = NULL;
    }
    free(key);
    return executable;
}

static cJSON *resolution(const char *executable, const char *version, const char *directory) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && cJSON_AddStringToObject(answer, "executable", executable) != NULL &&
                cJSON_AddStringToObject(answer, "version", version) != NULL &&
                cJSON_AddStringToObject(answer, "directory", directory) != NULL;
    if (!built) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_resolve_booted_host(const char *cache, const char *host_id, const char *platform, cJSON **refusal) {
    *refusal = NULL;
    char *version = hinted_version(cache, host_id, refusal);
    char *directory = version == NULL ? NULL : version_directory(cache, host_id, version, refusal);
    char *executable = directory != NULL && is_verified(host_id, directory, refusal)
                           ? executable_in(directory, host_id, platform, refusal)
                           : NULL;
    cJSON *answer = executable == NULL ? NULL : resolution(executable, version, directory);
    free(executable);
    free(directory);
    free(version);
    return answer;
}
