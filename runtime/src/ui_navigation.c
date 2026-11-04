#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "json_service.h"
#include "ui.h"
#include "ui_screen.h"
#include "ui_settings_view.h"

/** @implNote An entry is routed by its kind, so this id never names a view and a declared view called settings does not collide with it. */
#define SETTINGS_ENTRY_ID "settings"

typedef struct customisation {
    int hidden;
    double order;
    const cJSON *pinned;
    const char *default_view;
} customisation;

typedef struct customised_plugin {
    const cJSON *description;
    customisation own;
    size_t position;
} customised_plugin;

static const char *id_of(const cJSON *description) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(description, "id"));
}

static int is_integer(const cJSON *value) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) && floor(value->valuedouble) == value->valuedouble;
}

/** @implNote A value of the wrong type is ignored like an unknown key, so one bad edit to settings.json never hides a plugin. */
static customisation customisation_of(const cJSON *ui, const char *plugin) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(ui, "plugins");
    const cJSON *own = cJSON_IsObject(plugins) ? cJSON_GetObjectItemCaseSensitive(plugins, plugin) : NULL;
    own = cJSON_IsObject(own) ? own : NULL;
    const cJSON *order = cJSON_GetObjectItemCaseSensitive(own, "order");
    const cJSON *pinned = cJSON_GetObjectItemCaseSensitive(own, "pinned");
    customisation result = {cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(own, "hidden")),
                            is_integer(order) ? order->valuedouble : INFINITY, cJSON_IsArray(pinned) ? pinned : NULL,
                            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(own, "defaultView"))};
    return result;
}

static int by_order_then_id(const void *left_pointer, const void *right_pointer) {
    const customised_plugin *left = left_pointer;
    const customised_plugin *right = right_pointer;
    if (left->own.order != right->own.order) {
        return left->own.order < right->own.order ? -1 : 1;
    }
    int by_id = strcmp(id_of(left->description), id_of(right->description));
    if (by_id != 0) {
        return by_id;
    }
    return left->position < right->position ? -1 : left->position > right->position;
}

/** The plugins not hidden, in navigation order; NULL with *count 0 when none, NULL with *count nonzero when memory runs out. */
static customised_plugin *visible_plugins(const cJSON *descriptions, const cJSON *ui, size_t *count) {
    size_t size = (size_t)cJSON_GetArraySize(descriptions);
    customised_plugin *plugins = size == 0 ? NULL : malloc(size * sizeof *plugins);
    *count = size == 0 || plugins != NULL ? 0 : 1;
    size_t position = 0;
    const cJSON *source = plugins == NULL ? NULL : descriptions;
    const cJSON *description;
    cJSON_ArrayForEach(description, source) {
        customisation own = customisation_of(ui, id_of(description));
        if (!own.hidden) {
            customised_plugin plugin = {description, own, position};
            plugins[(*count)++] = plugin;
        }
        position++;
    }
    if (plugins != NULL) {
        qsort(plugins, *count, sizeof *plugins, by_order_then_id);
    }
    return plugins;
}

static int by_member_name(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/** The member names of object sorted, as borrowed pointers; NULL with *count 0 when it has none. */
static const char **sorted_names(const cJSON *object, size_t *count, int *complete) {
    *count = cJSON_IsObject(object) ? (size_t)cJSON_GetArraySize(object) : 0;
    const char **names = *count == 0 ? NULL : malloc(*count * sizeof *names);
    *complete = *count == 0 || names != NULL;
    size_t index = 0;
    const cJSON *source = names == NULL ? NULL : object;
    const cJSON *member;
    cJSON_ArrayForEach(member, source) {
        names[index++] = member->string;
    }
    if (names != NULL) {
        qsort(names, *count, sizeof *names, by_member_name);
    }
    return names;
}

static int is_pinned(const customisation *own, const char *view) {
    const cJSON *pinned;
    cJSON_ArrayForEach(pinned, own->pinned) {
        if (cJSON_IsString(pinned) && strcmp(pinned->valuestring, view) == 0) {
            return 1;
        }
    }
    return 0;
}

static int add_entry(cJSON *entries, const char *plugin, const char *kind, const char *id, const char *title, int pinned) {
    cJSON *entry = cJSON_CreateObject();
    int complete = entry != NULL && cJSON_AddStringToObject(entry, "plugin", plugin) != NULL &&
                   cJSON_AddStringToObject(entry, "kind", kind) != NULL && cJSON_AddStringToObject(entry, "id", id) != NULL &&
                   (title == NULL || cJSON_AddStringToObject(entry, "title", title) != NULL) &&
                   cJSON_AddBoolToObject(entry, "pinned", pinned) != NULL && cJSON_AddItemToArray(entries, entry);
    if (!complete) {
        cJSON_Delete(entry);
    }
    return complete;
}

static const char *title_of(const cJSON *declarations, const char *id, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(declarations, id), member));
}

static int is_listed(const cJSON *entries, const char *plugin, const char *view) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, entries) {
        if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "plugin")), plugin) == 0 &&
            strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind")), "view") == 0 &&
            strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "id")), view) == 0) {
            return 1;
        }
    }
    return 0;
}

static int add_view(cJSON *entries, const customised_plugin *plugin, const char *view) {
    const cJSON *views = cJSON_GetObjectItemCaseSensitive(plugin->description, "views");
    const char *id = id_of(plugin->description);
    if (view == NULL || !cJSON_IsObject(views) || cJSON_GetObjectItemCaseSensitive(views, view) == NULL ||
        is_listed(entries, id, view)) {
        return 1;
    }
    return add_entry(entries, id, "view", view, title_of(views, view, "title"), is_pinned(&plugin->own, view));
}

/** The default view first, then pinned views, then every other view by key; an undeclared one is skipped. */
static int add_views(cJSON *entries, const customised_plugin *plugin) {
    int complete = add_view(entries, plugin, plugin->own.default_view);
    const cJSON *pinned;
    cJSON_ArrayForEach(pinned, plugin->own.pinned) {
        complete = complete && add_view(entries, plugin, cJSON_GetStringValue(pinned));
    }
    size_t count = 0;
    const char **names = complete ? sorted_names(cJSON_GetObjectItemCaseSensitive(plugin->description, "views"), &count, &complete) : NULL;
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_view(entries, plugin, names[index]);
    }
    free(names);
    return complete;
}

static int add_settings(cJSON *entries, const customised_plugin *plugin) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(plugin->description, "settings");
    if (!cJSON_IsObject(settings) || settings->child == NULL) {
        return 1;
    }
    return add_entry(entries, id_of(plugin->description), "settings", SETTINGS_ENTRY_ID, RUNTIME_UI_SETTINGS_TITLE, 0);
}

static int add_actions(cJSON *entries, const customised_plugin *plugin) {
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(plugin->description, "actions");
    size_t count = 0;
    int complete = 0;
    const char **names = sorted_names(actions, &count, &complete);
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_entry(entries, id_of(plugin->description), "action", names[index], title_of(actions, names[index], "label"), 0);
    }
    free(names);
    return complete;
}

static cJSON *entries_of(const cJSON *descriptions, const cJSON *ui) {
    size_t count = 0;
    customised_plugin *plugins = visible_plugins(descriptions, ui, &count);
    cJSON *entries = plugins == NULL && count != 0 ? NULL : cJSON_CreateArray();
    int complete = entries != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_views(entries, &plugins[index]) && add_settings(entries, &plugins[index]) &&
                   add_actions(entries, &plugins[index]);
    }
    free(plugins);
    if (!complete) {
        cJSON_Delete(entries);
        return NULL;
    }
    return entries;
}

static cJSON *preferences_of(const cJSON *ui) {
    cJSON *preferences = cJSON_Duplicate(ui, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(preferences, "plugins");
    return preferences;
}

static cJSON *navigation_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *descriptions = cJSON_GetObjectItemCaseSensitive(argument, "descriptions");
    const cJSON *ui = cJSON_GetObjectItemCaseSensitive(argument, "ui");
    if (!runtime_ui_is_description_list(descriptions) || !cJSON_IsObject(ui)) {
        *refusal = runtime_refusal("the argument carries no usable descriptions or ui",
                                   "send {\"descriptions\": [<plugin manifest>, ...], \"ui\": {...}}, every manifest with "
                                   "its id and ui the user's ui settings");
        return NULL;
    }
    cJSON *navigation = cJSON_CreateObject();
    if (navigation == NULL || !cJSON_AddItemToObject(navigation, "entries", entries_of(descriptions, ui)) ||
        !cJSON_AddItemToObject(navigation, "preferences", preferences_of(ui))) {
        cJSON_Delete(navigation);
        return NULL;
    }
    return navigation;
}

RUNTIME_EXPORT int32_t runtime_ui_navigation(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, navigation_for);
}
