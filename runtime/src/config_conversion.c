#include "config_conversion.h"

#include <stdio.h>
#include <string.h>

#include "definition_support.h"
#include "spisor/json_value.h"

#define CHANGE_SHAPE SPISOR_SHAPE_THROUGH(runtime_settings_change, layer)

static const char *const LAYER_NAMES[] = {NULL, "application", "portable"};

int runtime_is_empty_tree(const spisor_value_tree *tree) {
    return tree != NULL && spisor_has_shape(tree->size, SPISOR_SHAPE_THROUGH(spisor_value_tree, storage)) &&
           tree->storage == NULL && tree->root.kind == SPISOR_VALUE_NONE;
}

static int is_known_layer(uint32_t layer) {
    return layer <= RUNTIME_SETTINGS_LAYER_PORTABLE;
}

int runtime_is_settings_layer_name(const char *name) {
    for (size_t layer = RUNTIME_SETTINGS_LAYER_APPLICATION; layer <= RUNTIME_SETTINGS_LAYER_PORTABLE; layer++) {
        if (strcmp(LAYER_NAMES[layer], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static spisor_status refuse_layer(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the argument names no known layer",
                             "send layer as a runtime_settings_layer: application, portable, or default for the "
                             "declared or default layer");
}

spisor_status runtime_record_json(const spisor_value *value, const char *what, cJSON **json, spisor_refusal *refusal) {
    spisor_status status;
    char detail[160];
    *json = NULL;
    if (value == NULL) {
        return SPISOR_OK;
    }
    status = value->kind == SPISOR_VALUE_RECORD ? spisor_json_of_value(value, json) : SPISOR_MALFORMED;
    if (status != SPISOR_MALFORMED) {
        return status;
    }
    snprintf(detail, sizeof detail, "the %s are not a well-formed record", what);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail,
                             "send them as the manifest declares them, a record, or NULL for none");
}

spisor_status runtime_scope_request(const runtime_settings_scope *scope, runtime_config_request *request,
                                    cJSON **names, spisor_refusal *refusal) {
    spisor_status status = runtime_record_json(scope->names, "names", names, refusal);
    request->home = scope->home;
    request->portable = scope->portable;
    request->names = *names;
    return status;
}

static int add_declaration(cJSON *declarations, const runtime_setting_declaration *declaration) {
    cJSON *entry = cJSON_CreateObject();
    int built = entry != NULL &&
                (declaration->type == NULL || cJSON_AddStringToObject(entry, "type", declaration->type) != NULL) &&
                (declaration->layer == RUNTIME_SETTINGS_LAYER_DEFAULT ||
                 cJSON_AddStringToObject(entry, "layer", LAYER_NAMES[declaration->layer]) != NULL);
    if (!built || !cJSON_AddItemToObject(declarations, declaration->key, entry)) {
        cJSON_Delete(entry);
        return 0;
    }
    return 1;
}

static spisor_status refuse_declaration(const runtime_setting_declaration *declaration, spisor_refusal *refusal) {
    char detail[4096];
    if (declaration == NULL || declaration->key == NULL) {
        return spisor_refuse_as(refusal, SPISOR_MALFORMED, "a setting declaration carries no key",
                                 "send every declaration with its key, and setting_count no larger than the "
                                 "declarations sent");
    }
    snprintf(detail, sizeof detail, "the declared layer of \"%s\" is neither application nor portable",
             declaration->key);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, "declare the setting's layer as application or portable");
}

spisor_status runtime_declarations_json(const runtime_setting_declaration *settings, size_t count, cJSON **json,
                                        spisor_refusal *refusal) {
    *json = NULL;
    for (size_t index = 0; index < count; index++) {
        if (settings == NULL || settings[index].key == NULL || !is_known_layer(settings[index].layer)) {
            return refuse_declaration(settings == NULL ? NULL : &settings[index], refusal);
        }
    }
    if (count == 0) {
        return SPISOR_OK;
    }
    *json = cJSON_CreateObject();
    for (size_t index = 0; *json != NULL && index < count; index++) {
        if (!add_declaration(*json, &settings[index])) {
            cJSON_Delete(*json);
            *json = NULL;
        }
    }
    return *json == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

int runtime_is_whole_change(const runtime_settings_change *change) {
    return change != NULL && spisor_has_shape(change->size, CHANGE_SHAPE);
}

spisor_status runtime_change_request(const runtime_settings_change *change, runtime_config_request *request,
                                     cJSON **value, spisor_refusal *refusal) {
    spisor_status status = SPISOR_OK;
    *value = NULL;
    if (!runtime_is_whole_change(change)) {
        return spisor_refuse_shape(refusal, "a set");
    }
    if (!is_known_layer(change->layer)) {
        return refuse_layer(refusal);
    }
    if (change->value != NULL) {
        status = spisor_json_of_value(change->value, value);
    }
    if (status == SPISOR_MALFORMED) {
        return spisor_refuse_as(refusal, status, "the value is not a well-formed value",
                                 "send the value as a spisor_value spisor_value_check accepts, of any kind but NONE");
    }
    request->key = change->key;
    request->value = *value;
    request->layer = change->layer;
    return status;
}

spisor_status runtime_tree_of_settings(const cJSON *json, spisor_value_tree *tree, spisor_refusal *refusal) {
    spisor_status status = json == NULL ? SPISOR_OK : spisor_value_of_json(json, tree);
    if (status == SPISOR_MALFORMED) {
        return spisor_refuse_as(refusal, SPISOR_REFUSED, "the settings hold what no value tree can carry",
                                 "repair the settings files, then try again");
    }
    return status;
}

void runtime_release_tree(void *context, spisor_value_tree *tree) {
    (void)context;
    if (tree != NULL && spisor_has_shape(tree->size, SPISOR_SHAPE_THROUGH(spisor_value_tree, storage))) {
        spisor_value_release(tree);
    }
}
