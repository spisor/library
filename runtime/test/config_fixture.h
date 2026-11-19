#ifndef RUNTIME_TEST_CONFIG_FIXTURE_H
#define RUNTIME_TEST_CONFIG_FIXTURE_H

#include <stdlib.h>
#include <string.h>

#include "spisor/allocation.h"
#include "cJSON.h"
#include "spisor/json_value.h"
#include "runtime/config.h"
#include "runtime/settings.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the config and settings definitions from a request written as JSON test data, the shape the services once
 * took, and answers their typed answer written back as that JSON, so a case reads as data and checks as text.
 *
 * A request holds `home`, `plugin`, `portable`, `names`, `config`, `settings` (each declaration under its key as
 * {type?, layer?}), `places` ({secretsFile, forceFileStore}), `key`, `value` and `layer`. A layer named neither
 * "application" nor "portable" crosses as a layer no runtime_settings_layer has. An answer is {"values"} for all,
 * {"value"} or {} (NONE) for get, {"previous"?, "secret"?} for set, and {"message", "fix"} for a refusal.
 */

typedef enum config_operation { CONFIG_ALL, CONFIG_GET, CONFIG_SET, SETTINGS_ALL, SETTINGS_SET } config_operation;

#define CONFIG_UNKNOWN_LAYER 99u

typedef struct config_answer {
    spisor_status status;
    cJSON *tree;
} config_answer;

/** Whether a call answered, a value or none. */
#define ANSWERED(answer) ((answer).status == SPISOR_OK || (answer).status == SPISOR_NONE)

/** Whether a call was refused, as malformed or declined. */
#define REFUSED_CALL(answer) ((answer).status == SPISOR_MALFORMED || (answer).status == SPISOR_REFUSED)

typedef struct typed_request {
    spisor_value_tree names;
    spisor_value_tree config;
    spisor_value_tree value;
    runtime_setting_declaration *declarations;
    size_t declaration_count;
    runtime_secret_places places;
    runtime_config_get_argument get;
    runtime_config_plugin plugin;
    runtime_settings_scope scope;
    runtime_settings_change change;
} typed_request;

static inline const char *config_fixture_text(const cJSON *request, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, member));
}

static inline uint32_t config_fixture_layer(const cJSON *layer) {
    if (layer == NULL) {
        return RUNTIME_SETTINGS_LAYER_DEFAULT;
    }
    if (cJSON_IsString(layer) && strcmp(layer->valuestring, "application") == 0) {
        return RUNTIME_SETTINGS_LAYER_APPLICATION;
    }
    if (cJSON_IsString(layer) && strcmp(layer->valuestring, "portable") == 0) {
        return RUNTIME_SETTINGS_LAYER_PORTABLE;
    }
    return CONFIG_UNKNOWN_LAYER;
}

/** Reads a member present in the request as a value tree, leaving the tree empty when it is absent. */
static inline const spisor_value *config_fixture_value(const cJSON *request, const char *member,
                                                       spisor_value_tree *tree) {
    if (cJSON_GetObjectItemCaseSensitive(request, member) == NULL || !spisor_vector_value(request, member, tree)) {
        return NULL;
    }
    return &tree->root;
}

static inline void config_fixture_declarations(const cJSON *request, typed_request *typed) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(request, "settings");
    const cJSON *declaration;
    typed->declaration_count = 0;
    typed->declarations =
        spisor_memory_allocate(((size_t)cJSON_GetArraySize(settings) + 1) * sizeof *typed->declarations);
    cJSON_ArrayForEach(declaration, settings) {
        runtime_setting_declaration *entry = &typed->declarations[typed->declaration_count++];
        entry->key = declaration->string;
        entry->type = config_fixture_text(declaration, "type");
        entry->layer = config_fixture_layer(cJSON_GetObjectItemCaseSensitive(declaration, "layer"));
    }
}

static inline void config_fixture_read(const cJSON *request, typed_request *typed) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(request, "places");
    runtime_config_plugin plugin = SPISOR_INIT(runtime_config_plugin);
    runtime_settings_scope scope = SPISOR_INIT(runtime_settings_scope);
    runtime_settings_change change = SPISOR_INIT(runtime_settings_change);
    spisor_value_tree empty = SPISOR_INIT(spisor_value_tree);
    runtime_secret_places no_places = SPISOR_INIT(runtime_secret_places);
    runtime_config_get_argument get = SPISOR_INIT(runtime_config_get_argument);
    typed->places = no_places;
    typed->get = get;
    typed->names = typed->config = typed->value = empty;
    config_fixture_declarations(request, typed);
    plugin.home = scope.home = config_fixture_text(request, "home");
    plugin.portable = scope.portable = config_fixture_text(request, "portable");
    plugin.names = scope.names = config_fixture_value(request, "names", &typed->names);
    plugin.plugin = config_fixture_text(request, "plugin");
    plugin.config = config_fixture_value(request, "config", &typed->config);
    plugin.settings = typed->declarations;
    plugin.setting_count = typed->declaration_count;
    if (places != NULL) {
        typed->places.secrets_file = config_fixture_text(places, "secretsFile");
        typed->places.force_file_store = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(places, "forceFileStore"));
        plugin.places = &typed->places;
    }
    change.key = config_fixture_text(request, "key");
    typed->get.key = change.key;
    change.value = config_fixture_value(request, "value", &typed->value);
    change.layer = config_fixture_layer(cJSON_GetObjectItemCaseSensitive(request, "layer"));
    typed->plugin = plugin;
    typed->scope = scope;
    typed->change = change;
}

static inline void config_fixture_forget(typed_request *typed) {
    spisor_value_release(&typed->names);
    spisor_value_release(&typed->config);
    spisor_value_release(&typed->value);
    spisor_memory_release(typed->declarations);
}

/** Adds a value under member, nothing for a value of kind NONE. */
static inline void config_fixture_add_value(cJSON *tree, const char *member, const spisor_value *value) {
    cJSON *json = NULL;
    if (value->kind != SPISOR_VALUE_NONE && spisor_json_of_value(value, &json) == SPISOR_OK) {
        cJSON_AddItemToObject(tree, member, json);
    }
}

static inline cJSON *config_fixture_refusal(const spisor_refusal *refusal) {
    cJSON *tree = cJSON_CreateObject();
    if (refusal->detail != NULL) {
        cJSON_AddStringToObject(tree, "message", refusal->detail);
    }
    if (refusal->fix != NULL) {
        cJSON_AddStringToObject(tree, "fix", refusal->fix);
    }
    return tree;
}

static inline spisor_status config_fixture_tree_call(config_operation operation, typed_request *typed,
                                                     const char *member, cJSON *tree, spisor_refusal *refusal) {
    const runtime_config_definition *config = runtime_config_definition_of();
    const runtime_settings_definition *settings = runtime_settings_definition_of();
    spisor_value_tree answer = SPISOR_INIT(spisor_value_tree);
    spisor_status status;
    if (operation == CONFIG_ALL) {
        status = config->all(config->context, &typed->plugin, &answer, refusal);
    } else if (operation == CONFIG_GET) {
        status = config->get(config->context, &typed->plugin, &typed->get, &answer, refusal);
    } else if (operation == SETTINGS_ALL) {
        status = settings->all(settings->context, &typed->scope, &answer, refusal);
    } else {
        status = settings->set(settings->context, &typed->scope, &typed->change, &answer, refusal);
    }
    if (status == SPISOR_OK) {
        config_fixture_add_value(tree, member, &answer.root);
    }
    if (operation == CONFIG_ALL || operation == CONFIG_GET) {
        config->release_tree(config->context, &answer);
    } else {
        settings->release_tree(settings->context, &answer);
    }
    return status;
}

static inline spisor_status config_fixture_set_call(typed_request *typed, cJSON *tree, spisor_refusal *refusal) {
    const runtime_config_definition *config = runtime_config_definition_of();
    runtime_config_set_answer answer = SPISOR_INIT(runtime_config_set_answer);
    spisor_status status = config->set(config->context, &typed->plugin, &typed->change, &answer, refusal);
    if (status == SPISOR_OK) {
        config_fixture_add_value(tree, "previous", &answer.previous);
        if (answer.secret) {
            cJSON_AddTrueToObject(tree, "secret");
        }
    }
    config->release_set_answer(config->context, &answer);
    return status;
}

/** Calls one operation with the request, which it deletes, and answers its status and its answer as JSON. */
static inline config_answer config_call(config_operation operation, cJSON *request) {
    static const char *const members[] = {"values", "value", "previous", "values", "previous"};
    const runtime_config_definition *config = runtime_config_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    typed_request typed;
    config_answer result;
    result.tree = cJSON_CreateObject();
    config_fixture_read(request, &typed);
    result.status = operation == CONFIG_SET
                        ? config_fixture_set_call(&typed, result.tree, &refusal)
                        : config_fixture_tree_call(operation, &typed, members[operation], result.tree, &refusal);
    if (!ANSWERED(result)) {
        cJSON_Delete(result.tree);
        result.tree = config_fixture_refusal(&refusal);
    }
    config->release_refusal(config->context, &refusal);
    config_fixture_forget(&typed);
    cJSON_Delete(request);
    return result;
}

#endif
