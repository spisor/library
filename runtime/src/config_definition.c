#include "runtime/config.h"

#include <string.h>

#include "config.h"
#include "config_conversion.h"
#include "definition_support.h"

#define PLUGIN_SHAPE RUNTIME_SHAPE_THROUGH(runtime_config_plugin, places)
#define SET_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_config_set_answer, storage)

/** The JSON a request points at while it is served, deleted by free_plugin_json. */
typedef struct plugin_json {
    cJSON *names;
    cJSON *config;
    cJSON *settings;
    cJSON *value;
    runtime_secret_place place;
} plugin_json;

static void free_plugin_json(plugin_json *json) {
    cJSON_Delete(json->names);
    cJSON_Delete(json->config);
    cJSON_Delete(json->settings);
    cJSON_Delete(json->value);
}

static spisor_status read_places(const runtime_config_plugin *plugin, plugin_json *json,
                                 runtime_config_request *request, spisor_refusal *refusal) {
    if (plugin->places == NULL) {
        return SPISOR_OK;
    }
    json->place.secrets_file = plugin->places->secrets_file;
    json->place.force_file_store = plugin->places->force_file_store != 0;
    if (!runtime_is_usable_secret_place(&json->place)) {
        return runtime_refuse_secret_place(refusal);
    }
    request->places = &json->place;
    return SPISOR_OK;
}

/** The request a plugin's operation serves, its JSON held by `json`. */
static spisor_status plugin_request(const runtime_config_plugin *plugin, runtime_config_request *request,
                                    plugin_json *json, spisor_refusal *refusal) {
    spisor_status status;
    memset(json, 0, sizeof *json);
    if (plugin->plugin == NULL) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no plugin",
                                 "send plugin as the manifest id");
    }
    status = runtime_record_json(plugin->names, "names", &json->names, refusal);
    if (status == SPISOR_OK) {
        status = runtime_record_json(plugin->config, "config", &json->config, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_declarations_json(plugin->settings, plugin->setting_count, &json->settings, refusal);
    }
    if (status == SPISOR_OK) {
        status = read_places(plugin, json, request, refusal);
    }
    request->home = plugin->home;
    request->portable = plugin->portable;
    request->plugin = plugin->plugin;
    request->names = json->names;
    request->config = json->config;
    request->settings = json->settings;
    return status;
}

static int is_whole_plugin(const runtime_config_plugin *plugin) {
    return plugin != NULL && runtime_has_shape(plugin->size, PLUGIN_SHAPE);
}

static spisor_status all(void *context, const runtime_config_plugin *plugin, spisor_value_tree *values,
                         spisor_refusal *refusal) {
    runtime_config_request request = {NULL};
    plugin_json json;
    cJSON *merged = NULL;
    spisor_status status;
    (void)context;
    if (!is_whole_plugin(plugin) || !runtime_is_empty_tree(values)) {
        return runtime_refuse_shape(refusal, "config's all");
    }
    status = plugin_request(plugin, &request, &json, refusal);
    if (status == SPISOR_OK) {
        status = runtime_config_all_values(&request, &merged, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_tree_of_settings(merged, values, refusal);
    }
    cJSON_Delete(merged);
    free_plugin_json(&json);
    return status;
}

static spisor_status get(void *context, const runtime_config_plugin *plugin, const char *key,
                         spisor_value_tree *value, spisor_refusal *refusal) {
    runtime_config_request request = {NULL};
    plugin_json json;
    cJSON *found = NULL;
    spisor_status status;
    (void)context;
    if (!is_whole_plugin(plugin) || !runtime_is_empty_tree(value)) {
        return runtime_refuse_shape(refusal, "config's get");
    }
    status = plugin_request(plugin, &request, &json, refusal);
    request.key = key;
    if (status == SPISOR_OK) {
        status = runtime_config_get_value(&request, &found, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_tree_of_settings(found, value, refusal);
    }
    cJSON_Delete(found);
    free_plugin_json(&json);
    return status;
}

static spisor_status fill_set_answer(const cJSON *previous, int secret, runtime_config_set_answer *answer,
                                     spisor_refusal *refusal) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    spisor_status status = runtime_tree_of_settings(previous, &tree, refusal);
    if (status == SPISOR_OK) {
        answer->secret = secret ? 1 : 0;
        answer->previous = tree.root;
        answer->storage = tree.storage;
    }
    return status;
}

static spisor_status set(void *context, const runtime_config_plugin *plugin, const runtime_settings_change *change,
                         runtime_config_set_answer *answer, spisor_refusal *refusal) {
    runtime_config_request request = {NULL};
    plugin_json json;
    cJSON *previous = NULL;
    int secret = 0;
    spisor_status status;
    (void)context;
    if (!is_whole_plugin(plugin) || !RUNTIME_IS_EMPTY_ANSWER(answer, SET_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "config's set");
    }
    status = plugin_request(plugin, &request, &json, refusal);
    if (status == SPISOR_OK) {
        status = runtime_change_request(change, &request, &json.value, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_config_set_value(&request, &previous, &secret, refusal);
    }
    if (status == SPISOR_OK) {
        status = fill_set_answer(previous, secret, answer, refusal);
    }
    cJSON_Delete(previous);
    free_plugin_json(&json);
    return status;
}

static void release_set_answer(void *context, runtime_config_set_answer *answer) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    (void)context;
    if (answer == NULL || !runtime_has_shape(answer->size, SET_ANSWER_SHAPE)) {
        return;
    }
    tree.root = answer->previous;
    tree.storage = answer->storage;
    spisor_value_release(&tree);
    RUNTIME_CLEAR_ANSWER(answer, SET_ANSWER_SHAPE);
}

static const runtime_config_definition definition = {
    sizeof(runtime_config_definition), NULL, all, get, set, runtime_release_tree, release_set_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_config_definition_of(void) {
    return &definition;
}
