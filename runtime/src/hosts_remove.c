#include "hosts.h"
#include "allocation.h"

#include <stdlib.h>

#include "boot_hint.h"
#include "cache_folders.h"
#include "cache_refusal.h"
#include "changes.h"
#include "directory_tree.h"
#include "holders.h"
#include "holders_lock.h"
#include "host_references.h"
#include "hosts_lock.h"
#include "hosts_request.h"
#include "json_service.h"
#include "paths.h"
#include "registry.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define STATE_FIX                                                                                    \
    "close whatever holds the host's files open, check that SPISOR_STATE names a writable folder, " \
    "then remove it again"

typedef struct removal {
    const cJSON *argument;
    const char *state;
    const char *cache;
    const char *id;
    runtime_hosts_before_forget before_forget;
    void *before_forget_context;
} removal;

static cJSON *host_request(const removal *request, const char *member) {
    cJSON *call = runtime_hosts_places_request(request->argument);
    if (call != NULL && cJSON_AddStringToObject(call, member, request->id) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Deletes the host's holders file under its holders lock, so no prune rewrites it as it goes; 1 when none is left. */
static int remove_holders(const removal *request) {
    char *path_problem = NULL;
    char *lock_problem = NULL;
    char *folder = runtime_processes_folder(request->state);
    char *path = runtime_holders_path(request->state, request->id, RUNTIME_JSON_EXTENSION, &path_problem);
    char *lock = runtime_holders_path(request->state, request->id, ".lock", &lock_problem);
    int removed = folder != NULL && path != NULL && lock != NULL;
    if (removed && spisor_file_exists(folder)) {
        runtime_holders_guard *guard = runtime_lock_holders(lock);
        removed = guard != NULL && runtime_remove_file_entry(path) == RUNTIME_ENTRY_DONE;
        if (guard != NULL) {
            runtime_unlock_holders(guard);
        }
    }
    spisor_memory_release(lock_problem);
    spisor_memory_release(path_problem);
    spisor_memory_release(lock);
    spisor_memory_release(path);
    spisor_memory_release(folder);
    return removed;
}

/** Deletes the host's boot hint, so the sweep no longer keeps the libraries it names; 1 when none is left. */
static int remove_boot_hint(const removal *request) {
    char *problem = NULL;
    char *hint = runtime_boot_hint_path(request->cache, request->id, &problem);
    int removed = hint != NULL && runtime_remove_file_entry(hint) == RUNTIME_ENTRY_DONE;
    spisor_memory_release(problem);
    spisor_memory_release(hint);
    return removed;
}

static cJSON *refuse_leftover(const removal *request, const char *what) {
    const char *parts[5] = {"the ", what, " of the host ", request->id,
                            " could not be deleted; its registration was left for another attempt"};
    return runtime_cache_refusal(parts, 5, STATE_FIX);
}

/** Adds the forget's flag under a constant key, which allocates nothing, so nothing can fail once forgotten. */
static cJSON *forget_into(const removal *request, cJSON *answer, cJSON **refusal) {
    cJSON *forgotten = runtime_call_operation(runtime_registry_forget_locked, host_request(request, "id"), refusal);
    cJSON *flag = cJSON_DetachItemFromObjectCaseSensitive(forgotten, "forgotten");
    cJSON_Delete(forgotten);
    if (flag == NULL || !cJSON_AddItemToObjectCS(answer, "forgotten", flag)) {
        cJSON_Delete(flag);
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

/**
 * Deletes what the host recorded in state and cache and then its registration, all under one hold of the
 * registry lock.
 *
 * @implNote An update write refuses a host with no registration under the same lock, so a copy of the
 * host still running cannot record anything again between these deletions and the forget.
 */
static cJSON *remove_recorded_and_forget(const removal *request, cJSON *answer, cJSON **refusal) {
    spisor_lock *guard;
    spisor_status locked = runtime_hosts_lock(request->state, &guard);
    if (locked == SPISOR_OUT_OF_MEMORY) {
        *refusal = NULL;
    } else if (locked != SPISOR_OK) {
        *refusal = refuse_leftover(request, "references file, run records and boot hint");
    } else if (!runtime_remove_host_references(request->state, request->id)) {
        *refusal = refuse_leftover(request, "references file or a run record");
    } else if (!remove_boot_hint(request)) {
        *refusal = refuse_leftover(request, "boot hint");
    } else if (request->before_forget != NULL && !request->before_forget(answer, request->before_forget_context)) {
        *refusal = NULL;
    } else {
        answer = forget_into(request, answer, refusal);
        spisor_unlock(guard);
        return answer;
    }
    spisor_unlock(guard);
    cJSON_Delete(answer);
    return NULL;
}

static cJSON *remove_host(const removal *request, cJSON **refusal) {
    cJSON *answer = runtime_call_operation(runtime_changes_reverse_request, host_request(request, "host"), refusal);
    if (answer == NULL) {
        return NULL;
    }
    if (!remove_holders(request)) {
        *refusal = refuse_leftover(request, "holders file");
        cJSON_Delete(answer);
        return NULL;
    }
    return remove_recorded_and_forget(request, answer, refusal);
}

static cJSON *remove_for(const cJSON *argument, runtime_hosts_before_forget before_forget, void *context,
                         cJSON **refusal) {
    const char *state = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(argument, "places"), "state"));
    const char *cache = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(argument, "places"), "cache"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "id"));
    if (state == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, state) || cache == NULL ||
        !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache) || id == NULL) {
        *refusal = runtime_refusal("the argument carries no usable places or id",
                                   "send {\"places\": {\"state\": \"<absolute directory>\", \"cache\": \"<absolute "
                                   "directory>\"}, \"id\": \"<host id>\"}");
        return NULL;
    }
    if (!runtime_is_plain_host_id(id)) {
        *refusal = runtime_refusal("the host id is not a plain file name, so it names no host to remove",
                                   RUNTIME_PLAIN_HOST_ID_FIX);
        return NULL;
    }
    removal request = {argument, state, cache, id, before_forget, context};
    return remove_host(&request, refusal);
}

cJSON *runtime_hosts_remove_operation(const cJSON *argument, cJSON **refusal) {
    return remove_for(argument, NULL, NULL, refusal);
}

cJSON *runtime_hosts_remove_preparing(const cJSON *argument, runtime_hosts_before_forget before_forget, void *context,
                                      cJSON **refusal) {
    return remove_for(argument, before_forget, context, refusal);
}
