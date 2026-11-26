#include "updates_catalog.h"

#include "cache_installed.h"
#include "catalog.h"
#include "catalog_answer.h"
#include "json_service.h"
#include "plugin_choices.h"

cJSON *runtime_updates_sources(const cJSON *settings, cJSON **refusal) {
    cJSON *empty = cJSON_IsObject(settings) ? NULL : cJSON_CreateObject();
    cJSON *read = cJSON_IsObject(settings) || empty != NULL ? runtime_catalog_sources(empty == NULL ? settings : empty)
                                                            : NULL;
    cJSON *sources = cJSON_DetachItemFromObjectCaseSensitive(read, "sources");
    *refusal = NULL;
    cJSON_Delete(empty);
    cJSON_Delete(read);
    return sources;
}

cJSON *runtime_updates_resolve_plugins(const cJSON *settings, const cJSON *defaults, const char *cache, cJSON **catalog,
                                       cJSON **refusal) {
    cJSON *sources = runtime_updates_sources(settings, refusal);
    cJSON *candidates = sources == NULL ? NULL : runtime_plugin_candidates(settings, defaults);
    cJSON *installed = candidates == NULL ? NULL : runtime_cache_installed_bundles(cache);
    cJSON *plugins = NULL;
    *catalog = installed == NULL ? NULL : runtime_catalog_current(sources, cache, refusal);
    plugins = *catalog == NULL ? NULL : runtime_catalog_resolve(*catalog, candidates, installed, "plugin", refusal);
    if (plugins == NULL) {
        cJSON_Delete(*catalog);
        *catalog = NULL;
    }
    cJSON_Delete(installed);
    cJSON_Delete(candidates);
    cJSON_Delete(sources);
    return plugins;
}

cJSON *runtime_updates_check_source(const cJSON *source, const char *cache, const char *now, double interval_hours,
                                    cJSON **refusal) {
    runtime_catalog_check_request request = {source, cache, now, interval_hours};
    cJSON *answer = runtime_catalog_check(&request, refusal);
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
