#include "runtime/ui.h"

#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "json_service.h"
#include "spisor/json_value.h"
#include "lists.h"
#include "runtime/lists.h"
#include "ui.h"
#include "ui_definition.h"
#include "ui_refusal.h"

#define NAVIGATION_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_navigation_argument, ui)
#define NAVIGATION_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_navigation_answer, storage)
#define SETTINGS_VIEW_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_settings_view_argument, description)
#define SETTINGS_VIEW_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_settings_view_answer, storage)
#define SCREEN_ARGUMENT_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_screen_argument, reply_count)
#define PLAN_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_plan_answer, storage)
#define SCREEN_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_screen, storage)
#define PLUGIN_LIST_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_plugin_list_argument, available)
#define PLUGIN_LIST_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_plugin_list_answer, storage)
#define ROUND_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_screen_argument, round)
#define ACTION_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_action_argument, item_count)
#define ACTION_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_action_answer, storage)
#define MOST_PLAN_ROUNDS 16
#define TEXT_OF(number) #number
#define TEXT(number) TEXT_OF(number)

static const char *const COMPONENT_NAMES[] = {NULL, "form", "field"};

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

int runtime_ui_is_manifest(const spisor_value *value) {
    const spisor_value *id = spisor_value_member(value, "id", 2);
    return value != NULL && value->kind == SPISOR_VALUE_RECORD && id != NULL && id->kind == SPISOR_VALUE_TEXT;
}

int runtime_ui_is_manifest_list(const spisor_value *value) {
    if (value == NULL || value->kind != SPISOR_VALUE_LIST) {
        return 0;
    }
    for (size_t index = 0; index < value->count; index++) {
        if (!runtime_ui_is_manifest(&value->items[index])) {
            return 0;
        }
    }
    return 1;
}

spisor_status runtime_ui_check_value(const spisor_value *value, spisor_value_kind kind) {
    spisor_status status = value == NULL || value->kind == SPISOR_VALUE_NONE ? SPISOR_MALFORMED
                                                                             : spisor_value_check(value);
    return status == SPISOR_OK && kind != SPISOR_VALUE_NONE && value->kind != (uint32_t)kind ? SPISOR_MALFORMED
                                                                                              : status;
}

int runtime_ui_add_value(cJSON *object, const char *name, const spisor_value *value) {
    cJSON *json = NULL;
    if (spisor_json_of_value(value, &json) != SPISOR_OK) {
        cJSON_Delete(json);
        return 0;
    }
    return runtime_add_owned(object, name, json);
}

/** {name: value} for a value already checked; NULL when memory runs out. */
static cJSON *value_call(const char *name, const spisor_value *value) {
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && !runtime_ui_add_value(call, name, value)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

typedef int (*value_shape)(const spisor_value *value);

/** MALFORMED, refused as `member`, unless the value is well-formed, of `kind` and, when given, of `shape`. */
static spisor_status check_member(spisor_refusal *refusal, const spisor_value *value, spisor_value_kind kind,
                                  value_shape shape, const char *member) {
    spisor_status status = runtime_ui_check_value(value, kind);
    if (status == SPISOR_OK && shape != NULL && !shape(value)) {
        status = SPISOR_MALFORMED;
    }
    return status == SPISOR_MALFORMED ? runtime_refuse_member(refusal, member, RUNTIME_UI_HEADER) : status;
}

static spisor_status fill_entry(void **storage, const cJSON *json, runtime_ui_entry *entry) {
    entry->kind = runtime_ui_kind_of(text_of(json, "kind"));
    entry->pinned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "pinned")) ? 1 : 0;
    return runtime_storage_copy(storage, text_of(json, "plugin"), &entry->plugin) &&
                   runtime_storage_copy(storage, text_of(json, "id"), &entry->id) &&
                   runtime_storage_copy(storage, text_of(json, "title"), &entry->title)
               ? SPISOR_OK
               : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_navigation(const cJSON *answered, void *filled) {
    runtime_ui_navigation_answer *answer = filled;
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(answered, "entries");
    const cJSON *entry;
    runtime_ui_entry *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(entries),
                                                           sizeof *copies, (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->entries = copies;
    cJSON_ArrayForEach(entry, entries) {
        if (status == SPISOR_OK) {
            status = fill_entry(&answer->storage, entry, &copies[answer->entry_count++]);
        }
    }
    return status == SPISOR_OK ? runtime_storage_value(&answer->storage,
                                                       cJSON_GetObjectItemCaseSensitive(answered, "preferences"),
                                                       &answer->preferences)
                               : status;
}

static spisor_status navigation(void *context, const runtime_ui_navigation_argument *argument,
                                runtime_ui_navigation_answer *answer, spisor_refusal *refusal) {
    cJSON *answered;
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, NAVIGATION_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, NAVIGATION_ANSWER_SHAPE, answer->entry_count)) {
        return spisor_refuse_shape(refusal, "ui's navigation");
    }
    if (argument->descriptions == NULL || argument->ui == NULL) {
        return runtime_refuse_member(refusal, argument->ui == NULL ? "ui" : "descriptions", RUNTIME_UI_HEADER);
    }
    status = check_member(refusal, argument->descriptions, SPISOR_VALUE_LIST, runtime_ui_is_manifest_list,
                          "descriptions");
    if (status == SPISOR_OK) {
        status = check_member(refusal, argument->ui, SPISOR_VALUE_RECORD, NULL, "ui");
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = value_call("descriptions", argument->descriptions);
    if (call != NULL && !runtime_ui_add_value(call, "ui", argument->ui)) {
        cJSON_Delete(call);
        call = NULL;
    }
    answered = call == NULL ? NULL
                            : runtime_ui_navigation(cJSON_GetObjectItemCaseSensitive(call, "descriptions"),
                                                    cJSON_GetObjectItemCaseSensitive(call, "ui"));
    cJSON_Delete(call);
    return runtime_serve_answered(answered, NULL, fill_navigation, answer, NAVIGATION_ANSWER_SHAPE, refusal);
}

static spisor_status fill_settings_node(void **storage, const cJSON *json, runtime_ui_settings_node *node) {
    const char *component = text_of(json, "component");
    node->component = component != NULL && strcmp(component, COMPONENT_NAMES[RUNTIME_UI_COMPONENT_FIELD]) == 0
                          ? RUNTIME_UI_COMPONENT_FIELD
                          : RUNTIME_UI_COMPONENT_FORM;
    return runtime_storage_copy(storage, text_of(json, "id"), &node->id) &&
                   runtime_storage_copy(storage, text_of(json, "parent"), &node->parent) &&
                   runtime_storage_copy(storage, text_of(json, "field"), &node->field) &&
                   runtime_storage_copy(storage, text_of(json, "label"), &node->label)
               ? SPISOR_OK
               : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_settings_view(const cJSON *answered, void *filled) {
    runtime_ui_settings_view_answer *answer = filled;
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(answered, "nodes");
    const cJSON *node;
    runtime_ui_settings_node *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_copy(&answer->storage, text_of(answered, "title"), &answer->title) ||
        !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(nodes), sizeof *copies,
                                    (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->nodes = copies;
    cJSON_ArrayForEach(node, nodes) {
        if (status == SPISOR_OK) {
            status = fill_settings_node(&answer->storage, node, &copies[answer->node_count++]);
        }
    }
    return status;
}

static spisor_status settings_view(void *context, const runtime_ui_settings_view_argument *argument,
                                   runtime_ui_settings_view_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, SETTINGS_VIEW_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, SETTINGS_VIEW_ANSWER_SHAPE, answer->node_count)) {
        return spisor_refuse_shape(refusal, "ui's settings_view");
    }
    status = check_member(refusal, argument->description, SPISOR_VALUE_RECORD, runtime_ui_is_manifest,
                          "description");
    if (status != SPISOR_OK) {
        return status;
    }
    call = value_call("description", argument->description);
    answered = call == NULL ? NULL
                            : runtime_ui_settings_view(cJSON_GetObjectItemCaseSensitive(call, "description"),
                                                       &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_settings_view, answer, SETTINGS_VIEW_ANSWER_SHAPE,
                                  refusal);
}

/** Answers plan or render, `screen_function`, for a checked argument, filling the answer with `fill`. */
static spisor_status serve_screen(cJSON *(*screen_function)(const cJSON *, const cJSON *, const cJSON *, cJSON **),
                                  const runtime_ui_screen_argument *argument, runtime_answer_filler fill, void *answer,
                                  size_t answer_shape, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *call = runtime_ui_screen_call(argument);
    cJSON *answered = call == NULL ? NULL
                                   : screen_function(cJSON_GetObjectItemCaseSensitive(call, "descriptions"),
                                                     cJSON_GetObjectItemCaseSensitive(call, "screen"),
                                                     cJSON_GetObjectItemCaseSensitive(call, "answers"),
                                                     &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill, answer, answer_shape, refusal);
}

static uint32_t round_of(const runtime_ui_screen_argument *argument) {
    return spisor_has_shape(argument->size, ROUND_SHAPE) ? argument->round : 0;
}

static spisor_status refuse_past_rounds(const runtime_ui_screen_argument *argument, spisor_refusal *refusal) {
    return runtime_refuse_json(
        refusal, runtime_ui_refusal("bind no node to an answer that only a deeper chain of bindings can give",
                                    argument->screen->plugin,
                                    "'s screen still names bindings to call after " TEXT(MOST_PLAN_ROUNDS)
                                    " rounds of plan",
                                    NULL));
}

static spisor_status plan(void *context, const runtime_ui_screen_argument *argument, runtime_ui_plan_answer *answer,
                          spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, SCREEN_ARGUMENT_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, PLAN_ANSWER_SHAPE, answer->call_count)) {
        return spisor_refuse_shape(refusal, "ui's plan");
    }
    status = runtime_ui_check_screen_argument(argument, refusal);
    if (status == SPISOR_OK && round_of(argument) >= MOST_PLAN_ROUNDS) {
        return refuse_past_rounds(argument, refusal);
    }
    return status != SPISOR_OK ? status
                               : serve_screen(runtime_ui_plan, argument, runtime_ui_fill_plan, answer,
                                              PLAN_ANSWER_SHAPE, refusal);
}

static spisor_status render(void *context, const runtime_ui_screen_argument *argument, runtime_ui_screen *answer,
                            spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, SCREEN_ARGUMENT_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, SCREEN_SHAPE, answer->node_count)) {
        return spisor_refuse_shape(refusal, "ui's render");
    }
    status = runtime_ui_check_screen_argument(argument, refusal);
    return status != SPISOR_OK ? status
                               : serve_screen(runtime_ui_render, argument, runtime_ui_fill_screen, answer,
                                              SCREEN_SHAPE, refusal);
}

static spisor_status check_groups(const runtime_ui_plugin_list_argument *argument, spisor_refusal *refusal) {
    const spisor_value *groups[] = {argument->installed, argument->detected, argument->available};
    static const char *const names[] = {"installed", "detected", "available"};
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; index < 3 && status == SPISOR_OK; index++) {
        if (groups[index] == NULL) {
            status = runtime_refuse_member(refusal, names[index], RUNTIME_UI_HEADER);
        }
    }
    for (size_t index = 0; index < 3 && status == SPISOR_OK; index++) {
        status = check_member(refusal, groups[index], SPISOR_VALUE_LIST, runtime_ui_is_manifest_list, names[index]);
    }
    return status;
}

static cJSON *groups_call(const runtime_ui_plugin_list_argument *argument) {
    cJSON *call = value_call("installed", argument->installed);
    if (call != NULL && !(runtime_ui_add_value(call, "detected", argument->detected) &&
                          runtime_ui_add_value(call, "available", argument->available))) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status fill_plugin_row(void **storage, const cJSON *entry, runtime_ui_plugin_row *row) {
    const cJSON *given = cJSON_GetObjectItemCaseSensitive(entry, "row");
    row->state = runtime_list_state_of(text_of(entry, "state"));
    row->unavailable = row->state == RUNTIME_LIST_INSTALLED ? 0 : 1;
    row->install = row->unavailable;
    if (!runtime_storage_copy(storage, text_of(given, "id"), &row->id)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return runtime_storage_value(storage, given, &row->row);
}

static spisor_status fill_plugin_list(const cJSON *answered, void *filled) {
    runtime_ui_plugin_list_answer *answer = filled;
    const cJSON *entry;
    runtime_ui_plugin_row *rows;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_copy(&answer->storage, "Plugins", &answer->title) ||
        !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(answered), sizeof *rows,
                                    (void **)&rows)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->rows = rows;
    cJSON_ArrayForEach(entry, answered) {
        if (status == SPISOR_OK) {
            status = fill_plugin_row(&answer->storage, entry, &rows[answer->row_count++]);
        }
    }
    return status;
}

static spisor_status plugin_list(void *context, const runtime_ui_plugin_list_argument *argument,
                                 runtime_ui_plugin_list_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, PLUGIN_LIST_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, PLUGIN_LIST_ANSWER_SHAPE, answer->row_count)) {
        return spisor_refuse_shape(refusal, "ui's plugin_list");
    }
    status = check_groups(argument, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    call = groups_call(argument);
    answered = call == NULL ? NULL
                            : runtime_lists_plugins(cJSON_GetObjectItemCaseSensitive(call, "installed"),
                                                    cJSON_GetObjectItemCaseSensitive(call, "detected"),
                                                    cJSON_GetObjectItemCaseSensitive(call, "available"),
                                                    &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_plugin_list, answer, PLUGIN_LIST_ANSWER_SHAPE,
                                  refusal);
}

static spisor_status check_action_argument(const runtime_ui_action_argument *argument, spisor_refusal *refusal) {
    int are_items = !argument->has_items || argument->item_count == 0 ||
                    (argument->items != NULL && runtime_are_arguments(argument->items, argument->item_count));
    spisor_status status;
    if (argument->action == NULL || argument->has_items > 1 || !are_items) {
        return runtime_refuse_member(refusal, argument->action == NULL ? "action" : "items", RUNTIME_UI_HEADER);
    }
    status = check_member(refusal, argument->description, SPISOR_VALUE_RECORD, runtime_ui_is_manifest, "description");
    return status == SPISOR_OK && argument->input != NULL
               ? check_member(refusal, argument->input, SPISOR_VALUE_RECORD, NULL, "input")
               : status;
}

/** The description, the input when given and the row as { item?, items? }, as action takes them; NULL out of memory. */
static cJSON *action_call(const runtime_ui_action_argument *argument) {
    cJSON *call = value_call("description", argument->description);
    cJSON *row = call == NULL ? NULL : cJSON_AddObjectToObject(call, "row");
    cJSON *items = row == NULL || !argument->has_items ? NULL : cJSON_AddArrayToObject(row, "items");
    int built = row != NULL && (!argument->has_items || items != NULL) &&
                (argument->item == NULL || cJSON_AddStringToObject(row, "item", argument->item) != NULL) &&
                (argument->input == NULL || runtime_ui_add_value(call, "input", argument->input));
    for (size_t index = 0; built && items != NULL && index < argument->item_count; index++) {
        cJSON *item = cJSON_CreateString(argument->items[index]);
        built = item != NULL && cJSON_AddItemToArray(items, item);
        if (!built) {
            cJSON_Delete(item);
        }
    }
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status fill_action(const cJSON *answered, void *filled) {
    runtime_ui_action_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK ? runtime_ui_fill_target(&answer->storage, answered, &answer->target) : status;
}

static spisor_status action(void *context, const runtime_ui_action_argument *argument,
                            runtime_ui_action_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, ACTION_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, ACTION_ANSWER_SHAPE)) {
        return spisor_refuse_shape(refusal, "ui's action");
    }
    status = check_action_argument(argument, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    call = action_call(argument);
    answered = call == NULL ? NULL
                            : runtime_ui_action(cJSON_GetObjectItemCaseSensitive(call, "description"),
                                                argument->action, cJSON_GetObjectItemCaseSensitive(call, "input"),
                                                cJSON_GetObjectItemCaseSensitive(call, "row"), &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_action, answer, ACTION_ANSWER_SHAPE, refusal);
}

static void release_navigation_answer(void *context, runtime_ui_navigation_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, NAVIGATION_ANSWER_SHAPE);
}

static void release_settings_view_answer(void *context, runtime_ui_settings_view_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, SETTINGS_VIEW_ANSWER_SHAPE);
}

static void release_plan_answer(void *context, runtime_ui_plan_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PLAN_ANSWER_SHAPE);
}

static void release_screen(void *context, runtime_ui_screen *answer) {
    (void)context;
    runtime_release_stored_answer(answer, SCREEN_SHAPE);
}

static void release_plugin_list_answer(void *context, runtime_ui_plugin_list_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PLUGIN_LIST_ANSWER_SHAPE);
}

static void release_action_answer(void *context, runtime_ui_action_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ACTION_ANSWER_SHAPE);
}

static const runtime_ui_definition definition = {
    sizeof(runtime_ui_definition),
    NULL,
    navigation,
    settings_view,
    plan,
    render,
    plugin_list,
    release_navigation_answer,
    release_settings_view_answer,
    release_plan_answer,
    release_screen,
    release_plugin_list_answer,
    spisor_release_refusal,
    action,
    release_action_answer,
};

RUNTIME_DEFINITION_API const void *runtime_ui_definition_of(void) {
    return &definition;
}
