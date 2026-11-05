#include "updates.h"

#include <stdlib.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
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

runtime_hosts_guard *runtime_updates_lock(const runtime_updates_request *request, cJSON **refusal) {
    runtime_hosts_guard *guard = runtime_hosts_lock(request->state);
    if (guard == NULL) {
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

runtime_hosts_guard *runtime_updates_lock_registered(const runtime_updates_request *request, cJSON **refusal) {
    runtime_hosts_guard *guard = runtime_updates_lock(request, refusal);
    cJSON *registration = NULL;
    runtime_registry_answer loaded =
        guard == NULL ? RUNTIME_REGISTRY_OK : runtime_registry_load(request->state, request->host_id, &registration);
    cJSON_Delete(registration);
    if (loaded == RUNTIME_REGISTRY_OUT_OF_MEMORY) {
        runtime_hosts_unlock(guard);
        return NULL;
    }
    if (loaded != RUNTIME_REGISTRY_OK) {
        *refusal = refuse_unregistered(request, loaded);
        runtime_hosts_unlock(guard);
        return NULL;
    }
    return guard;
}

cJSON *runtime_updates_unrecorded(char *problem) {
    const char *parts[1] = {problem};
    cJSON *refusal = problem == NULL ? NULL : runtime_cache_refusal(parts, 1, WRITABLE_STATE_FIX);
    free(problem);
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

static int32_t answer_or_refuse(cJSON *answer, cJSON *refusal, uint8_t **out, size_t *out_len) {
    if (answer != NULL || refusal == NULL) {
        return runtime_catalog_answer(answer, out, out_len);
    }
    int32_t status = spisor_fail(cJSON_GetObjectItemCaseSensitive(refusal, "message")->valuestring,
                                 cJSON_GetObjectItemCaseSensitive(refusal, "fix")->valuestring, out, out_len);
    cJSON_Delete(refusal);
    return status;
}

static int32_t refuse_host_id(const char *state, const char *host_id, uint8_t **out, size_t *out_len) {
    char *problem = NULL;
    free(runtime_host_references_path(state, host_id, &problem));
    if (problem == NULL) {
        return runtime_catalog_answer(NULL, out, out_len);
    }
    int32_t status = spisor_fail(problem, RUNTIME_PLAIN_HOST_ID_FIX, out, out_len);
    free(problem);
    return status;
}

static int32_t serve_request(const cJSON *argument, runtime_updates_operation operation, uint8_t **out,
                             size_t *out_len) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    runtime_updates_request request = {argument, absolute_place(places, "state"), absolute_place(places, "cache"),
                                       runtime_catalog_text_member(argument, "hostId"), platform_member(argument)};
    if (request.state == NULL || request.cache == NULL) {
        return runtime_catalog_refuse_argument("places", "the host's places, with an absolute state and cache", out,
                                               out_len);
    }
    if (request.host_id == NULL) {
        return runtime_catalog_refuse_argument("hostId", "the id of the host the call is for", out, out_len);
    }
    if (!runtime_is_plain_host_id(request.host_id)) {
        return refuse_host_id(request.state, request.host_id, out, out_len);
    }
    if (request.platform == NULL) {
        return runtime_catalog_refuse_argument("platform", "a platform key such as linux-x64, or leave it out", out,
                                               out_len);
    }
    cJSON *refusal = NULL;
    cJSON *answer = operation(&request, &refusal);
    return answer_or_refuse(answer, refusal, out, out_len);
}

static int32_t serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                     runtime_updates_operation operation) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = serve_request(call.argument, operation, out, out_len);
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_updates_apply(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, runtime_updates_apply_for);
}

RUNTIME_EXPORT int32_t runtime_updates_check(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, runtime_updates_check_for);
}

RUNTIME_EXPORT int32_t runtime_updates_rollback(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, runtime_updates_rollback_for);
}
