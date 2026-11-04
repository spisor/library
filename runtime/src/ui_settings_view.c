#include "ui_settings_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_service.h"
#include "ui.h"
#include "ui_screen.h"

#define FORM_NODE_ID "settingsForm"

static int by_key(const void *left, const void *right) {
    return strcmp((*(const cJSON *const *)left)->string, (*(const cJSON *const *)right)->string);
}

/**
 * @implNote Field ids are positional because a setting key may hold characters a node id refuses, such
 * as a dot.
 */
static cJSON *field_node(const cJSON *setting, size_t index) {
    char id[48];
    snprintf(id, sizeof id, "settingsField%zu", index);
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(setting, "label"));
    cJSON *node = cJSON_CreateObject();
    int complete = node != NULL && cJSON_AddStringToObject(node, "id", id) != NULL &&
                   cJSON_AddStringToObject(node, "parent", FORM_NODE_ID) != NULL &&
                   cJSON_AddStringToObject(node, "component", "field") != NULL &&
                   cJSON_AddStringToObject(node, "field", setting->string) != NULL &&
                   (label == NULL || cJSON_AddStringToObject(node, "label", label) != NULL);
    if (!complete) {
        cJSON_Delete(node);
        return NULL;
    }
    return node;
}

static cJSON *form_node(void) {
    cJSON *node = cJSON_CreateObject();
    if (node != NULL &&
        (cJSON_AddStringToObject(node, "id", FORM_NODE_ID) == NULL || cJSON_AddStringToObject(node, "component", "form") == NULL)) {
        cJSON_Delete(node);
        return NULL;
    }
    return node;
}

/** The settings of description sorted by key, as borrowed pointers; NULL with *count 0 when there are none. */
static const cJSON **sorted_settings(const cJSON *description, size_t *count, int *complete) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(description, "settings");
    *count = cJSON_IsObject(settings) ? (size_t)cJSON_GetArraySize(settings) : 0;
    const cJSON **sorted = *count == 0 ? NULL : malloc(*count * sizeof *sorted);
    *complete = *count == 0 || sorted != NULL;
    size_t index = 0;
    const cJSON *source = sorted == NULL ? NULL : settings;
    const cJSON *setting;
    cJSON_ArrayForEach(setting, source) {
        sorted[index++] = setting;
    }
    if (sorted != NULL) {
        qsort(sorted, *count, sizeof *sorted, by_key);
    }
    return sorted;
}

static int add_nodes(cJSON *nodes, const cJSON *description) {
    size_t count = 0;
    int complete = 0;
    const cJSON **settings = sorted_settings(description, &count, &complete);
    complete = complete && cJSON_AddItemToArray(nodes, form_node());
    for (size_t index = 0; complete && index < count; index++) {
        cJSON *field = field_node(settings[index], index);
        complete = field != NULL && cJSON_AddItemToArray(nodes, field);
    }
    free(settings);
    return complete;
}

cJSON *runtime_ui_settings_view_of(const cJSON *description) {
    cJSON *view = cJSON_CreateObject();
    cJSON *nodes = view == NULL || cJSON_AddStringToObject(view, "title", RUNTIME_UI_SETTINGS_TITLE) == NULL
                       ? NULL
                       : cJSON_AddArrayToObject(view, "nodes");
    if (nodes == NULL || !add_nodes(nodes, description)) {
        cJSON_Delete(view);
        return NULL;
    }
    return view;
}

static cJSON *settings_view_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *description = cJSON_GetObjectItemCaseSensitive(argument, "description");
    if (!runtime_ui_is_description(description)) {
        *refusal = runtime_refusal("the argument carries no usable description",
                                   "send {\"description\": {\"id\": \"<plugin id>\", \"settings\": {...}}}, the plugin's manifest");
        return NULL;
    }
    return runtime_ui_settings_view_of(description);
}

RUNTIME_EXPORT int32_t runtime_ui_settings_view(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, settings_view_for);
}
