#include "ui_items.h"

#include <string.h>

#include "json_service.h"
#include "lists.h"
#include "ui.h"

static int is_unusable(const cJSON *row) {
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "state"));
    return state != NULL && (strcmp(state, "detected") == 0 || strcmp(state, "available") == 0);
}

static cJSON *presented_row(const cJSON *row) {
    cJSON *copy = cJSON_Duplicate(row, 1);
    if (copy == NULL || !is_unusable(row)) {
        return copy;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "action");
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "unavailable");
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "install");
    if (cJSON_AddTrueToObject(copy, "unavailable") == NULL || cJSON_AddTrueToObject(copy, "install") == NULL) {
        cJSON_Delete(copy);
        return NULL;
    }
    return copy;
}

cJSON *runtime_ui_presented_items(const cJSON *items) {
    cJSON *presented = cJSON_CreateArray();
    const cJSON *source = presented == NULL ? NULL : items;
    const cJSON *row;
    cJSON_ArrayForEach(row, source) {
        cJSON *copy = presented_row(row);
        if (copy == NULL || !cJSON_AddItemToArray(presented, copy)) {
            cJSON_Delete(copy);
            cJSON_Delete(presented);
            return NULL;
        }
    }
    return presented;
}

static cJSON *table_node(cJSON *items) {
    cJSON *node = cJSON_CreateObject();
    cJSON *data = node == NULL || cJSON_AddStringToObject(node, "id", "plugins") == NULL ||
                          cJSON_AddStringToObject(node, "component", "table") == NULL ||
                          cJSON_AddStringToObject(node, "state", "ready") == NULL
                      ? NULL
                      : cJSON_AddObjectToObject(node, "data");
    if (data == NULL || !cJSON_AddItemToObject(data, "items", items)) {
        cJSON_Delete(items);
        cJSON_Delete(node);
        return NULL;
    }
    if (cJSON_AddObjectToObject(node, "props") == NULL) {
        cJSON_Delete(node);
        return NULL;
    }
    return node;
}

static cJSON *host_screen(cJSON *node) {
    cJSON *screen = node == NULL ? NULL : cJSON_CreateObject();
    cJSON *nodes = screen == NULL || cJSON_AddStringToObject(screen, "kind", "host") == NULL ||
                           cJSON_AddStringToObject(screen, "title", "Plugins") == NULL
                       ? NULL
                       : cJSON_AddArrayToObject(screen, "nodes");
    if (nodes == NULL || !cJSON_AddItemToArray(nodes, node)) {
        cJSON_Delete(node);
        cJSON_Delete(screen);
        return NULL;
    }
    return screen;
}

static cJSON *plugin_list_for(const cJSON *argument, cJSON **refusal) {
    cJSON *listed = runtime_call_or_refuse(runtime_lists_plugins, cJSON_Duplicate(argument, 1), refusal);
    cJSON *items = listed == NULL ? NULL : runtime_ui_presented_items(cJSON_GetObjectItemCaseSensitive(listed, "items"));
    cJSON_Delete(listed);
    return items == NULL ? NULL : host_screen(table_node(items));
}

RUNTIME_EXPORT int32_t runtime_ui_plugin_list(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, plugin_list_for);
}
