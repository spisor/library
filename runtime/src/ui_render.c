#include <string.h>

#include "json_service.h"
#include "ui.h"
#include "ui_binding.h"
#include "ui_items.h"
#include "ui_refusal.h"
#include "ui_screen.h"

static const char *const PROP_MEMBERS[] = {"size", "emphasis", "tone", "label", "text", "icon", "action", "field", "chart", "span"};
static const char *const MEASURE_MEMBERS[] = {"value", "maximum"};
static const char *const SERIES_MEMBERS[] = {"series"};
static const char *const ITEMS_FAMILY[] = {"list", "table", "tree"};

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static const cJSON *nodes_of(const ui_screen *screen) {
    return cJSON_GetObjectItemCaseSensitive(screen->view, "nodes");
}

static cJSON *refuse_answers(cJSON **refusal) {
    *refusal = runtime_refusal("the answers are no list of calls each carrying exactly one outcome",
                               "send answers as [{\"plugin\", \"binding\": {\"service\", \"method\"}, \"arguments\": {...}} "
                               "with one of \"answer\" and its \"checked\", \"error\": {\"detail\", \"fix\"} or \"unavailable\": "
                               "true], one per call ui.plan asked for");
    return NULL;
}

static cJSON *pending_call(cJSON *calls, const cJSON *call) {
    cJSON *pending;
    cJSON_ArrayForEach(pending, calls) {
        if (runtime_ui_is_same_call(pending, call)) {
            return pending;
        }
    }
    return NULL;
}

static int has_check(const cJSON *checks, const cJSON *check) {
    const cJSON *listed;
    cJSON_ArrayForEach(listed, checks) {
        if (runtime_ui_is_same_check(listed, check)) {
            return 1;
        }
    }
    return 0;
}

static int add_check(cJSON *call, cJSON *check) {
    cJSON *checks = cJSON_GetObjectItemCaseSensitive(call, "checks");
    if (checks == NULL) {
        checks = cJSON_AddArrayToObject(call, "checks");
    }
    if (checks == NULL || has_check(checks, check)) {
        cJSON_Delete(check);
        return checks != NULL;
    }
    return cJSON_AddItemToArray(checks, check);
}

/** Adds the call node needs to calls unless answers holds it, merging its check into one call already listed. */
static int plan_node(cJSON *calls, const char *plugin, const cJSON *node, const cJSON *answers) {
    cJSON *call = runtime_ui_call_of(plugin, node);
    cJSON *check = call == NULL ? NULL : runtime_ui_check_of(node);
    if (check == NULL || runtime_ui_answer_to(answers, call) != NULL) {
        cJSON_Delete(call);
        cJSON_Delete(check);
        return call != NULL && check != NULL;
    }
    cJSON *pending = pending_call(calls, call);
    if (pending != NULL) {
        cJSON_Delete(call);
        return add_check(pending, check);
    }
    if (!add_check(call, check) || !cJSON_AddItemToArray(calls, call)) {
        cJSON_Delete(call);
        return 0;
    }
    return 1;
}

static cJSON *plan_answer(cJSON *calls) {
    if (calls == NULL) {
        return NULL;
    }
    cJSON *answer = cJSON_CreateObject();
    int done = calls->child == NULL;
    int complete = answer != NULL && (done ? cJSON_AddTrueToObject(answer, "done") != NULL
                                           : cJSON_AddItemToObject(answer, "calls", calls));
    if (done || !complete) {
        cJSON_Delete(calls);
    }
    if (!complete) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *planned(const ui_screen *screen, const cJSON *answers) {
    cJSON *calls = cJSON_CreateArray();
    const cJSON *source = calls == NULL ? NULL : nodes_of(screen);
    const cJSON *node;
    cJSON_ArrayForEach(node, source) {
        if (runtime_ui_is_bound(node) && !plan_node(calls, screen->plugin, node, answers)) {
            cJSON_Delete(calls);
            return NULL;
        }
    }
    return plan_answer(calls);
}

static cJSON *plan_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *answers = cJSON_GetObjectItemCaseSensitive(argument, "answers");
    if (answers != NULL && !runtime_ui_is_answer_list(answers)) {
        return refuse_answers(refusal);
    }
    ui_screen screen;
    if (!runtime_ui_open_screen(argument, &screen, refusal)) {
        runtime_ui_close_screen(&screen);
        return NULL;
    }
    cJSON *answer = planned(&screen, answers);
    runtime_ui_close_screen(&screen);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_ui_plan(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, plan_for);
}

static int is_one_of(const char *text, const char *const *names, size_t count) {
    for (size_t index = 0; text != NULL && index < count; index++) {
        if (strcmp(text, names[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

/** A copy of the members of object named in names, the absent ones left out; NULL when memory runs out. */
static cJSON *picked(const cJSON *object, const char *const *names, size_t count) {
    cJSON *copy = cJSON_CreateObject();
    for (size_t index = 0; copy != NULL && index < count; index++) {
        const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, names[index]);
        if (member != NULL && !cJSON_AddItemToObject(copy, names[index], cJSON_Duplicate(member, 1))) {
            cJSON_Delete(copy);
            copy = NULL;
        }
    }
    return copy;
}

static cJSON *presented_answer(const cJSON *answer) {
    cJSON *copy = cJSON_Duplicate(answer, 1);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(answer, "items");
    if (copy == NULL || !cJSON_IsObject(answer) || !cJSON_IsArray(items)) {
        return copy;
    }
    cJSON *presented = runtime_ui_presented_items(items);
    if (presented == NULL || !cJSON_ReplaceItemInObjectCaseSensitive(copy, "items", presented)) {
        cJSON_Delete(presented);
        cJSON_Delete(copy);
        return NULL;
    }
    return copy;
}

/** The data a ready node carries: its chart's own branch, the items family presented, anything else as answered. */
static cJSON *data_of(const cJSON *node, const cJSON *answer) {
    const char *component = text_of(node, "component");
    if (component != NULL && strcmp(component, "chart") == 0) {
        const char *chart = text_of(node, "chart");
        int meter = chart != NULL && strcmp(chart, "meter") == 0;
        return meter ? picked(answer, MEASURE_MEMBERS, COUNT_OF(MEASURE_MEMBERS))
                     : picked(answer, SERIES_MEMBERS, COUNT_OF(SERIES_MEMBERS));
    }
    return is_one_of(component, ITEMS_FAMILY, COUNT_OF(ITEMS_FAMILY)) ? presented_answer(answer) : cJSON_Duplicate(answer, 1);
}

static int add_error(cJSON *rendered, const cJSON *source, const char *detail_member) {
    cJSON *error = cJSON_CreateObject();
    const char *path = text_of(source, "path");
    int complete = error != NULL && cJSON_AddStringToObject(error, "detail", text_of(source, detail_member)) != NULL &&
                   cJSON_AddStringToObject(error, "fix", text_of(source, "fix")) != NULL &&
                   (path == NULL || cJSON_AddStringToObject(error, "path", path) != NULL) &&
                   cJSON_AddStringToObject(rendered, "state", "error") != NULL && cJSON_AddItemToObject(rendered, "error", error);
    if (!complete) {
        cJSON_Delete(error);
    }
    return complete;
}

static const cJSON *checked_for(const cJSON *answer, const cJSON *check) {
    const cJSON *checked;
    cJSON_ArrayForEach(checked, cJSON_GetObjectItemCaseSensitive(answer, "checked")) {
        if (runtime_ui_is_same_check(checked, check) && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(checked, "issues"))) {
            return checked;
        }
    }
    return NULL;
}

static int is_issue(const cJSON *issue) {
    return text_of(issue, "message") != NULL && text_of(issue, "fix") != NULL;
}

static cJSON *unchecked(const cJSON *node, const cJSON *call, const cJSON *check) {
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(call, "binding");
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(check, "members");
    return runtime_ui_refusal("check every answer against each check ui.plan named for its call and pass each result under "
                              "checked as {\"against\", \"members\"?, \"issues\": [{\"message\", \"fix\", \"path\"?}]}",
                              "the answer to ", text_of(binding, "service"), ".", text_of(binding, "method"), " that node ",
                              text_of(node, "id"), " binds carries no check against ", text_of(check, "against"),
                              members == NULL ? "" : " over the members its node reads", NULL);
}

/** Puts the checked answer into rendered, ready with its data or in error with its first issue. */
static int add_checked_answer(cJSON *rendered, const cJSON *node, const cJSON *call, const cJSON *answer, cJSON **refusal) {
    cJSON *check = runtime_ui_check_of(node);
    const cJSON *checked = check == NULL ? NULL : checked_for(answer, check);
    if (check != NULL && checked == NULL) {
        *refusal = unchecked(node, call, check);
    }
    cJSON_Delete(check);
    if (checked == NULL) {
        return 0;
    }
    const cJSON *issue = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(checked, "issues"), 0);
    if (issue != NULL) {
        *refusal = is_issue(issue) ? NULL : runtime_refusal("a check's issue carries no message or fix",
                                                           "pass each issue the engine answered as {\"message\", \"fix\", \"path\"?}");
        return is_issue(issue) && add_error(rendered, issue, "message");
    }
    return cJSON_AddStringToObject(rendered, "state", "ready") != NULL &&
           cJSON_AddItemToObject(rendered, "data", data_of(node, cJSON_GetObjectItemCaseSensitive(answer, "answer")));
}

static cJSON *unanswered(const cJSON *node, const cJSON *call) {
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(call, "binding");
    return runtime_ui_refusal("call ui.plan until it answers done and pass every answer it asked for", "the answers carry none to ",
                              text_of(call, "plugin"), "'s ", text_of(binding, "service"), ".", text_of(binding, "method"),
                              " that node ", text_of(node, "id"), " binds", NULL);
}

static int add_outcome(cJSON *rendered, const char *plugin, const cJSON *node, const cJSON *answers, cJSON **refusal) {
    if (!runtime_ui_is_bound(node)) {
        return cJSON_AddStringToObject(rendered, "state", "ready") != NULL;
    }
    cJSON *call = runtime_ui_call_of(plugin, node);
    const cJSON *answer = call == NULL ? NULL : runtime_ui_answer_to(answers, call);
    int complete = 0;
    if (call != NULL && answer == NULL) {
        *refusal = unanswered(node, call);
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "unavailable"))) {
        complete = cJSON_AddStringToObject(rendered, "state", "unavailable") != NULL;
    } else if (cJSON_GetObjectItemCaseSensitive(answer, "error") != NULL) {
        complete = add_error(rendered, cJSON_GetObjectItemCaseSensitive(answer, "error"), "detail");
    } else if (answer != NULL) {
        complete = add_checked_answer(rendered, node, call, answer, refusal);
    }
    cJSON_Delete(call);
    return complete;
}

/** @implNote Only the known presentation members are carried, so an unknown one a description holds never reaches a renderer. */
static cJSON *props_of(const cJSON *node) {
    cJSON *props = cJSON_CreateObject();
    for (size_t index = 0; props != NULL && index < COUNT_OF(PROP_MEMBERS); index++) {
        const cJSON *member = cJSON_GetObjectItemCaseSensitive(node, PROP_MEMBERS[index]);
        if (member != NULL && !cJSON_AddItemToObject(props, PROP_MEMBERS[index], cJSON_Duplicate(member, 1))) {
            cJSON_Delete(props);
            props = NULL;
        }
    }
    return props;
}

static cJSON *rendered_node(const char *plugin, const cJSON *node, const cJSON *answers, cJSON **refusal) {
    const char *parent = text_of(node, "parent");
    cJSON *rendered = cJSON_CreateObject();
    int complete = rendered != NULL &&
                   cJSON_AddItemToObject(rendered, "id", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(node, "id"), 1)) &&
                   cJSON_AddItemToObject(rendered, "component", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(node, "component"), 1)) &&
                   (parent == NULL || cJSON_AddStringToObject(rendered, "parent", parent) != NULL) &&
                   add_outcome(rendered, plugin, node, answers, refusal) && cJSON_AddItemToObject(rendered, "props", props_of(node));
    if (!complete) {
        cJSON_Delete(rendered);
        return NULL;
    }
    return rendered;
}

static cJSON *screen_head(const ui_screen *screen) {
    cJSON *head = cJSON_CreateObject();
    const char *title = text_of(screen->view, "title");
    int complete = head != NULL && cJSON_AddStringToObject(head, "plugin", screen->plugin) != NULL &&
                   cJSON_AddStringToObject(head, "kind", screen->kind) != NULL &&
                   (screen->view_id == NULL || cJSON_AddStringToObject(head, "view", screen->view_id) != NULL) &&
                   (title == NULL || cJSON_AddStringToObject(head, "title", title) != NULL);
    if (!complete) {
        cJSON_Delete(head);
        return NULL;
    }
    return head;
}

static cJSON *rendered_screen(const ui_screen *screen, const cJSON *answers, cJSON **refusal) {
    cJSON *rendered = screen_head(screen);
    cJSON *nodes = rendered == NULL ? NULL : cJSON_AddArrayToObject(rendered, "nodes");
    int complete = nodes != NULL;
    const cJSON *source = complete ? nodes_of(screen) : NULL;
    const cJSON *node;
    cJSON_ArrayForEach(node, source) {
        cJSON *entry = complete ? rendered_node(screen->plugin, node, answers, refusal) : NULL;
        complete = entry != NULL && cJSON_AddItemToArray(nodes, entry);
    }
    if (!complete) {
        cJSON_Delete(rendered);
        return NULL;
    }
    return rendered;
}

static cJSON *render_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *answers = cJSON_GetObjectItemCaseSensitive(argument, "answers");
    if (!runtime_ui_is_answer_list(answers)) {
        return refuse_answers(refusal);
    }
    ui_screen screen;
    if (!runtime_ui_open_screen(argument, &screen, refusal)) {
        runtime_ui_close_screen(&screen);
        return NULL;
    }
    cJSON *rendered = rendered_screen(&screen, answers, refusal);
    runtime_ui_close_screen(&screen);
    return rendered;
}

RUNTIME_EXPORT int32_t runtime_ui_render(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, render_for);
}
