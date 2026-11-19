#include "runtime/lists.h"

#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "json_service.h"
#include "json_value.h"
#include "lists.h"
#include "paths.h"

#define PLUGINS_SHAPE RUNTIME_SHAPE_THROUGH(runtime_lists_plugins_argument, available)
#define APPLICATIONS_SHAPE RUNTIME_SHAPE_THROUGH(runtime_lists_applications_argument, detection_count)
#define LIST_SHAPE RUNTIME_SHAPE_THROUGH(runtime_list, storage)

#define UNUSABLE_GROUP "a group is not a well-formed list of rows with a text id"
#define UNUSABLE_GROUP_FIX "send installed, detected and available each as a list of records, each with its id as text"

static int is_row(const spisor_value *row) {
    const spisor_value *id = spisor_value_member(row, "id", 2);
    return row->kind == SPISOR_VALUE_RECORD && id != NULL && id->kind == SPISOR_VALUE_TEXT;
}

static int is_row_list(const spisor_value *rows) {
    if (rows == NULL || rows->kind != SPISOR_VALUE_LIST) {
        return 0;
    }
    for (size_t index = 0; index < rows->count; index++) {
        if (!is_row(&rows->items[index])) {
            return 0;
        }
    }
    return 1;
}

/** Adds a group under name, MALFORMED for a group that is not a well-formed list of rows. */
static spisor_status add_group(cJSON *call, const char *name, const spisor_value *group) {
    cJSON *json = NULL;
    spisor_status status = spisor_value_check(group);
    if (status == SPISOR_OK && !is_row_list(group)) {
        status = SPISOR_MALFORMED;
    }
    if (status == SPISOR_OK) {
        status = spisor_json_of_value(group, &json);
    }
    if (status == SPISOR_OK && !cJSON_AddItemToObject(call, name, json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

/** One entry of a three-state list into `item`, the caller's row kept as given when `keeps_rows`. */
static spisor_status fill_item(const cJSON *entry, int keeps_rows, void **storage, runtime_list_item *item) {
    const cJSON *row = cJSON_GetObjectItemCaseSensitive(entry, "row");
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    item->state = runtime_list_state_of(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "state")));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "id"));
    const char *label = keeps_rows ? NULL : cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "label"));
    if (runtime_storage_copy(storage, id, &item->id) && runtime_storage_copy(storage, label, &item->label)) {
        status = keeps_rows ? runtime_storage_value(storage, row, &item->row) : SPISOR_OK;
    }
    return status;
}

static spisor_status fill_list(cJSON *answered, int keeps_rows, runtime_list *answer) {
    cJSON *rows = answered;
    runtime_list_item *items;
    cJSON *row;
    size_t index = 0;
    spisor_status status = SPISOR_OK;
    if (!runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(rows), sizeof *items,
                                    (void **)&items)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(row, rows) {
        if (status == SPISOR_OK) {
            status = fill_item(row, keeps_rows, &answer->storage, &items[index++]);
        }
    }
    answer->items = items;
    answer->item_count = index;
    return status;
}

/** Fills the answer from the rows a lists function answered, which it deletes, or refuses with what it refused. */
static spisor_status serve_list(cJSON *answered, cJSON *operation_refusal, int keeps_rows, runtime_list *answer,
                                spisor_refusal *refusal) {
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                            : fill_list(answered, keeps_rows, answer);
    if (answered != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, LIST_SHAPE);
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status plugins(void *context, const runtime_lists_plugins_argument *argument, runtime_list *answer,
                             spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call;
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PLUGINS_SHAPE) ||
        !RUNTIME_IS_EMPTY_COUNTED_ANSWER(answer, LIST_SHAPE, answer->item_count)) {
        return runtime_refuse_shape(refusal, "lists' plugins");
    }
    call = cJSON_CreateObject();
    if (call != NULL) {
        status = add_group(call, "installed", argument->installed);
    }
    if (status == SPISOR_OK) {
        status = add_group(call, "detected", argument->detected);
    }
    if (status == SPISOR_OK) {
        status = add_group(call, "available", argument->available);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status != SPISOR_MALFORMED ? status : runtime_refuse_as(refusal, SPISOR_MALFORMED, UNUSABLE_GROUP,
                                                                       UNUSABLE_GROUP_FIX);
    }
    answered = runtime_lists_plugins(cJSON_GetObjectItemCaseSensitive(call, "installed"),
                                     cJSON_GetObjectItemCaseSensitive(call, "detected"),
                                     cJSON_GetObjectItemCaseSensitive(call, "available"), &operation_refusal);
    cJSON_Delete(call);
    return serve_list(answered, operation_refusal, 1, answer, refusal);
}

static spisor_status add_detections(cJSON *call, const runtime_lists_applications_argument *argument) {
    cJSON *detections = cJSON_AddArrayToObject(call, "detections");
    if (detections == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (argument->detection_count > 0 && argument->detections == NULL) {
        return SPISOR_MALFORMED;
    }
    for (size_t index = 0; index < argument->detection_count; index++) {
        const runtime_list_detection *detection = &argument->detections[index];
        cJSON *json = detection->item == NULL ? NULL : cJSON_CreateObject();
        cJSON *answered;
        if (detection->item == NULL) {
            return SPISOR_MALFORMED;
        }
        if (json == NULL || !cJSON_AddItemToArray(detections, json)) {
            cJSON_Delete(json);
            return SPISOR_OUT_OF_MEMORY;
        }
        answered = cJSON_AddObjectToObject(json, "answer");
        if (cJSON_AddStringToObject(json, "item", detection->item) == NULL || answered == NULL ||
            cJSON_AddBoolToObject(answered, "installed", detection->installed != 0) == NULL ||
            (detection->refused != NULL && cJSON_AddStringToObject(json, "refused", detection->refused) == NULL)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status add_catalog(cJSON *call, const spisor_value *items) {
    cJSON *catalog = cJSON_AddObjectToObject(call, "catalog");
    cJSON *json = NULL;
    spisor_status status = catalog == NULL ? SPISOR_OUT_OF_MEMORY : spisor_json_of_value(items, &json);
    if (status == SPISOR_OK && !cJSON_AddItemToObject(catalog, "items", json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

/** The environment, catalog and detections runtime_lists_applications reads, NULL unless the answer is OK. */
static spisor_status applications_call(const runtime_lists_applications_argument *argument, cJSON **call,
                                       spisor_refusal *refusal) {
    cJSON *environment = NULL;
    spisor_status status = runtime_environment_json(argument->environment, argument->environment_count, &environment);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    *call = status == SPISOR_OK ? cJSON_CreateObject() : NULL;
    if (*call == NULL || !cJSON_AddItemToObject(*call, "environment", environment)) {
        cJSON_Delete(environment);
        cJSON_Delete(*call);
        *call = NULL;
        return SPISOR_OUT_OF_MEMORY;
    }
    status = add_catalog(*call, argument->items);
    if (status == SPISOR_OK) {
        status = add_detections(*call, argument);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status == SPISOR_MALFORMED
               ? runtime_refuse_as(refusal, SPISOR_MALFORMED, "the catalog's items or a detection is unusable",
                                   "send items as the catalog's items record and every detection with its item")
               : status;
}

static spisor_status applications(void *context, const runtime_lists_applications_argument *argument,
                                  runtime_list *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, APPLICATIONS_SHAPE) ||
        !RUNTIME_IS_EMPTY_COUNTED_ANSWER(answer, LIST_SHAPE, answer->item_count)) {
        return runtime_refuse_shape(refusal, "lists' applications");
    }
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    if (argument->items == NULL || argument->items->kind != SPISOR_VALUE_RECORD) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the catalog's items are not a record",
                                 "send items as the catalog's items record, as catalog's read answers it");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    status = applications_call(argument, &call, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    answered = runtime_lists_applications(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(call, "catalog"), "items"), argument->state,
        cJSON_GetObjectItemCaseSensitive(call, "environment"), cJSON_GetObjectItemCaseSensitive(call, "detections"),
        &operation_refusal);
    cJSON_Delete(call);
    return serve_list(answered, operation_refusal, 0, answer, refusal);
}

static void release_list(void *context, runtime_list *answer) {
    (void)context;
    runtime_release_stored_answer(answer, LIST_SHAPE);
}

static const runtime_lists_definition definition = {
    sizeof(runtime_lists_definition), NULL, plugins, applications, release_list, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_lists_definition_of(void) {
    return &definition;
}
