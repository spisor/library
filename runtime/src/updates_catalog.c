#include "updates_catalog.h"

#include "catalog.h"
#include "catalog_answer.h"
#include "json_service.h"

/** The member of a successful answer, detached, as runtime_call_operation answers it. */
static cJSON *answered_member(runtime_json_operation operation, cJSON *argument, const char *member, cJSON **refusal) {
    cJSON *answer = runtime_call_operation(operation, argument, refusal);
    cJSON *detached = cJSON_DetachItemFromObjectCaseSensitive(answer, member);
    cJSON_Delete(answer);
    return detached;
}

static cJSON *argument_with(const char *member, const cJSON *value) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && !runtime_add_reference(argument, member, value)) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

cJSON *runtime_updates_sources(const cJSON *settings, cJSON **refusal) {
    cJSON *empty = cJSON_IsObject(settings) ? NULL : cJSON_CreateObject();
    cJSON *sources = answered_member(runtime_catalog_sources_operation,
                                     argument_with("settings", empty == NULL ? settings : empty), "sources", refusal);
    cJSON_Delete(empty);
    return sources;
}

cJSON *runtime_updates_current_catalog(const cJSON *sources, const char *cache, cJSON **refusal) {
    cJSON *argument = argument_with("sources", sources);
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return answered_member(runtime_catalog_current_operation, argument, "catalog", refusal);
}

cJSON *runtime_updates_resolve(const cJSON *catalog, const cJSON *candidates, const cJSON *installed, const char *kind,
                               cJSON **refusal) {
    cJSON *argument = argument_with("catalog", catalog);
    int built = argument != NULL && runtime_add_reference(argument, "enabled", candidates) &&
                runtime_add_reference(argument, "installed", installed) &&
                cJSON_AddStringToObject(argument, "kind", kind) != NULL;
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return runtime_call_operation(runtime_catalog_resolve_operation, argument, refusal);
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
    cJSON *answer = runtime_call_operation(runtime_catalog_check_operation, argument, refusal);
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
