#include "plugin_scope.h"

#include "allocation.h"
#include "spisor/definition.h"
#include "atomic.h"

runtime_config_request runtime_plugin_scope_request(const runtime_plugin_scope *scope) {
    runtime_config_request request = runtime_config_request_of(scope->host.home, scope->host.portable);
    request.plugin = scope->manifest.id;
    request.names = scope->host.names;
    request.config = scope->manifest.config;
    request.settings = scope->manifest.settings;
    return request;
}

runtime_secret_places runtime_plugin_scope_secret_places(const runtime_plugin_scope *scope) {
    runtime_secret_places places;
    places.size = (uint32_t)sizeof places;
    places.secrets_file = scope->host.secrets_file;
    places.force_file_store = scope->host.force_file_store;
    return places;
}

void runtime_plugin_scope_hold(runtime_plugin_scope *scope) {
    runtime_atomic_increment(&scope->references);
}

void runtime_plugin_scope_release(runtime_plugin_scope *scope) {
    if (scope == NULL || runtime_atomic_decrement(&scope->references) > 0) {
        return;
    }
    runtime_scope_host_free(&scope->host);
    runtime_scope_manifest_free(&scope->manifest);
    runtime_scope_directories_free(&scope->directories);
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
    return runtime_check_scope_plugin(&request, manifest_path, refusal);
}

static void fill_definitions(runtime_plugin_scope *scope) {
    runtime_scope_config_fill(scope);
    runtime_scope_secrets_fill(scope);
    runtime_scope_paths_fill(scope);
    runtime_scope_log_fill(scope);
}

spisor_status runtime_plugin_scope_open(const char *manifest_path, runtime_plugin_scope **scope,
                                        spisor_refusal *refusal) {
    runtime_plugin_scope *opened = spisor_memory_allocate_zeroed(1, sizeof *opened);
    spisor_status status = opened == NULL ? SPISOR_OUT_OF_MEMORY : runtime_scopes_host(&opened->host);
    *scope = NULL;
    if (opened != NULL) {
        opened->references = 1;
    }
    if (status == SPISOR_NONE) {
        status = refuse_unstarted(refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_read_scope_manifest(manifest_path, &opened->manifest, refusal);
    }
    if (status == SPISOR_OK) {
        status = check_plugin(opened, manifest_path, refusal);
    }
    if (status == SPISOR_OK) {
        status = runtime_scope_paths_resolve(opened, refusal);
    }
    if (status != SPISOR_OK) {
        runtime_plugin_scope_release(opened);
        return status;
    }
    fill_definitions(opened);
    *scope = opened;
    return SPISOR_OK;
}
