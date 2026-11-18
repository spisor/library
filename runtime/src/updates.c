#include "updates.h"
#include "allocation.h"

#include <stdlib.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
#include "json_service.h"
#include "host_references.h"
#include "platform_key.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "paths.h"
#include "registry.h"
#include "updates_request.h"

#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"
#define UNREGISTERED_FIX "install the host again to register it, or stop the copy of it that outlived its removal"
#define UNREADABLE_REGISTRATION_FIX "install the host again so its registration is rewritten"

spisor_lock *runtime_updates_lock(const runtime_updates_request *request, cJSON **refusal) {
    spisor_lock *guard;
    if (runtime_hosts_lock(request->state, &guard) == SPISOR_REFUSED) {
        const char *parts[1] = {"the registry lock under the state folder could not be taken"};
        *refusal = runtime_cache_refusal(parts, 1, WRITABLE_STATE_FIX);
    }
    return guard;
}

static cJSON *refuse_unregistered(const runtime_updates_request *request, runtime_registry_answer loaded) {
    if (loaded == RUNTIME_REGISTRY_ABSENT) {
        const char *parts[3] = {"the host ", request->host_id,
                                " is not registered, so nothing is recorded or installed for it"};
        return runtime_cache_refusal(parts, 3, UNREGISTERED_FIX);
    }
    const char *parts[3] = {"the registration of the host ", request->host_id,
                            " could not be read, so nothing is recorded or installed for it"};
    return runtime_cache_refusal(parts, 3, UNREADABLE_REGISTRATION_FIX);
}

spisor_lock *runtime_updates_lock_registered(const runtime_updates_request *request, cJSON **refusal) {
    spisor_lock *guard = runtime_updates_lock(request, refusal);
    cJSON *registration = NULL;
    runtime_registry_answer loaded =
        guard == NULL ? RUNTIME_REGISTRY_OK : runtime_registry_load(request->state, request->host_id, &registration);
    cJSON_Delete(registration);
    if (loaded == RUNTIME_REGISTRY_OUT_OF_MEMORY) {
        spisor_unlock(guard);
        return NULL;
    }
    if (loaded != RUNTIME_REGISTRY_OK) {
        *refusal = refuse_unregistered(request, loaded);
        spisor_unlock(guard);
        return NULL;
    }
    return guard;
}

cJSON *runtime_updates_unrecorded(char *problem) {
    const char *parts[1] = {problem};
    cJSON *refusal = problem == NULL ? NULL : runtime_cache_refusal(parts, 1, WRITABLE_STATE_FIX);
    spisor_memory_release(problem);
    return refusal;
}

static const char *absolute_place(const cJSON *places, const char *name) {
    const char *place = runtime_catalog_text_member(places, name);
    return place != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place) ? place : NULL;
}

static const char *platform_member(const cJSON *argument) {
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(argument, "platform");
    if (platform == NULL) {
        return runtime_platform_key();
    }
    return cJSON_IsString(platform) && runtime_is_platform_key(platform->valuestring) ? platform->valuestring : NULL;
}

static cJSON *refuse_host_id(const char *state, const char *host_id, cJSON **refusal) {
    char *problem = NULL;
    spisor_memory_release(runtime_host_references_path(state, host_id, &problem));
    *refusal = problem == NULL ? NULL : runtime_refusal(problem, RUNTIME_PLAIN_HOST_ID_FIX);
    spisor_memory_release(problem);
    return NULL;
}

static cJSON *serve_filling(const cJSON *argument, runtime_updates_operation operation,
                           runtime_updates_precommit precommit, void *context, cJSON **refusal) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    runtime_updates_request request = {argument,
                                       absolute_place(places, "state"),
                                       absolute_place(places, "cache"),
                                       runtime_catalog_text_member(argument, "hostId"),
                                       platform_member(argument),
                                       precommit,
                                       context};
    if (request.state == NULL || request.cache == NULL) {
        return runtime_catalog_refuse_member("places", "the host's places, with an absolute state and cache", refusal);
    }
    if (request.host_id == NULL) {
        return runtime_catalog_refuse_member("hostId", "the id of the host the call is for", refusal);
    }
    if (!runtime_is_plain_host_id(request.host_id)) {
        return refuse_host_id(request.state, request.host_id, refusal);
    }
    if (request.platform == NULL) {
        return runtime_catalog_refuse_member("platform", "a platform key such as linux-x64, or leave it out", refusal);
    }
    return operation(&request, refusal);
}

static cJSON *serve_request(const cJSON *argument, runtime_updates_operation operation, cJSON **refusal) {
    return serve_filling(argument, operation, NULL, NULL, refusal);
}

cJSON *runtime_updates_apply_operation(const cJSON *argument, cJSON **refusal) {
    return serve_request(argument, runtime_updates_apply_for, refusal);
}

cJSON *runtime_updates_check_operation(const cJSON *argument, cJSON **refusal) {
    return serve_request(argument, runtime_updates_check_for, refusal);
}

cJSON *runtime_updates_rollback_operation(const cJSON *argument, cJSON **refusal) {
    return serve_request(argument, runtime_updates_rollback_for, refusal);
}

cJSON *runtime_updates_rollback_filling(const cJSON *argument, runtime_updates_precommit fill, void *context,
                                       cJSON **refusal) {
    return serve_filling(argument, runtime_updates_rollback_for, fill, context, refusal);
}

cJSON *runtime_updates_recorded_operation(const cJSON *argument, cJSON **refusal) {
    return serve_request(argument, runtime_updates_recorded_for, refusal);
}
