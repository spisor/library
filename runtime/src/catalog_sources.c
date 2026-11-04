#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_answer.h"
#include "catalog_merge.h"
#include "catalog_order.h"
#include "catalog_source.h"
#include "spisor/json_kind.h"

static int add_skipped(cJSON *skipped, int index, char *problem) {
    cJSON *entry = cJSON_CreateObject();
    int added = problem != NULL && entry != NULL && cJSON_AddNumberToObject(entry, "index", index) != NULL &&
                cJSON_AddStringToObject(entry, "detail", problem) != NULL && cJSON_AddItemToArray(skipped, entry);
    if (!added) {
        cJSON_Delete(entry);
    }
    free(problem);
    return added;
}

static int add_source(cJSON *sources, const runtime_catalog_source *source) {
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
        runtime_catalog_source source;
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

RUNTIME_EXPORT int32_t runtime_catalog_sources(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(call.argument, "settings");
    status = cJSON_IsObject(settings) ? runtime_catalog_answer(read_sources(settings), out, out_len)
                                      : runtime_catalog_refuse_argument("settings", "the merged settings object", out, out_len);
    spisor_discard_call(&call);
    return status;
}

static int add_shadowed(cJSON *shadowed, const char *id, const runtime_catalog_source *source) {
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
        runtime_catalog_ordered_members(cJSON_GetObjectItemCaseSensitive(reading->catalog, "items"), &count);
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
    free(members);
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

static int32_t refuse_catalogs(uint8_t **out, size_t *out_len) {
    return runtime_catalog_refuse_argument("catalogs", "an array of {source, catalog}, each catalog with items", out,
                                           out_len);
}

static int read_readings(const cJSON *catalogs, runtime_catalog_reading *readings, size_t *count) {
    *count = 0;
    for (const cJSON *entry = catalogs->child; entry != NULL; entry = entry->next) {
        const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(entry, "catalog");
        char *problem = NULL;
        int usable = cJSON_IsObject(catalog) && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(catalog, "items")) &&
                     runtime_catalog_source_of(cJSON_GetObjectItemCaseSensitive(entry, "source"),
                                               &readings[*count].source, &problem);
        free(problem);
        if (!usable) {
            return 0;
        }
        readings[(*count)++].catalog = catalog;
    }
    return 1;
}

static int32_t merge(const cJSON *catalogs, uint8_t **out, size_t *out_len) {
    runtime_catalog_reading *readings = malloc(((size_t)cJSON_GetArraySize(catalogs) + 1) * sizeof *readings);
    if (readings == NULL) {
        return runtime_catalog_answer(NULL, out, out_len);
    }
    size_t count;
    if (!read_readings(catalogs, readings, &count)) {
        free(readings);
        return refuse_catalogs(out, out_len);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *shadowed = cJSON_CreateArray();
    cJSON *catalog = shadowed == NULL ? NULL : runtime_catalog_merge_readings(readings, count, shadowed);
    free(readings);
    int built = runtime_catalog_attach(result, "catalog", catalog);
    if (!(built && runtime_catalog_attach(result, "shadowed", shadowed))) {
        if (!built) {
            cJSON_Delete(shadowed);
        }
        cJSON_Delete(result);
        return runtime_catalog_answer(NULL, out, out_len);
    }
    return runtime_catalog_answer(result, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_catalog_merge(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *catalogs = cJSON_GetObjectItemCaseSensitive(call.argument, "catalogs");
    status = cJSON_IsArray(catalogs) ? merge(catalogs, out, out_len) : refuse_catalogs(out, out_len);
    spisor_discard_call(&call);
    return status;
}
