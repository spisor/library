#include "runtime/catalog.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "catalog.h"
#include "catalog_listed_source.h"
#include "catalog_time.h"
#include "definition_support.h"
#include "json_service.h"
#include "json_value.h"
#include "paths.h"
#include "spisor/path.h"

#define HEADER "catalog.h"
#define READ_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_read_argument, origin)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_answer, storage)
#define SOURCES_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_sources_argument, settings)
#define SOURCES_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_sources_answer, storage)
#define MERGE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_merge_argument, catalog_count)
#define MERGE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_merge_answer, storage)
#define CHECK_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_check_argument, interval_hours)
#define CHECK_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_check_answer, storage)
#define CURRENT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_current_argument, cache)
#define RESOLVE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_resolve_argument, kind)
#define RESOLVE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_catalog_resolve_answer, storage)

static const char *const SOURCE_KINDS[] = {NULL, "local", "https"};
static const char *const CHECK_KINDS[] = {NULL, "skipped", "unchanged", "changed", "failed"};
static const char *const ITEM_KINDS[] = {NULL, "plugin", "host", "library"};

static int is_time(const char *text) {
    char formatted[RUNTIME_CATALOG_TIME_SIZE];
    double milliseconds;
    return text != NULL && runtime_catalog_parse_time(text, &milliseconds) &&
           runtime_catalog_format_time(milliseconds, formatted);
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** The index of `name` in `names`, a NULL-led table of `count`, or 0. */
static uint32_t index_of(const char *const *names, uint32_t count, const char *name) {
    for (uint32_t index = 1; name != NULL && index < count; index++) {
        if (strcmp(name, names[index]) == 0) {
            return index;
        }
    }
    return 0;
}

/** Whether catalog is a well-formed record with an items record: OK, MALFORMED, or OUT_OF_MEMORY from the walk. */
static spisor_status check_catalog(const spisor_value *catalog) {
    const spisor_value *items = catalog == NULL ? NULL : spisor_value_member(catalog, "items", 5);
    if (items == NULL || items->kind != SPISOR_VALUE_RECORD || catalog->kind != SPISOR_VALUE_RECORD) {
        return SPISOR_MALFORMED;
    }
    return spisor_value_check(catalog);
}

static spisor_status add_catalog(cJSON *call, const char *name, const spisor_value *catalog) {
    return runtime_add_value_json(call, name, catalog, SPISOR_VALUE_RECORD);
}

/** Adds `item` under `name`, deleting it when it cannot be added; 0 for a NULL item or a failed add. */
static int add_member(cJSON *object, const char *name, cJSON *item) {
    if (item == NULL || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

static spisor_status add_to_array(cJSON *array, cJSON *item) {
    if (item == NULL || !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static int fill_source(void **storage, const cJSON *json, runtime_catalog_source *source) {
    const char *kind = text_of(json, "kind");
    source->kind = index_of(SOURCE_KINDS, 3, kind);
    return runtime_storage_copy(storage, text_of(json, source->kind == RUNTIME_CATALOG_LOCAL ? "path" : "url"),
                                &source->location);
}

/** The answered catalog as a value tree in the storage. */
static spisor_status fill_catalog(void **storage, const cJSON *catalog, const spisor_value **filled) {
    if (runtime_storage_take(storage, 1) == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return runtime_storage_value(storage, catalog, filled);
}

static spisor_status fill_answer(const cJSON *answered, void *filled) {
    runtime_catalog_answer *answer = filled;
    return fill_catalog(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "catalog"), &answer->catalog);
}

static spisor_status read_catalog(void *context, const runtime_catalog_read_argument *argument,
                                  runtime_catalog_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, READ_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "catalog's read");
    }
    if (argument->text == NULL || !runtime_is_text(argument->origin)) {
        return runtime_refuse_member(refusal, argument->text == NULL ? "text" : "origin", HEADER);
    }
    call = cJSON_CreateObject();
    if (call != NULL && (cJSON_AddStringToObject(call, "text", argument->text) == NULL ||
                         cJSON_AddStringToObject(call, "origin", argument->origin) == NULL)) {
        cJSON_Delete(call);
        call = NULL;
    }
    return runtime_serve_operation(runtime_catalog_read_operation, call, fill_answer, answer, ANSWER_SHAPE, refusal);
}

static spisor_status fill_sources(const cJSON *answered, void *filled) {
    runtime_catalog_sources_answer *answer = filled;
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(answered, "sources");
    const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(answered, "skipped");
    runtime_catalog_source *source_array;
    runtime_catalog_skipped *skipped_array;
    const cJSON *entry;
    if (runtime_storage_take(&answer->storage, 1) == NULL ||
        !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(sources), sizeof *source_array,
                                    (void **)&source_array) ||
        !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(skipped), sizeof *skipped_array,
                                    (void **)&skipped_array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, sources) {
        if (!fill_source(&answer->storage, entry, &source_array[answer->source_count++])) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    cJSON_ArrayForEach(entry, skipped) {
        runtime_catalog_skipped *each = &skipped_array[answer->skipped_count++];
        each->index = (size_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(entry, "index"));
        if (!runtime_storage_copy(&answer->storage, text_of(entry, "detail"), &each->detail)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    answer->sources = source_array;
    answer->skipped = skipped_array;
    return SPISOR_OK;
}

static spisor_status sources(void *context, const runtime_catalog_sources_argument *argument,
                             runtime_catalog_sources_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, SOURCES_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, SOURCES_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "catalog's sources");
    }
    call = cJSON_CreateObject();
    status = call == NULL ? SPISOR_OUT_OF_MEMORY : runtime_add_value_json(call, "settings", argument->settings, SPISOR_VALUE_RECORD);
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status == SPISOR_MALFORMED ? runtime_refuse_member(refusal, "settings record", HEADER) : status;
    }
    return runtime_serve_operation(runtime_catalog_sources_operation, call, fill_sources, answer, SOURCES_ANSWER_SHAPE, refusal);
}

/** Whether every reading has a source and a catalog: OK, MALFORMED, or OUT_OF_MEMORY from a catalog's walk. */
static spisor_status check_readings(const runtime_catalog_merge_argument *argument) {
    if (argument->catalog_count > 0 && argument->catalogs == NULL) {
        return SPISOR_MALFORMED;
    }
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < argument->catalog_count; index++) {
        const runtime_catalog_reading *reading = &argument->catalogs[index];
        status = runtime_catalog_are_sources(&reading->source, 1) ? check_catalog(reading->catalog) : SPISOR_MALFORMED;
    }
    return status;
}

static spisor_status add_readings(cJSON *call, const runtime_catalog_merge_argument *argument) {
    cJSON *catalogs = cJSON_AddArrayToObject(call, "catalogs");
    spisor_status status = catalogs == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < argument->catalog_count; index++) {
        const runtime_catalog_reading *reading = &argument->catalogs[index];
        cJSON *entry = cJSON_CreateObject();
        status = add_to_array(catalogs, entry);
        if (status == SPISOR_OK && !add_member(entry, "source", runtime_catalog_listed_source_json(&reading->source))) {
            status = SPISOR_OUT_OF_MEMORY;
        }
        if (status == SPISOR_OK) {
            status = add_catalog(entry, "catalog", reading->catalog);
        }
    }
    return status;
}

static spisor_status fill_merge(const cJSON *answered, void *filled) {
    runtime_catalog_merge_answer *answer = filled;
    const cJSON *shadowed = cJSON_GetObjectItemCaseSensitive(answered, "shadowed");
    runtime_catalog_shadowed *array;
    const cJSON *entry;
    spisor_status status = fill_catalog(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "catalog"),
                                      &answer->catalog);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(shadowed),
                                                           sizeof *array, (void **)&array)) {
        return status != SPISOR_OK ? status : SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, shadowed) {
        runtime_catalog_shadowed *each = &array[answer->shadowed_count++];
        if (!runtime_storage_copy(&answer->storage, text_of(entry, "id"), &each->id) ||
            !fill_source(&answer->storage, cJSON_GetObjectItemCaseSensitive(entry, "source"), &each->source)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    answer->shadowed = array;
    return SPISOR_OK;
}

static spisor_status merge(void *context, const runtime_catalog_merge_argument *argument,
                           runtime_catalog_merge_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, MERGE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, MERGE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "catalog's merge");
    }
    status = check_readings(argument);
    if (status != SPISOR_OK) {
        return status == SPISOR_MALFORMED
                   ? runtime_refuse_member(refusal, "catalogs, each a source and its items", HEADER)
                   : status;
    }
    call = cJSON_CreateObject();
    status = call == NULL ? SPISOR_OUT_OF_MEMORY : add_readings(call, argument);
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status;
    }
    return runtime_serve_operation(runtime_catalog_merge_operation, call, fill_merge, answer, MERGE_ANSWER_SHAPE, refusal);
}

static spisor_status fill_check(const cJSON *answered, void *filled) {
    runtime_catalog_check_answer *answer = filled;
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(answered, "catalog");
    answer->kind = index_of(CHECK_KINDS, 5, text_of(answered, "kind"));
    if (runtime_storage_take(&answer->storage, 1) == NULL ||
        !runtime_storage_copy(&answer->storage, text_of(answered, "detail"), &answer->detail)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return catalog == NULL ? SPISOR_OK : fill_catalog(&answer->storage, catalog, &answer->catalog);
}

/**
 * Serves check with its answer filled before the state the check keeps is written, so an answer that runs out of
 * memory keeps nothing and the next check still answers changed.
 */
static spisor_status serve_check(cJSON *call, runtime_catalog_check_answer *answer, spisor_refusal *refusal) {
    runtime_catalog_kept_state kept;
    cJSON *operation_refusal = NULL;
    memset(&kept, 0, sizeof kept);
    cJSON *answered = call == NULL ? NULL : runtime_catalog_check_deciding(call, &operation_refusal, &kept);
    cJSON_Delete(call);
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal) : fill_check(answered, answer);
    cJSON_Delete(answered);
    status = status == SPISOR_OK ? runtime_catalog_keep_state(&kept) : status;
    if (status != SPISOR_OK && answered != NULL) {
        runtime_release_stored_answer(answer, CHECK_ANSWER_SHAPE);
    }
    if (status == SPISOR_REFUSED) {
        answered = runtime_catalog_unkept_answer(&kept);
        status = answered == NULL ? SPISOR_OUT_OF_MEMORY : fill_check(answered, answer);
        if (status != SPISOR_OK) {
            runtime_release_stored_answer(answer, CHECK_ANSWER_SHAPE);
        }
        cJSON_Delete(answered);
    }
    runtime_catalog_free_kept_state(&kept);
    return status;
}

static spisor_status check(void *context, const runtime_catalog_check_argument *argument,
                           runtime_catalog_check_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, CHECK_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, CHECK_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "catalog's check");
    }
    if (!runtime_catalog_are_sources(argument->source, 1) || !runtime_is_place(argument->cache) ||
        !is_time(argument->now)) {
        return runtime_refuse_member(refusal,
                                     !runtime_catalog_are_sources(argument->source, 1) ? "source"
                                     : !runtime_is_place(argument->cache)            ? "cache"
                                                                                     : "now",
                                     HEADER);
    }
    call = cJSON_CreateObject();
    if (call != NULL &&
        (!add_member(call, "source", runtime_catalog_listed_source_json(argument->source)) ||
         cJSON_AddStringToObject(call, "cache", argument->cache) == NULL ||
         cJSON_AddStringToObject(call, "now", argument->now) == NULL ||
         cJSON_AddNumberToObject(call, "intervalHours", argument->interval_hours) == NULL)) {
        cJSON_Delete(call);
        call = NULL;
    }
    return serve_check(call, answer, refusal);
}

static spisor_status current(void *context, const runtime_catalog_current_argument *argument,
                             runtime_catalog_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, CURRENT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "catalog's current");
    }
    if (!runtime_is_place(argument->cache) || !runtime_catalog_are_sources(argument->sources, argument->source_count)) {
        return runtime_refuse_member(refusal, !runtime_is_place(argument->cache) ? "cache" : "sources", HEADER);
    }
    call = cJSON_CreateObject();
    status = call == NULL ? SPISOR_OUT_OF_MEMORY : runtime_catalog_add_sources(call, argument->sources, argument->source_count);
    if (status == SPISOR_OK && cJSON_AddStringToObject(call, "cache", argument->cache) == NULL) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status;
    }
    return runtime_serve_operation(runtime_catalog_current_operation, call, fill_answer, answer, ANSWER_SHAPE, refusal);
}

static int are_enabled(const runtime_catalog_resolve_argument *argument) {
    if (argument->enabled_count > 0 && argument->enabled == NULL) {
        return 0;
    }
    for (size_t index = 0; index < argument->enabled_count; index++) {
        if (!runtime_is_text(argument->enabled[index].id)) {
            return 0;
        }
    }
    return 1;
}

static int is_installed_entry(const runtime_catalog_resolve_argument *argument, size_t index) {
    const runtime_catalog_installed *installed = &argument->installed[index];
    if (!runtime_is_text(installed->id) || (installed->version_count > 0 && installed->versions == NULL)) {
        return 0;
    }
    for (size_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(argument->installed[earlier].id, installed->id) == 0) {
            return 0;
        }
    }
    for (size_t version = 0; version < installed->version_count; version++) {
        if (installed->versions[version] == NULL) {
            return 0;
        }
    }
    return 1;
}

static int are_installed(const runtime_catalog_resolve_argument *argument) {
    if (argument->installed_count > 0 && argument->installed == NULL) {
        return 0;
    }
    for (size_t index = 0; index < argument->installed_count; index++) {
        if (!is_installed_entry(argument, index)) {
            return 0;
        }
    }
    return 1;
}

/** Whether the whole resolve call is well formed, checked before anything is built for it. */
static spisor_status check_resolve(const runtime_catalog_resolve_argument *argument) {
    return are_enabled(argument) && are_installed(argument) ? check_catalog(argument->catalog) : SPISOR_MALFORMED;
}

static spisor_status add_enabled(cJSON *call, const runtime_catalog_resolve_argument *argument) {
    cJSON *array = cJSON_AddArrayToObject(call, "enabled");
    for (size_t index = 0; array != NULL && index < argument->enabled_count; index++) {
        const runtime_catalog_enabled *enabled = &argument->enabled[index];
        cJSON *entry = cJSON_CreateObject();
        if (add_to_array(array, entry) != SPISOR_OK || cJSON_AddStringToObject(entry, "id", enabled->id) == NULL ||
            (enabled->pin != NULL && cJSON_AddStringToObject(entry, "pin", enabled->pin) == NULL)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return array == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status add_installed(cJSON *call, const runtime_catalog_resolve_argument *argument) {
    cJSON *object = cJSON_AddObjectToObject(call, "installed");
    for (size_t index = 0; object != NULL && index < argument->installed_count; index++) {
        const runtime_catalog_installed *installed = &argument->installed[index];
        cJSON *versions = cJSON_AddArrayToObject(object, installed->id);
        for (size_t version = 0; versions != NULL && version < installed->version_count; version++) {
            if (add_to_array(versions, cJSON_CreateString(installed->versions[version])) != SPISOR_OK) {
                return SPISOR_OUT_OF_MEMORY;
            }
        }
        if (versions == NULL) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return object == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status resolve_call(const runtime_catalog_resolve_argument *argument, cJSON *call) {
    spisor_status status = add_catalog(call, "catalog", argument->catalog);
    if (status == SPISOR_OK) {
        status = add_enabled(call, argument);
    }
    if (status == SPISOR_OK) {
        status = add_installed(call, argument);
    }
    if (status == SPISOR_OK && cJSON_AddStringToObject(call, "kind", ITEM_KINDS[argument->kind]) == NULL) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static spisor_status fill_versions(void **storage, const cJSON *json, const runtime_item_version **filled,
                                   size_t *count) {
    runtime_item_version *array;
    const cJSON *entry;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(json), sizeof *array, (void **)&array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, json) {
        runtime_item_version *each = &array[(*count)++];
        const char *id = cJSON_IsString(entry) ? entry->string : text_of(entry, "id");
        const char *version = cJSON_IsString(entry) ? entry->valuestring : text_of(entry, "version");
        if (!runtime_storage_copy(storage, id, &each->id) || !runtime_storage_copy(storage, version, &each->version)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    *filled = array;
    return SPISOR_OK;
}

static spisor_status fill_resolve(const cJSON *answered, void *filled) {
    runtime_catalog_resolve_answer *answer = filled;
    spisor_status status = runtime_storage_take(&answer->storage, 1) == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    if (status == SPISOR_OK) {
        status = fill_versions(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "run"), &answer->run,
                               &answer->run_count);
    }
    return status != SPISOR_OK ? status
                               : fill_versions(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "fetch"),
                                               &answer->fetch, &answer->fetch_count);
}

static spisor_status resolve(void *context, const runtime_catalog_resolve_argument *argument,
                             runtime_catalog_resolve_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RESOLVE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RESOLVE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "catalog's resolve");
    }
    if (argument->kind < RUNTIME_CATALOG_PLUGIN || argument->kind > RUNTIME_CATALOG_LIBRARY) {
        return runtime_refuse_member(refusal, "kind", HEADER);
    }
    status = check_resolve(argument);
    if (status != SPISOR_OK) {
        return status == SPISOR_MALFORMED
                   ? runtime_refuse_member(refusal, "items, enabled items or installed versions", HEADER)
                   : status;
    }
    call = cJSON_CreateObject();
    status = call == NULL ? SPISOR_OUT_OF_MEMORY : resolve_call(argument, call);
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status;
    }
    return runtime_serve_operation(runtime_catalog_resolve_operation, call, fill_resolve, answer, RESOLVE_ANSWER_SHAPE, refusal);
}

static void release_answer(void *context, runtime_catalog_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ANSWER_SHAPE);
}

static void release_sources_answer(void *context, runtime_catalog_sources_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, SOURCES_ANSWER_SHAPE);
}

static void release_merge_answer(void *context, runtime_catalog_merge_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, MERGE_ANSWER_SHAPE);
}

static void release_check_answer(void *context, runtime_catalog_check_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, CHECK_ANSWER_SHAPE);
}

static void release_resolve_answer(void *context, runtime_catalog_resolve_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RESOLVE_ANSWER_SHAPE);
}

static const runtime_catalog_definition definition = {
    sizeof(runtime_catalog_definition),
    NULL,
    read_catalog,
    sources,
    merge,
    check,
    current,
    resolve,
    release_answer,
    release_sources_answer,
    release_merge_answer,
    release_check_answer,
    release_resolve_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_catalog_definition_of(void) {
    return &definition;
}
