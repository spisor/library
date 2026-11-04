#include "ui_screen.h"

#include <string.h>

#include "json_service.h"
#include "ui_refusal.h"
#include "ui_settings_view.h"

int runtime_ui_is_description(const cJSON *description) {
    return cJSON_IsObject(description) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(description, "id"));
}

int runtime_ui_is_description_list(const cJSON *descriptions) {
    if (!cJSON_IsArray(descriptions)) {
        return 0;
    }
    const cJSON *description;
    cJSON_ArrayForEach(description, descriptions) {
        if (!runtime_ui_is_description(description)) {
            return 0;
        }
    }
    return 1;
}

static const cJSON *description_of(const cJSON *descriptions, const char *plugin) {
    const cJSON *description;
    cJSON_ArrayForEach(description, descriptions) {
        if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(description, "id")), plugin) == 0) {
            return description;
        }
    }
    return NULL;
}

static int refuse(cJSON **refusal, cJSON *reason) {
    *refusal = reason;
    return 0;
}

static int is_node(const cJSON *node) {
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(node, "id")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(node, "component"));
}

/** Whether view is an object whose nodes are an array of nodes each with an id and a component. */
static int is_view(const cJSON *view) {
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(view, "nodes");
    if (!cJSON_IsObject(view) || !cJSON_IsArray(nodes)) {
        return 0;
    }
    const cJSON *node;
    cJSON_ArrayForEach(node, nodes) {
        if (!is_node(node)) {
            return 0;
        }
    }
    return 1;
}

static int open_view(const cJSON *description, const cJSON *entry, ui_screen *screen, cJSON **refusal) {
    screen->view_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id"));
    if (screen->view_id == NULL) {
        return refuse(refusal, runtime_refusal("the screen names no view to open",
                                               "send the navigation entry {\"plugin\", \"kind\": \"view\", \"id\": \"<view id>\"}"));
    }
    screen->view = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(description, "views"), screen->view_id);
    if (!is_view(screen->view)) {
        return refuse(refusal, runtime_ui_refusal("open a view the plugin declares under views, each of its nodes with an id and a component",
                                                  screen->plugin, " declares no view ", screen->view_id,
                                                  " whose nodes each carry an id and a component", NULL));
    }
    return 1;
}

static int open_settings(const cJSON *description, ui_screen *screen, cJSON **refusal) {
    screen->generated = runtime_ui_settings_view_of(description);
    screen->view = screen->generated;
    *refusal = NULL;
    return screen->generated != NULL;
}

static int open_entry(const cJSON *description, const cJSON *entry, ui_screen *screen, cJSON **refusal) {
    if (strcmp(screen->kind, "view") == 0) {
        return open_view(description, entry, screen, refusal);
    }
    if (strcmp(screen->kind, "settings") == 0) {
        return open_settings(description, screen, refusal);
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id"));
    if (strcmp(screen->kind, "action") == 0) {
        return refuse(refusal, runtime_ui_refusal("run the action through the layer's engine; only a view or settings entry opens a screen",
                                                  screen->plugin, " action ", id == NULL ? "" : id, " is run, not rendered", NULL));
    }
    return refuse(refusal, runtime_ui_refusal("send a navigation entry whose kind is view or settings", "the screen's kind ",
                                              screen->kind, " is none of view or settings", NULL));
}

static int refuse_argument(cJSON **refusal) {
    return refuse(refusal, runtime_refusal("the argument carries no usable descriptions or screen",
                                           "send {\"descriptions\": [<plugin manifest>, ...], \"screen\": {\"plugin\": "
                                           "\"<plugin id>\", \"kind\": \"view\" or \"settings\", \"id\": \"<view id>\"}}, the "
                                           "screen being the navigation entry ui.navigation answered"));
}

int runtime_ui_open_screen(const cJSON *argument, ui_screen *screen, cJSON **refusal) {
    memset(screen, 0, sizeof *screen);
    const cJSON *descriptions = cJSON_GetObjectItemCaseSensitive(argument, "descriptions");
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(argument, "screen");
    screen->plugin = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "plugin"));
    screen->kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
    if (!runtime_ui_is_description_list(descriptions) || screen->plugin == NULL || screen->kind == NULL) {
        return refuse_argument(refusal);
    }
    const cJSON *description = description_of(descriptions, screen->plugin);
    if (description == NULL) {
        return refuse(refusal, runtime_ui_refusal("pass the plugin's manifest among descriptions, or open a screen of a plugin they describe",
                                                  screen->plugin, " is described by none of the descriptions", NULL));
    }
    return open_entry(description, entry, screen, refusal);
}

void runtime_ui_close_screen(ui_screen *screen) {
    cJSON_Delete(screen->generated);
    screen->generated = NULL;
}
