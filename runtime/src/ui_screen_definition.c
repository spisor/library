#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "ui_definition.h"

static const char *const KIND_NAMES[] = {NULL, "view", "settings", "action", "host"};
static const char *const STATE_NAMES[] = {NULL, "ready", "error", "unavailable"};

const char *runtime_ui_kind_name(uint32_t kind) {
    return kind >= RUNTIME_UI_KIND_VIEW && kind <= RUNTIME_UI_KIND_HOST ? KIND_NAMES[kind] : NULL;
}

static uint32_t index_of(const char *name, const char *const *names, uint32_t last) {
    for (uint32_t index = 1; name != NULL && index <= last; index++) {
        if (strcmp(name, names[index]) == 0) {
            return index;
        }
    }
    return 0;
}

uint32_t runtime_ui_kind_of(const char *name) {
    return index_of(name, KIND_NAMES, RUNTIME_UI_KIND_HOST);
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static int is_array_of(const void *array, size_t count) {
    return count == 0 || array != NULL;
}

static int are_texts_present(const char *const *texts, size_t count) {
    return is_array_of(texts, count) && (count == 0 || runtime_are_arguments(texts, count));
}

static int is_check(const runtime_ui_check *check) {
    return check->against != NULL && check->has_members <= 1 &&
           (!check->has_members || are_texts_present(check->members, check->member_count));
}

static int are_issues(const runtime_ui_issue *issues, size_t count) {
    if (!is_array_of(issues, count)) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (issues[index].message == NULL || issues[index].fix == NULL) {
            return 0;
        }
    }
    return 1;
}

/** The first reply that breaks a rule, `index` meaningless when `has_index` is 0, and the `rule` it breaks. */
typedef struct reply_fault {
    int has_index;
    size_t index;
    const char *rule;
} reply_fault;

/** The rule an answered reply's checked breaks, NULL for none. */
static const char *checked_fault(const runtime_ui_checked *checked, size_t count) {
    if (!is_array_of(checked, count)) {
        return "its checked is NULL while checked_count is not zero";
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_check(&checked[index].check)) {
            return "a check names no component, has_members outside 0 and 1, or a NULL member";
        }
        if (!are_issues(checked[index].issues, checked[index].issue_count)) {
            return "an issue carries no message or fix, or issues is NULL while issue_count is not zero";
        }
    }
    return NULL;
}

static int is_target(const runtime_ui_target *target) {
    return target->plugin != NULL && target->service != NULL && target->method != NULL && target->arguments != NULL;
}

/** The rule a reply breaks, NULL for none. */
static const char *rule_broken_by(const runtime_ui_reply *reply) {
    if (!is_target(&reply->target)) {
        return "its target lacks a plugin, service, method or arguments";
    }
    switch (reply->outcome) {
    case RUNTIME_UI_OUTCOME_ANSWERED:
        return reply->answer == NULL ? "an answered reply carries no answer"
                                     : checked_fault(reply->checked, reply->checked_count);
    case RUNTIME_UI_OUTCOME_FAILED:
        return reply->error.detail != NULL && reply->error.fix != NULL ? NULL
                                                                       : "a failed reply's error lacks a detail or fix";
    case RUNTIME_UI_OUTCOME_UNAVAILABLE:
        return NULL;
    default:
        return "its outcome is none of ANSWERED, FAILED and UNAVAILABLE";
    }
}

static reply_fault first_reply_fault(const runtime_ui_reply *replies, size_t count) {
    reply_fault fault = {0, 0, NULL};
    if (!is_array_of(replies, count)) {
        fault.rule = "replies is NULL while reply_count is not zero";
        return fault;
    }
    for (size_t index = 0; index < count && fault.rule == NULL; index++) {
        fault.rule = rule_broken_by(&replies[index]);
        fault.index = index;
        fault.has_index = fault.rule != NULL;
    }
    return fault;
}

static spisor_status refuse_reply(spisor_refusal *refusal, reply_fault fault) {
    char detail[256];
    if (fault.has_index) {
        snprintf(detail, sizeof detail, "the argument carries no usable replies: reply %zu: %s", fault.index,
                 fault.rule);
    } else {
        snprintf(detail, sizeof detail, "the argument carries no usable replies: %s", fault.rule);
    }
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, "pass each reply as runtime_ui_reply describes it");
}

static int is_screen(const runtime_ui_entry *screen) {
    return screen != NULL && screen->plugin != NULL && runtime_ui_kind_name(screen->kind) != NULL;
}

/**
 * The value checks of every reply, each walking its tree, after every check that allocates nothing has passed; a
 * MALFORMED one names its reply in `fault`.
 */
static spisor_status check_reply_values(const runtime_ui_reply *replies, size_t count, reply_fault *fault) {
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < count; index++) {
        fault->index = index;
        fault->rule = "its arguments are not a well-formed record";
        status = runtime_ui_check_value(replies[index].target.arguments, SPISOR_VALUE_RECORD);
        if (status == SPISOR_OK && replies[index].outcome == RUNTIME_UI_OUTCOME_ANSWERED) {
            fault->rule = "its answer is not a well-formed value";
            status = runtime_ui_check_value(replies[index].answer, SPISOR_VALUE_NONE);
        }
    }
    fault->has_index = status == SPISOR_MALFORMED;
    return status;
}

spisor_status runtime_ui_check_screen_argument(const runtime_ui_screen_argument *argument, spisor_refusal *refusal) {
    spisor_status status;
    reply_fault fault = first_reply_fault(argument->replies, argument->reply_count);
    if (!is_screen(argument->screen)) {
        return runtime_refuse_member(refusal, "screen", RUNTIME_UI_HEADER);
    }
    if (fault.rule != NULL) {
        return refuse_reply(refusal, fault);
    }
    if (argument->descriptions == NULL) {
        return runtime_refuse_member(refusal, "descriptions", RUNTIME_UI_HEADER);
    }
    status = runtime_ui_check_value(argument->descriptions, SPISOR_VALUE_LIST);
    if (status == SPISOR_OK && !runtime_ui_is_manifest_list(argument->descriptions)) {
        status = SPISOR_MALFORMED;
    }
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_member(refusal, "descriptions", RUNTIME_UI_HEADER);
    }
    status = status == SPISOR_OK ? check_reply_values(argument->replies, argument->reply_count, &fault) : status;
    return status == SPISOR_MALFORMED ? refuse_reply(refusal, fault) : status;
}

static int add_texts(cJSON *object, const char *name, const char *const *texts, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(object, name);
    int built = array != NULL;
    for (size_t index = 0; built && index < count; index++) {
        cJSON *text = cJSON_CreateString(texts[index]);
        built = text != NULL && cJSON_AddItemToArray(array, text);
        if (!built) {
            cJSON_Delete(text);
        }
    }
    return built;
}

static int add_check(cJSON *object, const runtime_ui_check *check) {
    return add_text(object, "against", check->against) &&
           (!check->has_members || add_texts(object, "members", check->members, check->member_count));
}

static cJSON *issue_json(const runtime_ui_issue *issue) {
    cJSON *json = cJSON_CreateObject();
    if (json != NULL && !(add_text(json, "message", issue->message) && add_text(json, "fix", issue->fix) &&
                          add_text(json, "path", issue->path))) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *checked_json(const runtime_ui_checked *checked) {
    cJSON *json = cJSON_CreateObject();
    cJSON *issues = json == NULL || !add_check(json, &checked->check) ? NULL : cJSON_AddArrayToObject(json, "issues");
    int built = issues != NULL;
    for (size_t index = 0; built && index < checked->issue_count; index++) {
        cJSON *issue = issue_json(&checked->issues[index]);
        built = issue != NULL && cJSON_AddItemToArray(issues, issue);
        if (!built) {
            cJSON_Delete(issue);
        }
    }
    if (!built) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static int add_checked(cJSON *object, const runtime_ui_checked *checked, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(object, "checked");
    int built = array != NULL;
    for (size_t index = 0; built && index < count; index++) {
        cJSON *entry = checked_json(&checked[index]);
        built = entry != NULL && cJSON_AddItemToArray(array, entry);
        if (!built) {
            cJSON_Delete(entry);
        }
    }
    return built;
}

static int add_error(cJSON *object, const runtime_ui_error *error) {
    cJSON *json = cJSON_AddObjectToObject(object, "error");
    return json != NULL && add_text(json, "detail", error->detail) && add_text(json, "fix", error->fix) &&
           add_text(json, "path", error->path);
}

static int add_outcome(cJSON *object, const runtime_ui_reply *reply) {
    if (reply->outcome == RUNTIME_UI_OUTCOME_ANSWERED) {
        return runtime_ui_add_value(object, "answer", reply->answer) &&
               add_checked(object, reply->checked, reply->checked_count);
    }
    if (reply->outcome == RUNTIME_UI_OUTCOME_FAILED) {
        return add_error(object, &reply->error);
    }
    return cJSON_AddTrueToObject(object, "unavailable") != NULL;
}

static int add_target(cJSON *object, const runtime_ui_target *target) {
    cJSON *binding = add_text(object, "plugin", target->plugin) ? cJSON_AddObjectToObject(object, "binding") : NULL;
    return binding != NULL && add_text(binding, "service", target->service) &&
           add_text(binding, "method", target->method) && runtime_ui_add_value(object, "arguments", target->arguments);
}

static int add_replies(cJSON *call, const runtime_ui_reply *replies, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(call, "answers");
    int built = array != NULL;
    for (size_t index = 0; built && index < count; index++) {
        cJSON *reply = cJSON_CreateObject();
        built = reply != NULL && cJSON_AddItemToArray(array, reply);
        if (!built) {
            cJSON_Delete(reply);
        }
        built = built && add_target(reply, &replies[index].target) && add_outcome(reply, &replies[index]);
    }
    return built;
}

static int add_screen(cJSON *call, const runtime_ui_entry *screen) {
    cJSON *json = cJSON_AddObjectToObject(call, "screen");
    return json != NULL && add_text(json, "plugin", screen->plugin) &&
           add_text(json, "kind", runtime_ui_kind_name(screen->kind)) && add_text(json, "id", screen->id);
}

cJSON *runtime_ui_screen_call(const runtime_ui_screen_argument *argument) {
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && !(runtime_ui_add_value(call, "descriptions", argument->descriptions) &&
                          add_screen(call, argument->screen) &&
                          add_replies(call, argument->replies, argument->reply_count))) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static int fill_check(void **storage, const cJSON *json, runtime_ui_check *check) {
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(json, "members");
    check->has_members = members != NULL;
    return runtime_storage_copy(storage, text_of(json, "against"), &check->against) &&
           runtime_storage_texts(storage, members, &check->members, &check->member_count);
}

static spisor_status fill_target(void **storage, const cJSON *json, runtime_ui_target *target) {
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(json, "binding");
    if (!runtime_storage_copy(storage, text_of(json, "plugin"), &target->plugin) ||
        !runtime_storage_copy(storage, text_of(binding, "service"), &target->service) ||
        !runtime_storage_copy(storage, text_of(binding, "method"), &target->method)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "arguments"), &target->arguments);
}

static spisor_status fill_call(void **storage, const cJSON *json, runtime_ui_call *call) {
    const cJSON *checks = cJSON_GetObjectItemCaseSensitive(json, "checks");
    const cJSON *check;
    runtime_ui_check *copies;
    spisor_status status = fill_target(storage, json, &call->target);
    if (status != SPISOR_OK || !runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(checks),
                                                           sizeof *copies, (void **)&copies)) {
        return status != SPISOR_OK ? status : SPISOR_OUT_OF_MEMORY;
    }
    call->checks = copies;
    cJSON_ArrayForEach(check, checks) {
        if (!fill_check(storage, check, &copies[call->check_count++])) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

spisor_status runtime_ui_fill_plan(const cJSON *answered, void *filled) {
    runtime_ui_plan_answer *answer = filled;
    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(answered, "calls");
    const cJSON *call;
    runtime_ui_call *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(calls),
                                                           sizeof *copies, (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->calls = copies;
    cJSON_ArrayForEach(call, calls) {
        if (status == SPISOR_OK) {
            status = fill_call(&answer->storage, call, &copies[answer->call_count++]);
        }
    }
    return status;
}

static int fill_error(void **storage, const cJSON *json, const runtime_ui_error **error) {
    runtime_ui_error *copy;
    if (json == NULL) {
        return 1;
    }
    copy = runtime_storage_take(storage, sizeof *copy);
    *error = copy;
    return copy != NULL && runtime_storage_copy(storage, text_of(json, "detail"), &copy->detail) &&
           runtime_storage_copy(storage, text_of(json, "fix"), &copy->fix) &&
           runtime_storage_copy(storage, text_of(json, "path"), &copy->path);
}

static spisor_status fill_node(void **storage, const cJSON *json, runtime_ui_node *node) {
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    node->state = index_of(text_of(json, "state"), STATE_NAMES, RUNTIME_UI_STATE_UNAVAILABLE);
    if (runtime_storage_copy(storage, text_of(json, "id"), &node->id) &&
        runtime_storage_copy(storage, text_of(json, "component"), &node->component) &&
        runtime_storage_copy(storage, text_of(json, "parent"), &node->parent) &&
        fill_error(storage, cJSON_GetObjectItemCaseSensitive(json, "error"), &node->error)) {
        status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "data"), &node->data);
    }
    return status == SPISOR_OK
               ? runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "props"), &node->props)
               : status;
}

spisor_status runtime_ui_fill_screen(const cJSON *answered, void *filled) {
    runtime_ui_screen *answer = filled;
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(answered, "nodes");
    const cJSON *node;
    runtime_ui_node *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    answer->kind = runtime_ui_kind_of(text_of(answered, "kind"));
    if (status != SPISOR_OK || !runtime_storage_copy(&answer->storage, text_of(answered, "plugin"), &answer->plugin) ||
        !runtime_storage_copy(&answer->storage, text_of(answered, "view"), &answer->view) ||
        !runtime_storage_copy(&answer->storage, text_of(answered, "title"), &answer->title) ||
        !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(nodes), sizeof *copies,
                                    (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->nodes = copies;
    cJSON_ArrayForEach(node, nodes) {
        if (status == SPISOR_OK) {
            status = fill_node(&answer->storage, node, &copies[answer->node_count++]);
        }
    }
    return status;
}
