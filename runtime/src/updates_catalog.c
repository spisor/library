#include "updates_catalog.h"

#include "catalog.h"
#include "catalog_answer.h"
#include "service_call.h"
#include "spisor/json_kind.h"

/** The service's answer; NULL with *refusal set to its {message, fix} when it refused, or when memory ran out. */
static cJSON *answer_of(runtime_json_function function, cJSON *argument, cJSON **refusal) {
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON *answer = argument == NULL ? NULL : runtime_call_service(function, argument, &status);
    cJSON_Delete(argument);
    if (status == SPISOR_STATUS_SUCCESS) {
        return answer;
    }
    if (status == SPISOR_STATUS_FAILURE && answer != NULL) {
        *refusal = answer;
    } else {
        cJSON_Delete(answer);
    }
    return NULL;
}

/** The member of a successful answer, detached, as answer_of answers it. */
static cJSON *answered_member(runtime_json_function function, cJSON *argument, const char *member, cJSON **refusal) {
    cJSON *answer = answer_of(function, argument, refusal);
    cJSON *detached = cJSON_DetachItemFromObjectCaseSensitive(answer, member);
    cJSON_Delete(answer);
    return detached;
}

static cJSON *argument_with(const char *member, const cJSON *value) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && !cJSON_AddItemReferenceToObject(argument, member, (cJSON *)value)) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

cJSON *runtime_updates_sources(const cJSON *settings, cJSON **refusal) {
    cJSON *empty = cJSON_IsObject(settings) ? NULL : cJSON_CreateObject();
    cJSON *sources = answered_member(runtime_catalog_sources, argument_with("settings", empty == NULL ? settings : empty),
                                     "sources", refusal);
    cJSON_Delete(empty);
    return sources;
}

cJSON *runtime_updates_current_catalog(const cJSON *sources, const char *cache, cJSON **refusal) {
    cJSON *argument = argument_with("sources", sources);
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return answered_member(runtime_catalog_current, argument, "catalog", refusal);
}

cJSON *runtime_updates_resolve(const cJSON *catalog, const cJSON *candidates, const cJSON *installed, const char *kind,
                               cJSON **refusal) {
    cJSON *argument = argument_with("catalog", catalog);
    int built = argument != NULL && cJSON_AddItemReferenceToObject(argument, "enabled", (cJSON *)candidates) &&
                cJSON_AddItemReferenceToObject(argument, "installed", (cJSON *)installed) &&
                cJSON_AddStringToObject(argument, "kind", kind) != NULL;
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return answer_of(runtime_catalog_resolve, argument, refusal);
}

cJSON *runtime_updates_check_source(const cJSON *source, const char *cache, const char *now, double interval_hours,
                                    cJSON **refusal) {
    cJSON *argument = argument_with("source", source);
    int built = argument != NULL && cJSON_AddStringToObject(argument, "cache", cache) != NULL &&
                cJSON_AddStringToObject(argument, "now", now) != NULL &&
                cJSON_AddNumberToObject(argument, "intervalHours", interval_hours) != NULL;
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    cJSON *answer = answer_of(runtime_catalog_check, argument, refusal);
    cJSON_DeleteItemFromObjectCaseSensitive(answer, "catalog");
    return answer;
}

const cJSON *runtime_updates_item(const cJSON *catalog, const char *id) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"), id);
    return cJSON_IsObject(item) ? item : NULL;
}

int runtime_updates_lists(const cJSON *catalog, const char *id, const char *version) {
    const cJSON *versions = cJSON_GetObjectItemCaseSensitive(runtime_updates_item(catalog, id), "versions");
    return cJSON_IsObject(versions) && cJSON_GetObjectItemCaseSensitive(versions, version) != NULL;
}
