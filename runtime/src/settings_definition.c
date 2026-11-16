#include "runtime/settings.h"

#include "config.h"
#include "config_conversion.h"
#include "definition_support.h"

#define SCOPE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_settings_scope, portable)

static int is_whole_scope(const runtime_settings_scope *scope) {
    return scope != NULL && runtime_has_shape(scope->size, SCOPE_SHAPE);
}

static spisor_status all(void *context, const runtime_settings_scope *scope, spisor_value_tree *values,
                         spisor_refusal *refusal) {
    runtime_config_request request = {NULL};
    cJSON *names = NULL;
    cJSON *merged = NULL;
    spisor_status status;
    (void)context;
    if (!is_whole_scope(scope) || !runtime_is_empty_tree(values)) {
        return runtime_refuse_shape(refusal, "settings' all");
    }
    status = runtime_scope_request(scope, &request, &names, refusal);
    if (status == SPISOR_OK) {
        status = runtime_config_all_values(&request, &merged, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_tree_of_settings(merged, values, refusal);
    }
    cJSON_Delete(merged);
    cJSON_Delete(names);
    return status;
}

static spisor_status set(void *context, const runtime_settings_scope *scope, const runtime_settings_change *change,
                         spisor_value_tree *previous, spisor_refusal *refusal) {
    runtime_config_request request = {NULL};
    cJSON *names = NULL;
    cJSON *value = NULL;
    cJSON *held = NULL;
    int secret = 0;
    spisor_status status;
    (void)context;
    if (!is_whole_scope(scope) || !runtime_is_empty_tree(previous)) {
        return runtime_refuse_shape(refusal, "settings' set");
    }
    status = runtime_scope_request(scope, &request, &names, refusal);
    if (status == SPISOR_OK) {
        status = runtime_change_request(change, &request, &value, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_config_set_value(&request, &held, &secret, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_tree_of_settings(held, previous, refusal);
    }
    cJSON_Delete(held);
    cJSON_Delete(value);
    cJSON_Delete(names);
    return status;
}

static const runtime_settings_definition definition = {
    sizeof(runtime_settings_definition), NULL, all, set, runtime_release_tree, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_settings_definition_of(void) {
    return &definition;
}
