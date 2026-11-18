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

static spisor_status removed_entry(runtime_entry_outcome removed) {
    if (removed == RUNTIME_ENTRY_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return removed == RUNTIME_ENTRY_DONE ? SPISOR_OK : SPISOR_REFUSED;
}

static spisor_status removed_under_lock(const char *lock, const char *path) {
    runtime_holders_guard *guard = NULL;
    spisor_status removed = runtime_lock_holders(lock, &guard);
    if (removed == SPISOR_OK) {
        removed = removed_entry(runtime_remove_file_entry(path));
    }
    if (guard != NULL) {
        runtime_unlock_holders(guard);
    }
    return removed;
}

/**
 * Deletes the host's holders file under its holders lock, so no prune rewrites it as it goes: SPISOR_OK when none is
 * left, SPISOR_REFUSED when one is, SPISOR_OUT_OF_MEMORY when memory ran out.
 */
static spisor_status remove_holders(const removal *request) {
    char *path_problem = NULL;
    char *lock_problem = NULL;
    char *folder = runtime_processes_folder(request->state);
    char *path = runtime_holders_path(request->state, request->id, RUNTIME_JSON_EXTENSION, &path_problem);
    char *lock = runtime_holders_path(request->state, request->id, ".lock", &lock_problem);
    int named = folder != NULL && path != NULL && lock != NULL;
    int unnamed_for_memory = (path == NULL && path_problem == NULL) || (lock == NULL && lock_problem == NULL);
    spisor_status removed = named ? SPISOR_OK : folder == NULL || unnamed_for_memory ? SPISOR_OUT_OF_MEMORY
                                                                                   : SPISOR_REFUSED;
    spisor_file_presence presence = named ? spisor_probe_file(folder) : SPISOR_FILE_MISSING;
    if (presence == SPISOR_FILE_OUT_OF_MEMORY) {
        removed = SPISOR_OUT_OF_MEMORY;
    } else if (presence == SPISOR_FILE_PRESENT) {
        removed = removed_under_lock(lock, path);
    }
    spisor_memory_release(lock_problem);
    spisor_memory_release(path_problem);
    spisor_memory_release(lock);
    spisor_memory_release(path);
    spisor_memory_release(folder);
    return removed;
}

/** Deletes the host's boot hint, so the sweep no longer keeps the libraries it names; OK when none is left. */
static spisor_status remove_boot_hint(const removal *request) {
    char *problem = NULL;
    char *hint = runtime_boot_hint_path(request->cache, request->id, &problem);
    spisor_status removed = hint != NULL      ? removed_entry(runtime_remove_file_entry(hint))
                            : problem == NULL ? SPISOR_OUT_OF_MEMORY
                                              : SPISOR_REFUSED;
    spisor_memory_release(problem);
    spisor_memory_release(hint);
    return removed;
}

static cJSON *refuse_leftover(const removal *request, const char *what) {
    const char *parts[5] = {"the ", what, " of the host ", request->id,
                            " could not be deleted; its registration was left for another attempt"};
    return runtime_cache_refusal(parts, 5, STATE_FIX);
}

/**
 * Adds the forget's flag, taken before the forget, under a constant key, which allocates nothing, so nothing can fail
 * once forgotten.
 */
static cJSON *forget_into(const removal *request, cJSON *answer, cJSON **refusal) {
    cJSON *flag = cJSON_CreateFalse();
    int forgotten = flag == NULL ? -1 : runtime_registry_forget_locked(request->state, request->cache, request->id,
                                                                       refusal);
    if (flag == NULL) {
        *refusal = NULL;
    }
    if (forgotten < 0 || !cJSON_AddItemToObjectCS(answer, "forgotten", flag)) {
        cJSON_Delete(flag);
        cJSON_Delete(answer);
        return NULL;
    }
    cJSON_SetBoolValue(flag, forgotten);
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
    spisor_status references = locked == SPISOR_OK ? runtime_remove_host_references(request->state, request->id)
                                                   : SPISOR_OK;
    spisor_status hint = locked == SPISOR_OK && references == SPISOR_OK ? remove_boot_hint(request) : SPISOR_OK;
    if (locked == SPISOR_OUT_OF_MEMORY || references == SPISOR_OUT_OF_MEMORY || hint == SPISOR_OUT_OF_MEMORY) {
        *refusal = NULL;
    } else if (locked != SPISOR_OK) {
        *refusal = refuse_leftover(request, "references file, run records and boot hint");
    } else if (references != SPISOR_OK) {
        *refusal = refuse_leftover(request, "references file or a run record");
    } else if (hint != SPISOR_OK) {
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
    cJSON *answer = runtime_changes_reverse(request->state, request->id, refusal);
    if (answer == NULL) {
        return NULL;
    }
    spisor_status holders = remove_holders(request);
    if (holders != SPISOR_OK) {
        *refusal = holders == SPISOR_OUT_OF_MEMORY ? NULL : refuse_leftover(request, "holders file");
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
