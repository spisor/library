#include "ui_settings_view.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_conversion.h"
#include "json_service.h"
#include "merge.h"
#include "ui.h"
#include "ui_screen.h"

#define FORM_NODE_ID "settingsForm"
#define TEXT_KIND "text"

static int by_key(const void *left, const void *right) {
    return strcmp((*(const cJSON *const *)left)->string, (*(const cJSON *const *)right)->string);
}

static const char *type_of(const cJSON *setting) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(setting, "type"));
    return type == NULL ? TEXT_KIND : type;
}

/** Adds a copy of what `from` holds as `member`, or nothing when it is NULL; 0 when memory runs out. */
static int add_copy(cJSON *node, const char *member, const cJSON *from) {
    return from == NULL || runtime_add_owned(node, member, cJSON_Duplicate(from, 1));
}

/**
 * @implNote Field ids are positional because a setting key may hold characters a node id refuses, such
 * as a dot.
 */
static cJSON *field_node(const cJSON *setting, const cJSON *defaults, size_t index) {
    char id[48];
    snprintf(id, sizeof id, "settingsField%zu", index);
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(setting, "label"));
    cJSON *node = cJSON_CreateObject();
    int complete = node != NULL && cJSON_AddStringToObject(node, "id", id) != NULL &&
                   cJSON_AddStringToObject(node, "parent", FORM_NODE_ID) != NULL &&
                   cJSON_AddStringToObject(node, "component", "field") != NULL &&
                   cJSON_AddStringToObject(node, "field", setting->string) != NULL &&
                   (label == NULL || cJSON_AddStringToObject(node, "label", label) != NULL) &&
                   cJSON_AddStringToObject(node, "type", type_of(setting)) != NULL &&
                   add_copy(node, "choices", cJSON_GetObjectItemCaseSensitive(setting, "choices")) &&
                   add_copy(node, "default", runtime_walk(defaults, setting->string));
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
    const cJSON **sorted = *count == 0 ? NULL : spisor_memory_allocate(*count * sizeof *sorted);
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
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(description, "config");
    const cJSON *defaults = cJSON_GetObjectItemCaseSensitive(config, "defaults");
    complete = complete && cJSON_AddItemToArray(nodes, form_node());
    for (size_t index = 0; complete && index < count; index++) {
        cJSON *field = field_node(settings[index], cJSON_IsObject(defaults) ? defaults : NULL, index);
        complete = field != NULL && cJSON_AddItemToArray(nodes, field);
    }
    spisor_memory_release(settings);
    return complete;
}

static int is_readable_declaration(const cJSON *declaration) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(declaration, "type");
    return cJSON_IsObject(declaration) &&
           (type == NULL || (cJSON_IsString(type) && runtime_field_kind_named(type->valuestring) != 0)) &&
           runtime_are_setting_choices(cJSON_GetObjectItemCaseSensitive(declaration, "choices"));
}

int runtime_ui_are_settings_readable(const cJSON *description) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(description, "settings");
    const cJSON *declaration;
    if (settings == NULL) {
        return 1;
    }
    if (!cJSON_IsObject(settings)) {
        return 0;
    }
    cJSON_ArrayForEach(declaration, settings) {
        if (!is_readable_declaration(declaration)) {
            return 0;
        }
    }
    return 1;
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

cJSON *runtime_ui_settings_view(const cJSON *description, cJSON **refusal) {
    *refusal = NULL;
    if (!runtime_ui_is_description(description)) {
        *refusal = runtime_refusal("the call carries no usable description",
                                   "pass the plugin's manifest as description, a record with its id as text");
        return NULL;
    }
    return runtime_ui_settings_view_of(description);
}
