#include "ui.h"

#include "json_service.h"
#include "ui_refusal.h"

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** The declared action's call, NULL when the manifest declares no action of that id as an object. */
static const cJSON *declared_action(const cJSON *description, const char *action) {
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(description, "actions");
    const cJSON *declared = cJSON_IsObject(actions) ? cJSON_GetObjectItemCaseSensitive(actions, action) : NULL;
    return cJSON_IsObject(declared) ? declared : NULL;
}

/** Lays every member of `over` on `arguments`, a member of the same name replaced where it stands; 0 out of memory. */
static int lay_over(cJSON *arguments, const cJSON *over) {
    const cJSON *member;
    cJSON_ArrayForEach(member, over) {
        cJSON *copy = cJSON_Duplicate(member, 1);
        if (cJSON_GetObjectItemCaseSensitive(arguments, member->string) == NULL) {
            if (!runtime_add_owned(arguments, member->string, copy)) {
                return 0;
            }
        } else if (copy == NULL || !cJSON_ReplaceItemInObjectCaseSensitive(arguments, member->string, copy)) {
            cJSON_Delete(copy);
            return 0;
        }
    }
    return 1;
}

static cJSON *merged_arguments(const cJSON *call, const cJSON *input, const cJSON *row) {
    const cJSON *declared = cJSON_GetObjectItemCaseSensitive(call, "argument");
    cJSON *arguments = cJSON_IsObject(declared) ? cJSON_Duplicate(declared, 1) : cJSON_CreateObject();
    if (arguments != NULL && !(lay_over(arguments, input) && lay_over(arguments, row))) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

static cJSON *call_json(const char *plugin, const cJSON *call, cJSON *arguments) {
    cJSON *json = cJSON_CreateObject();
    cJSON *binding = json == NULL || cJSON_AddStringToObject(json, "plugin", plugin) == NULL
                         ? NULL
                         : cJSON_AddObjectToObject(json, "binding");
    int built = binding != NULL && cJSON_AddStringToObject(binding, "service", text_of(call, "service")) != NULL &&
                cJSON_AddStringToObject(binding, "method", text_of(call, "method")) != NULL;
    if (!built || !runtime_add_owned(json, "arguments", arguments)) {
        if (!built) {
            cJSON_Delete(arguments);
        }
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

cJSON *runtime_ui_action(const cJSON *description, const char *action, const cJSON *input, const cJSON *row,
                         cJSON **refusal) {
    const char *plugin = text_of(description, "id");
    const cJSON *declared = declared_action(description, action);
    const cJSON *call = cJSON_GetObjectItemCaseSensitive(declared, "call");
    cJSON *arguments;
    *refusal = NULL;
    if (declared == NULL) {
        *refusal = runtime_ui_refusal("run an action the plugin lists under actions", plugin, " declares no action ",
                                      action, NULL);
        return NULL;
    }
    if (text_of(call, "service") == NULL || text_of(call, "method") == NULL) {
        *refusal = runtime_ui_refusal("declare the action's call with a service and a method, each text", plugin,
                                      " declares action ", action, " with no call naming a service and a method", NULL);
        return NULL;
    }
    arguments = merged_arguments(call, input, row);
    return arguments == NULL ? NULL : call_json(plugin, call, arguments);
}
