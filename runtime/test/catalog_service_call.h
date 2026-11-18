#ifndef RUNTIME_TEST_CATALOG_SERVICE_CALL_H
#define RUNTIME_TEST_CATALOG_SERVICE_CALL_H

#include <stdlib.h>
#include <string.h>

#include "spisor/allocation.h"
#include "cJSON.h"
#include "cache_call.h"
#include "catalog_call.h"
#include "check.h"
#include "json_value.h"
#include "runtime/catalog.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the catalog definition with a request a test writes as JSON, in the shape catalog's operations take inside
 * runtime, and answers what the definition answered as that operation's JSON. A refusal answers {message, fix} with
 * the definition's status. Test code only: the JSON is the test's data, never the boundary.
 */

typedef enum catalog_operation {
    CATALOG_READ,
    CATALOG_SOURCES,
    CATALOG_MERGE,
    CATALOG_CHECK,
    CATALOG_CURRENT,
    CATALOG_RESOLVE
} catalog_operation;

/** What a call holds until its answer is turned into JSON: every tree and array the request was read into. */
typedef struct catalog_request {
    spisor_value_tree trees[8];
    size_t tree_count;
    void *blocks[8];
    size_t block_count;
} catalog_request;

static inline const spisor_value *catalog_request_value(catalog_request *request, const cJSON *object,
                                                         const char *name) {
    spisor_value_tree *tree;
    if (cJSON_GetObjectItemCaseSensitive(object, name) == NULL) {
        return NULL;
    }
    CHECK(request->tree_count < 8);
    tree = &request->trees[request->tree_count++];
    tree->size = (uint32_t)sizeof *tree;
    CHECK(spisor_vector_value(object, name, tree));
    return &tree->root;
}

static inline void *catalog_request_block(catalog_request *request, size_t count, size_t width) {
    void *block = spisor_memory_allocate_zeroed(count + 1, width);
    CHECK(block != NULL && request->block_count < 8);
    request->blocks[request->block_count++] = block;
    return block;
}

static inline void catalog_request_release(catalog_request *request) {
    for (size_t index = 0; index < request->tree_count; index++) {
        spisor_value_release(&request->trees[index]);
    }
    for (size_t index = 0; index < request->block_count; index++) {
        spisor_memory_release(request->blocks[index]);
    }
}

static inline runtime_catalog_source catalog_source_of(const cJSON *json) {
    const char *kind = cache_member_text(json, "kind");
    runtime_catalog_source source = {0, NULL};
    if (kind != NULL && strcmp(kind, "local") == 0) {
        source.kind = RUNTIME_CATALOG_LOCAL;
        source.location = cache_member_text(json, "path");
    } else if (kind != NULL && strcmp(kind, "https") == 0) {
        source.kind = RUNTIME_CATALOG_HTTPS;
        source.location = cache_member_text(json, "url");
    }
    return source;
}

static inline cJSON *catalog_source_json(const runtime_catalog_source *source) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "kind", source->kind == RUNTIME_CATALOG_LOCAL ? "local" : "https");
    cache_add_text(json, source->kind == RUNTIME_CATALOG_LOCAL ? "path" : "url", source->location);
    return json;
}

static inline cJSON *catalog_record_json(const spisor_value *catalog) {
    cJSON *json = NULL;
    CHECK(catalog != NULL && spisor_json_of_value(catalog, &json) == SPISOR_OK);
    return json;
}

static inline runtime_catalog_source *catalog_sources_of(catalog_request *request, const cJSON *array, size_t *count) {
    runtime_catalog_source *sources =
        catalog_request_block(request, (size_t)cJSON_GetArraySize(array), sizeof(runtime_catalog_source));
    const cJSON *entry;
    *count = 0;
    cJSON_ArrayForEach(entry, array) {
        sources[(*count)++] = catalog_source_of(entry);
    }
    return sources;
}

static inline spisor_status catalog_read_call(const runtime_catalog_definition *catalog, const cJSON *request,
                                              catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_catalog_read_argument argument = SPISOR_INIT(runtime_catalog_read_argument);
    runtime_catalog_answer answer = SPISOR_INIT(runtime_catalog_answer);
    spisor_status status;
    (void)held;
    argument.text = cache_member_text(request, "text");
    argument.origin = cache_member_text(request, "origin");
    status = catalog->read(catalog->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "catalog", catalog_record_json(answer.catalog));
    }
    catalog->release_answer(catalog->context, &answer);
    return status;
}

static inline spisor_status catalog_sources_call(const runtime_catalog_definition *catalog, const cJSON *request,
                                                 catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_catalog_sources_argument argument = SPISOR_INIT(runtime_catalog_sources_argument);
    runtime_catalog_sources_answer answer = SPISOR_INIT(runtime_catalog_sources_answer);
    spisor_status status;
    argument.settings = catalog_request_value(held, request, "settings");
    status = catalog->sources(catalog->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *sources = cJSON_CreateArray();
        cJSON *skipped = cJSON_CreateArray();
        for (size_t index = 0; index < answer.source_count; index++) {
            cJSON_AddItemToArray(sources, catalog_source_json(&answer.sources[index]));
        }
        for (size_t index = 0; index < answer.skipped_count; index++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "index", (double)answer.skipped[index].index);
            cache_add_text(entry, "detail", answer.skipped[index].detail);
            cJSON_AddItemToArray(skipped, entry);
        }
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "sources", sources);
        cJSON_AddItemToObject(*tree, "skipped", skipped);
    }
    catalog->release_sources_answer(catalog->context, &answer);
    return status;
}

static inline spisor_status catalog_merge_call(const runtime_catalog_definition *catalog, const cJSON *request,
                                               catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    const cJSON *catalogs = cJSON_GetObjectItemCaseSensitive(request, "catalogs");
    runtime_catalog_merge_argument argument = SPISOR_INIT(runtime_catalog_merge_argument);
    runtime_catalog_merge_answer answer = SPISOR_INIT(runtime_catalog_merge_answer);
    runtime_catalog_reading *readings =
        catalog_request_block(held, (size_t)cJSON_GetArraySize(catalogs), sizeof(runtime_catalog_reading));
    const cJSON *entry;
    spisor_status status;
    cJSON_ArrayForEach(entry, catalogs) {
        runtime_catalog_reading *reading = &readings[argument.catalog_count++];
        reading->source = catalog_source_of(cJSON_GetObjectItemCaseSensitive(entry, "source"));
        reading->catalog = catalog_request_value(held, entry, "catalog");
    }
    argument.catalogs = readings;
    status = catalog->merge(catalog->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *shadowed = cJSON_CreateArray();
        for (size_t index = 0; index < answer.shadowed_count; index++) {
            cJSON *each = cJSON_CreateObject();
            cache_add_text(each, "id", answer.shadowed[index].id);
            cJSON_AddItemToObject(each, "source", catalog_source_json(&answer.shadowed[index].source));
            cJSON_AddItemToArray(shadowed, each);
        }
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "catalog", catalog_record_json(answer.catalog));
        cJSON_AddItemToObject(*tree, "shadowed", shadowed);
    }
    catalog->release_merge_answer(catalog->context, &answer);
    return status;
}

static inline spisor_status catalog_check_call(const runtime_catalog_definition *catalog, const cJSON *request,
                                               catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    static const char *const kinds[] = {"", "skipped", "unchanged", "changed", "failed"};
    const cJSON *source_json = cJSON_GetObjectItemCaseSensitive(request, "source");
    runtime_catalog_source source = catalog_source_of(source_json);
    runtime_catalog_check_argument argument = SPISOR_INIT(runtime_catalog_check_argument);
    runtime_catalog_check_answer answer = SPISOR_INIT(runtime_catalog_check_answer);
    spisor_status status;
    (void)held;
    argument.source = source_json == NULL ? NULL : &source;
    argument.cache = cache_member_text(request, "cache");
    argument.now = cache_member_text(request, "now");
    argument.interval_hours = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(request, "intervalHours"));
    status = catalog->check(catalog->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        CHECK(answer.kind >= RUNTIME_CATALOG_SKIPPED && answer.kind <= RUNTIME_CATALOG_FAILED);
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "kind", kinds[answer.kind <= RUNTIME_CATALOG_FAILED ? answer.kind : 0]);
        if (answer.catalog != NULL) {
            cJSON_AddItemToObject(*tree, "catalog", catalog_record_json(answer.catalog));
        }
        cache_add_text(*tree, "detail", answer.detail);
    }
    catalog->release_check_answer(catalog->context, &answer);
    return status;
}

static inline spisor_status catalog_current_call(const runtime_catalog_definition *catalog, const cJSON *request,
                                                 catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_catalog_current_argument argument = SPISOR_INIT(runtime_catalog_current_argument);
    runtime_catalog_answer answer = SPISOR_INIT(runtime_catalog_answer);
    spisor_status status;
    argument.sources =
        catalog_sources_of(held, cJSON_GetObjectItemCaseSensitive(request, "sources"), &argument.source_count);
    argument.cache = cache_member_text(request, "cache");
    status = catalog->current(catalog->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "catalog", catalog_record_json(answer.catalog));
    }
    catalog->release_answer(catalog->context, &answer);
    return status;
}

static inline uint32_t catalog_kind_of(const cJSON *request) {
    const char *kind = cache_member_text(request, "kind");
    if (kind == NULL || strcmp(kind, "plugin") == 0) {
        return RUNTIME_CATALOG_PLUGIN;
    }
    return strcmp(kind, "host") == 0 ? RUNTIME_CATALOG_HOST : strcmp(kind, "library") == 0 ? RUNTIME_CATALOG_LIBRARY : 0;
}

static inline void catalog_resolve_argument_of(const cJSON *request, catalog_request *held,
                                               runtime_catalog_resolve_argument *argument) {
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(request, "enabled");
    const cJSON *installed = cJSON_GetObjectItemCaseSensitive(request, "installed");
    runtime_catalog_enabled *enabled_items =
        catalog_request_block(held, (size_t)cJSON_GetArraySize(enabled), sizeof(runtime_catalog_enabled));
    runtime_catalog_installed *installed_items =
        catalog_request_block(held, (size_t)cJSON_GetArraySize(installed), sizeof(runtime_catalog_installed));
    const cJSON *entry;
    cJSON_ArrayForEach(entry, enabled) {
        enabled_items[argument->enabled_count].id = cache_member_text(entry, "id");
        enabled_items[argument->enabled_count++].pin = cache_member_text(entry, "pin");
    }
    cJSON_ArrayForEach(entry, installed) {
        runtime_catalog_installed *each = &installed_items[argument->installed_count++];
        const char **versions = catalog_request_block(held, (size_t)cJSON_GetArraySize(entry), sizeof(char *));
        const cJSON *version;
        each->id = entry->string;
        cJSON_ArrayForEach(version, entry) {
            versions[each->version_count++] = cJSON_GetStringValue(version);
        }
        each->versions = versions;
    }
    argument->catalog = catalog_request_value(held, request, "catalog");
    argument->enabled = enabled_items;
    argument->installed = installed_items;
    argument->kind = catalog_kind_of(request);
}

static inline spisor_status catalog_resolve_call(const runtime_catalog_definition *catalog, const cJSON *request,
                                                 catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_catalog_resolve_argument argument = SPISOR_INIT(runtime_catalog_resolve_argument);
    runtime_catalog_resolve_answer answer = SPISOR_INIT(runtime_catalog_resolve_answer);
    spisor_status status;
    catalog_resolve_argument_of(request, held, &argument);
    status = catalog->resolve(catalog->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *run = cJSON_CreateObject();
        cJSON *fetch = cJSON_CreateArray();
        for (size_t index = 0; index < answer.run_count; index++) {
            cache_add_text(run, answer.run[index].id, answer.run[index].version);
        }
        for (size_t index = 0; index < answer.fetch_count; index++) {
            cJSON *each = cJSON_CreateObject();
            cache_add_text(each, "id", answer.fetch[index].id);
            cache_add_text(each, "version", answer.fetch[index].version);
            cJSON_AddItemToArray(fetch, each);
        }
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "run", run);
        cJSON_AddItemToObject(*tree, "fetch", fetch);
    }
    catalog->release_resolve_answer(catalog->context, &answer);
    return status;
}

/** Calls the operation with the request, which it deletes, answering its status and its answer or refusal as JSON. */
static inline catalog_answer catalog_service_call(catalog_operation operation, cJSON *request) {
    typedef spisor_status (*caller)(const runtime_catalog_definition *, const cJSON *, catalog_request *, cJSON **,
                                    spisor_refusal *);
    static const caller callers[] = {catalog_read_call,    catalog_sources_call, catalog_merge_call,
                                     catalog_check_call,   catalog_current_call, catalog_resolve_call};
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_request held;
    catalog_answer result = {0, NULL};
    spisor_status status;
    memset(&held, 0, sizeof held);
    status = callers[operation](catalog, request, &held, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cache_refusal_tree(&refusal);
    }
    catalog->release_refusal(catalog->context, &refusal);
    catalog_request_release(&held);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
