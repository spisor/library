#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "allocation.h"
#include "catalog_answer.h"
#include "catalog_merge.h"
#include "spisor/json_write.h"
#include "catalog_source.h"

static int add_skipped(cJSON *skipped, int index, char *problem) {
    cJSON *entry = cJSON_CreateObject();
    int added = problem != NULL && entry != NULL && cJSON_AddNumberToObject(entry, "index", index) != NULL &&
                cJSON_AddStringToObject(entry, "detail", problem) != NULL && cJSON_AddItemToArray(skipped, entry);
    if (!added) {
        cJSON_Delete(entry);
    }
    spisor_memory_release(problem);
    return added;
}

static int add_source(cJSON *sources, const runtime_catalog_location *source) {
    cJSON *written = runtime_catalog_source_json(source);
    if (written == NULL || !cJSON_AddItemToArray(sources, written)) {
        cJSON_Delete(written);
        return 0;
    }
    return 1;
}

static const cJSON *configured_sources(const cJSON *settings) {
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(settings, "catalog");
    const cJSON *configured = cJSON_IsObject(catalog) ? cJSON_GetObjectItemCaseSensitive(catalog, "sources") : NULL;
    return cJSON_IsArray(configured) ? configured : NULL;
}

static cJSON *read_sources(const cJSON *settings) {
    cJSON *result = cJSON_CreateObject();
    cJSON *sources = cJSON_AddArrayToObject(result, "sources");
    cJSON *skipped = cJSON_AddArrayToObject(result, "skipped");
    if (sources == NULL || skipped == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    const cJSON *configured = configured_sources(settings);
    int index = 0;
    for (const cJSON *entry = configured == NULL ? NULL : configured->child; entry != NULL; entry = entry->next) {
        runtime_catalog_location source;
        char *problem;
        int kept = runtime_catalog_source_of(entry, &source, &problem) ? add_source(sources, &source)
                                                                       : add_skipped(skipped, index, problem);
        if (!kept) {
            cJSON_Delete(result);
            return NULL;
        }
        index++;
    }
    return result;
}

cJSON *runtime_catalog_sources(const cJSON *settings) {
    return read_sources(settings);
}

static int add_shadowed(cJSON *shadowed, const char *id, const runtime_catalog_location *source) {
    cJSON *entry = cJSON_CreateObject();
    int added = entry != NULL && cJSON_AddStringToObject(entry, "id", id) != NULL &&
                runtime_catalog_attach(entry, "source", runtime_catalog_source_json(source));
    if (!added || !cJSON_AddItemToArray(shadowed, entry)) {
        cJSON_Delete(entry);
        return 0;
    }
    return 1;
}

static int merge_reading(cJSON *items, const runtime_catalog_reading *reading, cJSON *shadowed) {
    size_t count;
    const cJSON **members =
        spisor_json_members_in_javascript_order(cJSON_GetObjectItemCaseSensitive(reading->catalog, "items"), &count);
    if (members == NULL && count > 0) {
        return 0;
    }
    int merged = 1;
    for (size_t index = 0; merged && index < count; index++) {
        const char *id = members[index]->string;
        if (cJSON_GetObjectItemCaseSensitive(items, id) != NULL) {
            merged = shadowed == NULL || add_shadowed(shadowed, id, &reading->source);
            continue;
        }
        cJSON *copy = cJSON_Duplicate(members[index], 1);
        merged = copy != NULL && cJSON_AddItemToObject(items, id, copy);
        if (!merged) {
            cJSON_Delete(copy);
        }
    }
    spisor_memory_release(members);
    return merged;
}

cJSON *runtime_catalog_merge_readings(const runtime_catalog_reading *readings, size_t count, cJSON *shadowed) {
    cJSON *catalog = cJSON_CreateObject();
    cJSON *items = NULL;
    if (catalog != NULL && cJSON_AddNumberToObject(catalog, "format", 1) != NULL) {
        items = cJSON_AddObjectToObject(catalog, "items");
    }
    if (items == NULL) {
        cJSON_Delete(catalog);
        return NULL;
    }
    for (size_t index = 0; index < count; index++) {
        if (!merge_reading(items, &readings[index], shadowed)) {
            cJSON_Delete(catalog);
            return NULL;
        }
    }
    return catalog;
}

/** Reads every {source, catalog} of catalogs: 1, 0 for one that is not usable, -1 when memory runs out. */
static int read_readings(const cJSON *catalogs, runtime_catalog_reading *readings, size_t *count) {
    *count = 0;
    for (const cJSON *entry = catalogs->child; entry != NULL; entry = entry->next) {
        const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(entry, "catalog");
        char *problem = NULL;
        if (!cJSON_IsObject(catalog) || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(catalog, "items"))) {
            return 0;
        }
        if (!runtime_catalog_source_of(cJSON_GetObjectItemCaseSensitive(entry, "source"), &readings[*count].source,
                                       &problem)) {
            int out_of_memory = problem == NULL;
            spisor_memory_release(problem);
            return out_of_memory ? -1 : 0;
        }
        readings[(*count)++].catalog = catalog;
    }
    return 1;
}

static cJSON *merge(const cJSON *catalogs, cJSON **refusal) {
    size_t reading_count = (size_t)cJSON_GetArraySize(catalogs) + 1;
    runtime_catalog_reading *readings = spisor_memory_allocate(reading_count * sizeof *readings);
    if (readings == NULL) {
        return NULL;
    }
    size_t count;
    int usable = read_readings(catalogs, readings, &count);
    if (usable != 1) {
        spisor_memory_release(readings);
        return usable < 0 ? NULL : runtime_catalog_refuse_member("catalogs", "an array of {source, catalog}, each catalog with items",
                                             refusal);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *shadowed = cJSON_CreateArray();
    cJSON *catalog = shadowed == NULL ? NULL : runtime_catalog_merge_readings(readings, count, shadowed);
    spisor_memory_release(readings);
    int built = runtime_catalog_attach(result, "catalog", catalog);
    if (!(built && runtime_catalog_attach(result, "shadowed", shadowed))) {
        if (!built) {
            cJSON_Delete(shadowed);
        }
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

cJSON *runtime_catalog_merge_operation(const cJSON *argument, cJSON **refusal) {
    const cJSON *catalogs = cJSON_GetObjectItemCaseSensitive(argument, "catalogs");
    return cJSON_IsArray(catalogs) ? merge(catalogs, refusal)
                                   : runtime_catalog_refuse_member("catalogs",
                                                                   "an array of {source, catalog}, each catalog with items",
                                                                   refusal);
}
