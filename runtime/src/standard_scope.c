#include "runtime/standard.h"

#include <string.h>

#include <weivel/descriptions.h>
#include <weivel/scope.h>

#include "plugin_scope.h"
#include "spisor/definition.h"
#include "standard_refusal.h"

#define SCOPE_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_scope, storage)

static spisor_status refuse_missing_manifest(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, "scopes' open was called without a manifest path",
                            "pass the path of the plugin's manifest");
}

static weivel_status open_scope(void *context, const char *manifest_path, weivel_standard_scope *scope,
                                weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    runtime_plugin_scope *opened = NULL;
    spisor_status status;
    (void)context;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (!SPISOR_IS_EMPTY_ANSWER(scope, SCOPE_SHAPE)) {
        return runtime_answer_service(spisor_refuse_shape(&filled, "scopes' open"), &filled, refusal);
    }
    if (manifest_path == NULL) {
        return runtime_answer_service(refuse_missing_manifest(&filled), &filled, refusal);
    }
    status = runtime_plugin_scope_open(manifest_path, &opened, &filled);
    if (status == SPISOR_OK) {
        scope->config = &opened->config;
        scope->secrets = NULL;
        scope->paths = NULL;
        scope->log = NULL;
        scope->storage = opened;
        runtime_zero_beyond(scope, sizeof *scope);
    }
    return runtime_answer_service(status, &filled, refusal);
}

static void release_scope(void *context, weivel_standard_scope *scope) {
    (void)context;
    if (scope == NULL || !spisor_has_shape(scope->size, SCOPE_SHAPE)) {
        return;
    }
    runtime_plugin_scope_free(scope->storage);
    SPISOR_CLEAR_ANSWER(scope, SCOPE_SHAPE);
}

static const weivel_standard_scopes definition = {
    sizeof(weivel_standard_scopes), NULL, open_scope, release_scope, runtime_release_service_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_standard_scope_definition_of(void) {
    return &definition;
}

RUNTIME_DEFINITION_API const weivel_standard_description *runtime_standard_scope_definition_description(void) {
    return weivel_standard_scopes_description();
}
