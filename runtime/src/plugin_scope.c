#include "plugin_scope.h"

#include <stdio.h>

#include "allocation.h"
#include "spisor/definition.h"

runtime_config_request runtime_plugin_scope_request(const runtime_plugin_scope *scope) {
    runtime_config_request request = runtime_config_request_of(scope->host.home, scope->host.portable);
    request.plugin = scope->manifest.id;
    request.names = scope->host.names;
    request.config = scope->manifest.config;
    request.settings = scope->manifest.settings;
    return request;
}

void runtime_plugin_scope_free(runtime_plugin_scope *scope) {
    if (scope == NULL) {
        return;
    }
    runtime_scope_host_free(&scope->host);
    runtime_scope_manifest_free(&scope->manifest);
    spisor_memory_release(scope);
}

static spisor_status refuse_unstarted(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "no host is started, so no plugin's scope can open",
                            "start the host through weivel.host before opening a plugin's scope");
}

/** The host's checks of every later operation, made once, so a name or id no operation could use refuses here. */
static spisor_status check_plugin(const runtime_plugin_scope *scope, const char *manifest_path,
                                  spisor_refusal *refusal) {
    runtime_config_request request = runtime_plugin_scope_request(scope);
    spisor_refusal checked = SPISOR_INIT(spisor_refusal);
    spisor_status status = runtime_config_check_request(&request, &checked);
    char detail[4096];
    if (status != SPISOR_MALFORMED) {
        spisor_refusal_release(&checked);
        return status;
    }
    snprintf(detail, sizeof detail, "the manifest at %s is refused: %s", manifest_path,
             checked.detail == NULL ? "its plugin cannot be located" : checked.detail);
    status = spisor_refuse_as(refusal, SPISOR_REFUSED, detail, checked.fix);
    spisor_refusal_release(&checked);
    return status;
}

spisor_status runtime_plugin_scope_open(const char *manifest_path, runtime_plugin_scope **scope,
                                        spisor_refusal *refusal) {
    runtime_plugin_scope *opened = spisor_memory_allocate_zeroed(1, sizeof *opened);
    spisor_status status = opened == NULL ? SPISOR_OUT_OF_MEMORY : runtime_scopes_host(&opened->host);
    *scope = NULL;
    if (status == SPISOR_NONE) {
        status = refuse_unstarted(refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_read_scope_manifest(manifest_path, &opened->manifest, refusal);
    }
    if (status == SPISOR_OK) {
        status = check_plugin(opened, manifest_path, refusal);
    }
    if (status != SPISOR_OK) {
        runtime_plugin_scope_free(opened);
        return status;
    }
    runtime_scope_config_fill(opened);
    *scope = opened;
    return SPISOR_OK;
}
