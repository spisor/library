#ifndef RUNTIME_TEST_UI_CALL_H
#define RUNTIME_TEST_UI_CALL_H

#include "spisor/allocation.h"
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog_call.h"
#include "check.h"
#include "json_value.h"
#include "runtime/ui.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the ui definition with a request a test writes as JSON, in the shape ui's operations take inside runtime, and
 * answers what the definition answered as that operation's JSON, so a test reads an answer the way it read the JSON
 * functions ui replaced. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is the
 * test's data, never the boundary.
 */

typedef enum ui_operation {
    UI_NAVIGATION,
    UI_SETTINGS_VIEW,
    UI_PLAN,
    UI_RENDER,
    UI_PLUGIN_LIST
} ui_operation;

#define UI_REQUEST_CAPACITY 128

/** Every tree and block a request was read into, held until its answer is turned into JSON. */
typedef struct ui_request {
    spisor_value_tree trees[UI_REQUEST_CAPACITY];
    size_t tree_count;
    void *blocks[UI_REQUEST_CAPACITY];
    size_t block_count;
} ui_request;

static const char *const UI_KIND_NAMES[] = {NULL, "view", "settings", "action", "host"};
static const char *const UI_STATE_NAMES[] = {NULL, "ready", "error", "unavailable"};

static inline const char *ui_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline const spisor_value *ui_request_value(ui_request *held, const cJSON *object, const char *name) {
    spisor_value_tree *tree;
    if (cJSON_GetObjectItemCaseSensitive(object, name) == NULL) {
        return NULL;
    }
    CHECK(held->tree_count < UI_REQUEST_CAPACITY);
    tree = &held->trees[held->tree_count++];
    tree->size = (uint32_t)sizeof *tree;
    CHECK(spisor_vector_value(object, name, tree));
    return &tree->root;
}

static inline void *ui_request_block(ui_request *held, size_t count, size_t width) {
    void *block = spisor_memory_allocate_zeroed(count + 1, width);
    CHECK(block != NULL && held->block_count < UI_REQUEST_CAPACITY);
    held->blocks[held->block_count++] = block;
    return block;
}

static inline void ui_request_release(ui_request *held) {
    for (size_t index = 0; index < held->tree_count; index++) {
        spisor_value_release(&held->trees[index]);
    }
    for (size_t index = 0; index < held->block_count; index++) {
        spisor_memory_release(held->blocks[index]);
    }
}

static inline uint32_t ui_index_of(const char *name, const char *const *names, uint32_t last) {
    for (uint32_t index = 1; name != NULL && index <= last; index++) {
        if (strcmp(name, names[index]) == 0) {
            return index;
        }
    }
    return 0;
}

static inline const char *const *ui_request_texts(ui_request *held, const cJSON *array, size_t *count) {
    const char **texts = ui_request_block(held, (size_t)cJSON_GetArraySize(array), sizeof *texts);
    const cJSON *item;
    *count = 0;
    cJSON_ArrayForEach(item, array) {
        texts[(*count)++] = cJSON_GetStringValue(item);
    }
    return texts;
}

static inline void ui_read_check(ui_request *held, const cJSON *json, runtime_ui_check *check) {
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(json, "members");
    check->against = ui_text(json, "against");
    check->has_members = members != NULL;
    check->members = members == NULL ? NULL : ui_request_texts(held, members, &check->member_count);
}

static inline void ui_read_checked(ui_request *held, const cJSON *array, runtime_ui_reply *reply) {
    runtime_ui_checked *checked = ui_request_block(held, (size_t)cJSON_GetArraySize(array), sizeof *checked);
    const cJSON *each;
    cJSON_ArrayForEach(each, array) {
        runtime_ui_checked *read = &checked[reply->checked_count++];
        const cJSON *issues = cJSON_GetObjectItemCaseSensitive(each, "issues");
        runtime_ui_issue *copies = ui_request_block(held, (size_t)cJSON_GetArraySize(issues), sizeof *copies);
        const cJSON *issue;
        ui_read_check(held, each, &read->check);
        cJSON_ArrayForEach(issue, issues) {
            runtime_ui_issue *copy = &copies[read->issue_count++];
            copy->message = ui_text(issue, "message");
            copy->fix = ui_text(issue, "fix");
            copy->path = ui_text(issue, "path");
        }
        read->issues = copies;
    }
    reply->checked = checked;
}

/** The outcome a JSON answer carries, 0 for none or more than one, as a caller that cannot build one would send. */
static inline uint32_t ui_outcome_of(const cJSON *json) {
    int answered = cJSON_GetObjectItemCaseSensitive(json, "answer") != NULL;
    int failed = cJSON_GetObjectItemCaseSensitive(json, "error") != NULL;
    int unavailable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "unavailable"));
    if (answered + failed + unavailable != 1 ||
        (cJSON_GetObjectItemCaseSensitive(json, "unavailable") != NULL && !unavailable)) {
        return 0;
    }
    return answered ? RUNTIME_UI_OUTCOME_ANSWERED
                    : (failed ? RUNTIME_UI_OUTCOME_FAILED : RUNTIME_UI_OUTCOME_UNAVAILABLE);
}

static inline void ui_read_reply(ui_request *held, const cJSON *json, runtime_ui_reply *reply) {
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(json, "binding");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    reply->target.plugin = ui_text(json, "plugin");
    reply->target.service = ui_text(binding, "service");
    reply->target.method = ui_text(binding, "method");
    reply->target.arguments = ui_request_value(held, json, "arguments");
    reply->outcome = ui_outcome_of(json);
    reply->answer = ui_request_value(held, json, "answer");
    ui_read_checked(held, cJSON_GetObjectItemCaseSensitive(json, "checked"), reply);
    reply->error.detail = ui_text(error, "detail");
    reply->error.fix = ui_text(error, "fix");
    reply->error.path = ui_text(error, "path");
}

static inline void ui_read_screen_argument(ui_request *held, const cJSON *request,
                                           runtime_ui_screen_argument *argument) {
    const cJSON *screen = cJSON_GetObjectItemCaseSensitive(request, "screen");
    const cJSON *answers = cJSON_GetObjectItemCaseSensitive(request, "answers");
    runtime_ui_reply *replies = ui_request_block(held, (size_t)cJSON_GetArraySize(answers), sizeof *replies);
    const cJSON *answer;
    argument->descriptions = ui_request_value(held, request, "descriptions");
    if (screen != NULL) {
        runtime_ui_entry *entry = ui_request_block(held, 1, sizeof *entry);
        entry->plugin = ui_text(screen, "plugin");
        entry->kind = ui_index_of(ui_text(screen, "kind"), UI_KIND_NAMES, RUNTIME_UI_KIND_HOST);
        entry->id = ui_text(screen, "id");
        argument->screen = entry;
    }
    cJSON_ArrayForEach(answer, answers) {
        ui_read_reply(held, answer, &replies[argument->reply_count++]);
    }
    argument->replies = replies;
}

static inline void ui_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        CHECK(cJSON_AddStringToObject(object, name, text) != NULL);
    }
}

static inline void ui_add_value(cJSON *object, const char *name, const spisor_value *value) {
    cJSON *json = NULL;
    if (value != NULL) {
        CHECK(spisor_json_of_value(value, &json) == SPISOR_OK);
        CHECK(cJSON_AddItemToObject(object, name, json));
    }
}

static inline void ui_add_texts(cJSON *object, const char *name, const char *const *texts, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(object, name);
    for (size_t index = 0; index < count; index++) {
        CHECK(cJSON_AddItemToArray(array, cJSON_CreateString(texts[index])));
    }
}

static inline cJSON *ui_navigation_json(const runtime_ui_navigation_answer *answer) {
    cJSON *tree = cJSON_CreateObject();
    cJSON *entries = cJSON_AddArrayToObject(tree, "entries");
    for (size_t index = 0; index < answer->entry_count; index++) {
        const runtime_ui_entry *entry = &answer->entries[index];
        cJSON *json = cJSON_CreateObject();
        ui_add_text(json, "plugin", entry->plugin);
        ui_add_text(json, "kind", entry->kind <= RUNTIME_UI_KIND_HOST ? UI_KIND_NAMES[entry->kind] : NULL);
        ui_add_text(json, "id", entry->id);
        ui_add_text(json, "title", entry->title);
        CHECK(cJSON_AddBoolToObject(json, "pinned", entry->pinned != 0) != NULL);
        CHECK(cJSON_AddItemToArray(entries, json));
    }
    ui_add_value(tree, "preferences", answer->preferences);
    return tree;
}

static inline cJSON *ui_settings_view_json(const runtime_ui_settings_view_answer *answer) {
    static const char *const components[] = {NULL, "form", "field"};
    cJSON *tree = cJSON_CreateObject();
    cJSON *nodes;
    ui_add_text(tree, "title", answer->title);
    nodes = cJSON_AddArrayToObject(tree, "nodes");
    for (size_t index = 0; index < answer->node_count; index++) {
        const runtime_ui_settings_node *node = &answer->nodes[index];
        cJSON *json = cJSON_CreateObject();
        ui_add_text(json, "id", node->id);
        ui_add_text(json, "parent", node->parent);
        ui_add_text(json, "component", node->component <= 2 ? components[node->component] : NULL);
        ui_add_text(json, "field", node->field);
        ui_add_text(json, "label", node->label);
        CHECK(cJSON_AddItemToArray(nodes, json));
    }
    return tree;
}

static inline cJSON *ui_call_json(const runtime_ui_call *call) {
    cJSON *json = cJSON_CreateObject();
    cJSON *binding;
    cJSON *checks;
    ui_add_text(json, "plugin", call->target.plugin);
    binding = cJSON_AddObjectToObject(json, "binding");
    ui_add_text(binding, "service", call->target.service);
    ui_add_text(binding, "method", call->target.method);
    ui_add_value(json, "arguments", call->target.arguments);
    checks = cJSON_AddArrayToObject(json, "checks");
    for (size_t index = 0; index < call->check_count; index++) {
        cJSON *check = cJSON_CreateObject();
        ui_add_text(check, "against", call->checks[index].against);
        if (call->checks[index].has_members) {
            ui_add_texts(check, "members", call->checks[index].members, call->checks[index].member_count);
        }
        CHECK(cJSON_AddItemToArray(checks, check));
    }
    return json;
}

static inline cJSON *ui_plan_json(const runtime_ui_plan_answer *answer) {
    cJSON *tree = cJSON_CreateObject();
    cJSON *calls;
    if (answer->call_count == 0) {
        CHECK(cJSON_AddTrueToObject(tree, "done") != NULL);
        return tree;
    }
    calls = cJSON_AddArrayToObject(tree, "calls");
    for (size_t index = 0; index < answer->call_count; index++) {
        CHECK(cJSON_AddItemToArray(calls, ui_call_json(&answer->calls[index])));
    }
    return tree;
}

static inline cJSON *ui_node_json(const runtime_ui_node *node) {
    cJSON *json = cJSON_CreateObject();
    ui_add_text(json, "id", node->id);
    ui_add_text(json, "component", node->component);
    ui_add_text(json, "parent", node->parent);
    ui_add_text(json, "state", node->state <= RUNTIME_UI_STATE_UNAVAILABLE ? UI_STATE_NAMES[node->state] : NULL);
    ui_add_value(json, "data", node->data);
    if (node->error != NULL) {
        cJSON *error = cJSON_AddObjectToObject(json, "error");
        ui_add_text(error, "detail", node->error->detail);
        ui_add_text(error, "fix", node->error->fix);
        ui_add_text(error, "path", node->error->path);
    }
    ui_add_value(json, "props", node->props);
    return json;
}

static inline cJSON *ui_screen_json(const runtime_ui_screen *answer) {
    cJSON *tree = cJSON_CreateObject();
    cJSON *nodes;
    ui_add_text(tree, "plugin", answer->plugin);
    ui_add_text(tree, "kind", answer->kind <= RUNTIME_UI_KIND_HOST ? UI_KIND_NAMES[answer->kind] : NULL);
    ui_add_text(tree, "view", answer->view);
    ui_add_text(tree, "title", answer->title);
    nodes = cJSON_AddArrayToObject(tree, "nodes");
    for (size_t index = 0; index < answer->node_count; index++) {
        CHECK(cJSON_AddItemToArray(nodes, ui_node_json(&answer->nodes[index])));
    }
    return tree;
}

static inline spisor_status ui_navigation_call(const runtime_ui_definition *ui, const cJSON *request,
                                               ui_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_ui_navigation_argument argument = SPISOR_INIT(runtime_ui_navigation_argument);
    runtime_ui_navigation_answer answer = SPISOR_INIT(runtime_ui_navigation_answer);
    spisor_status status;
    argument.descriptions = ui_request_value(held, request, "descriptions");
    argument.ui = ui_request_value(held, request, "ui");
    status = ui->navigation(ui->context, &argument, &answer, refusal);
    *tree = status == SPISOR_OK ? ui_navigation_json(&answer) : NULL;
    ui->release_navigation_answer(ui->context, &answer);
    return status;
}

static inline spisor_status ui_settings_view_call(const runtime_ui_definition *ui, const cJSON *request,
                                                  ui_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_ui_settings_view_argument argument = SPISOR_INIT(runtime_ui_settings_view_argument);
    runtime_ui_settings_view_answer answer = SPISOR_INIT(runtime_ui_settings_view_answer);
    spisor_status status;
    argument.description = ui_request_value(held, request, "description");
    status = ui->settings_view(ui->context, &argument, &answer, refusal);
    *tree = status == SPISOR_OK ? ui_settings_view_json(&answer) : NULL;
    ui->release_settings_view_answer(ui->context, &answer);
    return status;
}

static inline spisor_status ui_plan_call(const runtime_ui_definition *ui, const cJSON *request, ui_request *held,
                                         cJSON **tree, spisor_refusal *refusal) {
    runtime_ui_screen_argument argument = SPISOR_INIT(runtime_ui_screen_argument);
    runtime_ui_plan_answer answer = SPISOR_INIT(runtime_ui_plan_answer);
    spisor_status status;
    ui_read_screen_argument(held, request, &argument);
    status = ui->plan(ui->context, &argument, &answer, refusal);
    *tree = status == SPISOR_OK ? ui_plan_json(&answer) : NULL;
    ui->release_plan_answer(ui->context, &answer);
    return status;
}

static inline spisor_status ui_render_call(const runtime_ui_definition *ui, const cJSON *request, ui_request *held,
                                           cJSON **tree, spisor_refusal *refusal) {
    runtime_ui_screen_argument argument = SPISOR_INIT(runtime_ui_screen_argument);
    runtime_ui_screen answer = SPISOR_INIT(runtime_ui_screen);
    spisor_status status;
    ui_read_screen_argument(held, request, &argument);
    status = ui->render(ui->context, &argument, &answer, refusal);
    *tree = status == SPISOR_OK ? ui_screen_json(&answer) : NULL;
    ui->release_screen(ui->context, &answer);
    return status;
}

static inline spisor_status ui_plugin_list_call(const runtime_ui_definition *ui, const cJSON *request,
                                                ui_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_ui_plugin_list_argument argument = SPISOR_INIT(runtime_ui_plugin_list_argument);
    runtime_ui_screen answer = SPISOR_INIT(runtime_ui_screen);
    spisor_status status;
    argument.installed = ui_request_value(held, request, "installed");
    argument.detected = ui_request_value(held, request, "detected");
    argument.available = ui_request_value(held, request, "available");
    status = ui->plugin_list(ui->context, &argument, &answer, refusal);
    *tree = status == SPISOR_OK ? ui_screen_json(&answer) : NULL;
    ui->release_screen(ui->context, &answer);
    return status;
}

typedef spisor_status (*ui_caller)(const runtime_ui_definition *ui, const cJSON *request, ui_request *held,
                                   cJSON **tree, spisor_refusal *refusal);

/** Calls `operation` with `request`, which it frees, answering the definition's status and the answer as JSON. */
static inline catalog_answer ui_call(ui_operation operation, cJSON *request) {
    static const ui_caller callers[] = {ui_navigation_call, ui_settings_view_call, ui_plan_call, ui_render_call,
                                        ui_plugin_list_call};
    const runtime_ui_definition *ui = runtime_ui_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    ui_request *held = spisor_memory_allocate_zeroed(1, sizeof *held);
    catalog_answer result = {0, NULL};
    spisor_status status;
    CHECK(held != NULL);
    status = callers[operation](ui, request, held, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cJSON_CreateObject();
        ui_add_text(result.tree, "message", refusal.detail);
        ui_add_text(result.tree, "fix", refusal.fix);
    }
    ui->release_refusal(ui->context, &refusal);
    ui_request_release(held);
    spisor_memory_release(held);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
