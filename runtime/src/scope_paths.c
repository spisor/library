#include "allocation.h"
#include "paths.h"
#include "plugin_scope.h"
#include "segment.h"
#include "spisor/definition.h"

static spisor_status refuse_id(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "the plugin id is not a usable folder name in the cache place",
                            "name the plugin by a manifest id that is one portable path segment such as "
                            "\"my-plugin\"");
}

/** The plugin's own folder in the cache place, named by its manifest id as every plugin folder is. */
static spisor_status resolve_cache(const runtime_plugin_scope *scope, char **cache, spisor_refusal *refusal) {
    char *folder_name = NULL;
    spisor_status status = runtime_portable_segment(scope->manifest.id, &folder_name);
    *cache = NULL;
    if (status == SPISOR_MALFORMED) {
        return refuse_id(refusal);
    }
    if (status == SPISOR_OK) {
        *cache = runtime_plugin_folder(scope->host.cache, folder_name);
        status = *cache == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    }
    spisor_memory_release(folder_name);
    return status;
}

/** The config and plugin directories are the application's, as runtime's paths service resolves them. */
spisor_status runtime_scope_paths_resolve(runtime_plugin_scope *scope, spisor_refusal *refusal) {
    runtime_paths paths;
    runtime_scope_directories *directories = &scope->directories;
    spisor_status status = resolve_cache(scope, &directories->cache, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    if (runtime_resolve_paths(scope->host.home, scope->host.names, &paths) != 0) {
        spisor_memory_release(directories->cache);
        directories->cache = NULL;
        return SPISOR_OUT_OF_MEMORY;
    }
    directories->config = paths.config;
    directories->plugin = paths.plugin;
    paths.config = NULL;
    paths.plugin = NULL;
    runtime_free_paths(&paths);
    return SPISOR_OK;
}

static weivel_status answer_directory(const char *resolved, const char **directory) {
    if (directory == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    *directory = resolved;
    return WEIVEL_STATUS_OK;
}

static weivel_status cache_directory(void *context, const char **directory) {
    const runtime_plugin_scope *scope = context;
    return answer_directory(scope->directories.cache, directory);
}

static weivel_status config_directory(void *context, const char **directory) {
    const runtime_plugin_scope *scope = context;
    return answer_directory(scope->directories.config, directory);
}

static weivel_status plugin_directory(void *context, const char **directory) {
    const runtime_plugin_scope *scope = context;
    return answer_directory(scope->directories.plugin, directory);
}

void runtime_scope_paths_fill(runtime_plugin_scope *scope) {
    weivel_standard_paths *paths = &scope->paths;
    paths->size = (uint32_t)sizeof *paths;
    paths->context = scope;
    paths->cache = cache_directory;
    paths->config = config_directory;
    paths->plugin = plugin_directory;
}
