#include "allocation.h"
#include "paths.h"
#include "plugin_scope.h"
#include "segment.h"
#include "spisor/definition.h"
#include "spisor/path.h"

static spisor_status refuse_id(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "the plugin id is not a usable folder name",
                            "name the plugin by a manifest id that is one portable path segment such as "
                            "\"my-plugin\"");
}

/**
 * The plugin's own folder: `<portable>/plugins/<id>`, where its portable `config.json` lives (the home layout's
 * `ctx.paths.portable`), or, for a host with no portable layer, its folder in the application's plugin folder.
 */
static char *own_folder(const runtime_plugin_scope *scope, const char *folder_name) {
    runtime_paths paths;
    char *folder;
    const char *segments[2] = {NULL, folder_name};
    if (scope->host.portable != NULL) {
        return runtime_plugin_folder(scope->host.portable, folder_name);
    }
    if (runtime_resolve_paths(scope->host.home, scope->host.names, &paths) != 0) {
        return NULL;
    }
    segments[0] = paths.plugin;
    folder = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    runtime_free_paths(&paths);
    return folder;
}

void runtime_scope_directories_free(runtime_scope_directories *directories) {
    spisor_memory_release(directories->cache);
    spisor_memory_release(directories->config);
    spisor_memory_release(directories->plugin);
    directories->cache = directories->config = directories->plugin = NULL;
}

/**
 * Every folder is the plugin's alone, named by its manifest id as every plugin folder is: `cache` its folder in the
 * cache place, and `plugin` and `config` its own folder, which holds its settings file.
 */
spisor_status runtime_scope_paths_resolve(runtime_plugin_scope *scope, spisor_refusal *refusal) {
    runtime_scope_directories *directories = &scope->directories;
    char *folder_name = NULL;
    spisor_status status = runtime_portable_segment(scope->manifest.id, &folder_name);
    if (status == SPISOR_MALFORMED) {
        return refuse_id(refusal);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    directories->cache = runtime_plugin_folder(scope->host.cache, folder_name);
    directories->plugin = own_folder(scope, folder_name);
    directories->config = directories->plugin == NULL ? NULL : spisor_memory_duplicate_text(directories->plugin);
    spisor_memory_release(folder_name);
    if (directories->cache == NULL || directories->config == NULL) {
        runtime_scope_directories_free(directories);
        return SPISOR_OUT_OF_MEMORY;
    }
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
