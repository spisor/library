#include "host_start.h"

#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "cache.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "host_output.h"
#include "install_runtime.h"
#include "node_json_file.h"
#include "paths.h"
#include "spisor/path.h"

#define NATIVE_RUNTIME "native"
#define JAVASCRIPT_RUNTIME "javascript"

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

static cJSON *refused(const char *first, const char *second, const char *third) {
    const char *parts[3] = {first, second, third};
    return host_refusal_of(parts, 3, HOST_REINSTALL_FIX);
}

/**
 * cache/bundles/<id>/<version>, refused unless that version is installed with every file it lists at its
 * recorded size.
 *
 * @implNote Sizes only, as spisor-boot checks at every start: hashing is for install, update and status,
 * so a start never waits on it.
 */
static char *installed_version(const host_context *context, const char *id, const char *version, cJSON **refusal) {
    const char *segments[4] = {context->cache, "bundles", id, version};
    char *directory = version == NULL ? NULL : joined(segments, 4);
    cJSON *check = directory == NULL ? NULL : runtime_cache_check_version_sizes(directory);
    const char *kind = host_text(check, "kind");
    int intact = kind != NULL && strcmp(kind, "intact") == 0;
    if (check != NULL && !intact) {
        *refusal = refused(id, " is not installed intact in ", directory);
    }
    if (!intact) {
        free(directory);
        directory = NULL;
    }
    cJSON_Delete(check);
    return directory;
}

/** The file entry names inside directory, refused when it would lie outside it. */
static char *entry_inside(const char *directory, const char *id, const char *entry, cJSON **refusal) {
    char *path = entry == NULL ? NULL : runtime_cache_resolve(directory, &entry, 1);
    if (path != NULL && !runtime_cache_is_strictly_inside(directory, path)) {
        free(path);
        path = NULL;
    }
    if (path == NULL) {
        *refusal = refused("the start recorded for ", id, " names no entry inside its bundle");
    }
    return path;
}

static cJSON *start_with(const host_context *context, const char *executable, const char *script, const char *id) {
    cJSON *start = cJSON_CreateObject();
    cJSON *arguments = start == NULL ? NULL : cJSON_AddArrayToObject(start, "arguments");
    int built = arguments != NULL && cJSON_AddStringToObject(start, "executable", executable) != NULL &&
                (script == NULL || cJSON_AddItemToArray(arguments, cJSON_CreateString(script))) &&
                cJSON_AddItemToArray(arguments, cJSON_CreateString(id)) &&
                cJSON_AddItemToObject(start, "environment", cJSON_Duplicate(context->environment, 1));
    if (!built) {
        cJSON_Delete(start);
        return NULL;
    }
    return start;
}

/** The start of the entry inside directory on runtime, with id as the host it starts. */
static cJSON *layer_start(const host_context *context, const char *runtime, const char *directory, const char *entry,
                          const char *id, cJSON **refusal) {
    char *path = entry_inside(directory, id, entry, refusal);
    int native = runtime != NULL && strcmp(runtime, NATIVE_RUNTIME) == 0;
    int javascript = runtime != NULL && strcmp(runtime, JAVASCRIPT_RUNTIME) == 0;
    char *executable = path != NULL && javascript ? runtime_javascript_executable(context->cache, context->environment, refusal)
                                                  : NULL;
    cJSON *start = NULL;
    if (path != NULL && native && !host_make_runnable(path)) {
        *refusal = refused("the entry of ", id, " cannot be made runnable");
    } else if (path != NULL && native) {
        start = start_with(context, path, NULL, id);
    } else if (executable != NULL) {
        start = start_with(context, executable, path, id);
    } else if (path != NULL && !javascript) {
        *refusal = refused("the start recorded for ", id, " names a runtime this host cannot start");
    }
    free(executable);
    free(path);
    return start;
}

cJSON *host_start_of(const host_context *context, const char *id, cJSON **refusal) {
    cJSON *registration = host_registration(context, id, refusal);
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(registration, "start");
    if (registration != NULL && !cJSON_IsObject(recorded)) {
        *refusal = refused("the background host ", id, " records no start, so it cannot be started");
    }
    char *directory = cJSON_IsObject(recorded) ? installed_version(context, id, host_text(registration, "version"), refusal) : NULL;
    cJSON *start = directory == NULL ? NULL
                                     : layer_start(context, host_text(recorded, "runtime"), directory,
                                                   host_text(recorded, "entry"), id, refusal);
    free(directory);
    cJSON_Delete(registration);
    return start;
}

int host_start_append_arguments(cJSON *start, const char *const *extra, size_t count) {
    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(start, "arguments");
    for (size_t index = 0; index < count; index++) {
        if (!cJSON_AddItemToArray(arguments, cJSON_CreateString(extra[index]))) {
            return 0;
        }
    }
    return 1;
}

static char *hinted_version(const host_context *context, const char *id) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && (cJSON_AddStringToObject(argument, "cache", context->cache) == NULL ||
                             cJSON_AddStringToObject(argument, "hostId", id) == NULL)) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    cJSON *refusal = NULL;
    cJSON *read = host_call(runtime_boot_read, argument, &refusal);
    cJSON_Delete(refusal);
    const char *host = host_text(cJSON_GetObjectItemCaseSensitive(read, "hint"), "host");
    char *version = host == NULL ? NULL : malloc(strlen(host) + 1);
    if (version != NULL) {
        strcpy(version, host);
    }
    cJSON_Delete(read);
    return version;
}

static char *entry_of_bundle(const char *directory) {
    const char *segments[2] = {directory, "host.json"};
    char *path = joined(segments, 2);
    cJSON *standard = path == NULL ? NULL : runtime_read_node_json(path);
    const char *entry = host_text(standard, "entry");
    char *copy = entry == NULL ? NULL : malloc(strlen(entry) + 1);
    if (copy != NULL) {
        strcpy(copy, entry);
    }
    cJSON_Delete(standard);
    free(path);
    return copy;
}

cJSON *host_session_start_of(const host_context *context, const char *loader_id, cJSON **refusal) {
    char *version = hinted_version(context, HOST_JAVASCRIPT_HOST_ID);
    if (version == NULL) {
        *refusal = refused("no " HOST_JAVASCRIPT_HOST_ID " version is recorded in ", context->cache,
                           ", which runs the plugins the loader's settings enable");
        return NULL;
    }
    char *directory = installed_version(context, HOST_JAVASCRIPT_HOST_ID, version, refusal);
    char *entry = directory == NULL ? NULL : entry_of_bundle(directory);
    cJSON *start = directory == NULL ? NULL
                                     : layer_start(context, JAVASCRIPT_RUNTIME, directory, entry, loader_id, refusal);
    free(entry);
    free(directory);
    free(version);
    return start;
}
