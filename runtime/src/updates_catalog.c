#include "updates_catalog.h"

#include "catalog.h"
#include "catalog_answer.h"
#include "service_call.h"
#include "spisor/json_kind.h"

/** The member of a successful answer, detached; NULL when the call failed or memory ran out. */
static cJSON *answered_member(runtime_json_function function, cJSON *argument, const char *member) {
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON *answer = argument == NULL ? NULL : runtime_call_service(function, argument, &status);
    cJSON_Delete(argument);
    cJSON *detached = status == SPISOR_STATUS_SUCCESS ? cJSON_DetachItemFromObjectCaseSensitive(answer, member) : NULL;
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

cJSON *runtime_updates_sources(const cJSON *settings) {
    cJSON *empty = cJSON_IsObject(settings) ? NULL : cJSON_CreateObject();
    cJSON *sources = answered_member(runtime_catalog_sources, argument_with("settings", empty == NULL ? settings : empty),
                                     "sources");
    cJSON_Delete(empty);
    return sources;
}

cJSON *runtime_updates_current_catalog(const cJSON *sources, const char *cache) {
    cJSON *argument = argument_with("sources", sources);
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return answered_member(runtime_catalog_current, argument, "catalog");
}

cJSON *runtime_updates_resolve(const cJSON *catalog, const cJSON *candidates, const cJSON *installed, const char *kind) {
    cJSON *argument = argument_with("catalog", catalog);
    int built = argument != NULL && cJSON_AddItemReferenceToObject(argument, "enabled", (cJSON *)candidates) &&
                cJSON_AddItemReferenceToObject(argument, "installed", (cJSON *)installed) &&
                cJSON_AddStringToObject(argument, "kind", kind) != NULL;
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON *answer = built ? runtime_call_service(runtime_catalog_resolve, argument, &status) : NULL;
    cJSON_Delete(argument);
    if (status != SPISOR_STATUS_SUCCESS) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_updates_check_source(const cJSON *source, const char *cache, const char *now, double interval_hours) {
    cJSON *argument = argument_with("source", source);
    int built = argument != NULL && cJSON_AddStringToObject(argument, "cache", cache) != NULL &&
                cJSON_AddStringToObject(argument, "now", now) != NULL &&
                cJSON_AddNumberToObject(argument, "intervalHours", interval_hours) != NULL;
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON *answer = built ? runtime_call_service(runtime_catalog_check, argument, &status) : NULL;
    cJSON_Delete(argument);
    if (status != SPISOR_STATUS_SUCCESS) {
        cJSON_Delete(answer);
        return NULL;
    }
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
