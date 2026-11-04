#include "ui_binding.h"

#include <string.h>

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

int runtime_ui_is_bound(const cJSON *node) {
    const cJSON *bind = cJSON_GetObjectItemCaseSensitive(node, "bind");
    return text_of(bind, "service") != NULL && text_of(bind, "method") != NULL;
}

static cJSON *arguments_of(const cJSON *bind) {
    const cJSON *argument = cJSON_GetObjectItemCaseSensitive(bind, "argument");
    return cJSON_IsObject(argument) ? cJSON_Duplicate(argument, 1) : cJSON_CreateObject();
}

cJSON *runtime_ui_call_of(const char *plugin, const cJSON *node) {
    const cJSON *bind = cJSON_GetObjectItemCaseSensitive(node, "bind");
    cJSON *call = cJSON_CreateObject();
    cJSON *binding = call == NULL || cJSON_AddStringToObject(call, "plugin", plugin) == NULL
                         ? NULL
                         : cJSON_AddObjectToObject(call, "binding");
    int complete = binding != NULL && cJSON_AddStringToObject(binding, "service", text_of(bind, "service")) != NULL &&
                   cJSON_AddStringToObject(binding, "method", text_of(bind, "method")) != NULL &&
                   cJSON_AddItemToObject(call, "arguments", arguments_of(bind));
    if (!complete) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static int is_meter(const cJSON *node) {
    const char *chart = text_of(node, "chart");
    return chart != NULL && strcmp(chart, "meter") == 0;
}

static int is_chart(const cJSON *node) {
    const char *component = text_of(node, "component");
    return component != NULL && strcmp(component, "chart") == 0;
}

static const char *against_of(const cJSON *node) {
    if (!is_chart(node)) {
        return text_of(node, "component");
    }
    return is_meter(node) ? "progress" : "chart";
}

cJSON *runtime_ui_check_of(const cJSON *node) {
    const char *against = against_of(node);
    cJSON *check = cJSON_CreateObject();
    int complete = check != NULL && cJSON_AddStringToObject(check, "against", against == NULL ? "" : against) != NULL;
    if (complete && is_chart(node) && !is_meter(node)) {
        const char *members[] = {"series"};
        complete = cJSON_AddItemToObject(check, "members", cJSON_CreateStringArray(members, 1));
    }
    if (!complete) {
        cJSON_Delete(check);
        return NULL;
    }
    return check;
}

static int is_same_text(const cJSON *left, const cJSON *right, const char *member) {
    const char *left_text = text_of(left, member);
    const char *right_text = text_of(right, member);
    return left_text != NULL && right_text != NULL && strcmp(left_text, right_text) == 0;
}

int runtime_ui_is_same_call(const cJSON *left, const cJSON *right) {
    const cJSON *left_binding = cJSON_GetObjectItemCaseSensitive(left, "binding");
    const cJSON *right_binding = cJSON_GetObjectItemCaseSensitive(right, "binding");
    return is_same_text(left, right, "plugin") && is_same_text(left_binding, right_binding, "service") &&
           is_same_text(left_binding, right_binding, "method") &&
           cJSON_Compare(cJSON_GetObjectItemCaseSensitive(left, "arguments"), cJSON_GetObjectItemCaseSensitive(right, "arguments"), 1);
}

int runtime_ui_is_same_check(const cJSON *left, const cJSON *right) {
    const cJSON *left_members = cJSON_GetObjectItemCaseSensitive(left, "members");
    const cJSON *right_members = cJSON_GetObjectItemCaseSensitive(right, "members");
    int same_members = (left_members == NULL && right_members == NULL) ||
                       (left_members != NULL && right_members != NULL && cJSON_Compare(left_members, right_members, 1));
    return is_same_text(left, right, "against") && same_members;
}

static int is_call(const cJSON *call) {
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(call, "binding");
    return cJSON_IsObject(call) && text_of(call, "plugin") != NULL && text_of(binding, "service") != NULL &&
           text_of(binding, "method") != NULL && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(call, "arguments"));
}

static int is_error(const cJSON *error) {
    return text_of(error, "detail") != NULL && text_of(error, "fix") != NULL;
}

static int outcome_count(const cJSON *answer) {
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(answer, "error");
    const cJSON *unavailable = cJSON_GetObjectItemCaseSensitive(answer, "unavailable");
    int answered = cJSON_GetObjectItemCaseSensitive(answer, "answer") != NULL;
    if ((error != NULL && !is_error(error)) || (unavailable != NULL && !cJSON_IsTrue(unavailable))) {
        return -1;
    }
    return answered + (error != NULL) + (unavailable != NULL);
}

int runtime_ui_is_answer_list(const cJSON *answers) {
    if (!cJSON_IsArray(answers)) {
        return 0;
    }
    const cJSON *answer;
    cJSON_ArrayForEach(answer, answers) {
        if (!is_call(answer) || outcome_count(answer) != 1) {
            return 0;
        }
    }
    return 1;
}

const cJSON *runtime_ui_answer_to(const cJSON *answers, const cJSON *call) {
    const cJSON *answer;
    cJSON_ArrayForEach(answer, answers) {
        if (runtime_ui_is_same_call(answer, call)) {
            return answer;
        }
    }
    return NULL;
}
