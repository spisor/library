#include "hosts_layers.h"
#include "allocation.h"

#include <string.h>

#include "boot.h"
#include "cache_folders.h"
#include "cache_refusal.h"
#include "catalog.h"
#include "config.h"
#include "install_entries.h"
#include "install_run.h"
#include "json_service.h"
#include "node_json_file.h"
#include "paths.h"
#include "registry.h"
#include "updates_catalog.h"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

int runtime_is_layer_runtime(const char *runtime) {
    return runtime != NULL &&
           (strcmp(runtime, RUNTIME_LAYER_JAVASCRIPT) == 0 || strcmp(runtime, RUNTIME_LAYER_NATIVE) == 0);
}

const char *runtime_layer_item_of(const char *runtime) {
    if (runtime != NULL && strcmp(runtime, RUNTIME_LAYER_JAVASCRIPT) == 0) {
        return RUNTIME_JAVASCRIPT_HOST_ID;
    }
    return runtime != NULL && strcmp(runtime, RUNTIME_LAYER_NATIVE) == 0 ? RUNTIME_NATIVE_HOST_ID : NULL;
}

cJSON *runtime_layer_hint(const char *cache, const char *layer_id, cJSON **refusal) {
    cJSON *read = runtime_boot_read(cache, layer_id, refusal);
    cJSON *hint = cJSON_DetachItemFromObjectCaseSensitive(read, "hint");
    cJSON_Delete(read);
    if (read != NULL && text_of(hint, "host") == NULL) {
        const char *parts[5] = {"no ", layer_id, " version is recorded in ", cache,
                                ", which runs the plugins the loader's settings enable"};
        *refusal = runtime_cache_refusal(parts, 5, RUNTIME_REINSTALL_FIX);
        cJSON_Delete(hint);
        hint = NULL;
    }
    return hint;
}

const char *runtime_plugin_runtime(const cJSON *catalog, const char *id) {
    const cJSON *runtime = cJSON_GetObjectItemCaseSensitive(runtime_updates_item(catalog, id), "runtime");
    return cJSON_IsString(runtime) && runtime->valuestring[0] != '\0' ? runtime->valuestring
                                                                      : RUNTIME_LAYER_JAVASCRIPT;
}

cJSON *runtime_loader_standard(const char *cache, const char *host_id, const char *version, int *out_of_memory) {
    const char *segments[5] = {cache, "bundles", host_id, version, "host.json"};
    char *file = version == NULL ? NULL : runtime_cache_joined(segments, 5);
    cJSON *standard = file == NULL ? NULL : runtime_read_node_json(file, out_of_memory);
    *out_of_memory = *out_of_memory || (version != NULL && file == NULL);
    spisor_memory_release(file);
    return standard;
}

int runtime_names_plugin_on(const cJSON *catalog, const cJSON *plugins, const char *runtime) {
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, plugins) {
        if (strcmp(runtime_plugin_runtime(catalog, plugin->string), runtime) == 0) {
            return 1;
        }
    }
    return 0;
}

const char *runtime_front_door(const cJSON *catalog, const cJSON *standard, const cJSON *plugins) {
    const cJSON *required = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(standard, "plugins"), 0);
    if (cJSON_IsString(required)) {
        return runtime_plugin_runtime(catalog, required->valuestring);
    }
    return !runtime_names_plugin_on(catalog, plugins, RUNTIME_LAYER_JAVASCRIPT) &&
                   runtime_names_plugin_on(catalog, plugins, RUNTIME_LAYER_NATIVE)
               ? RUNTIME_LAYER_NATIVE
               : RUNTIME_LAYER_JAVASCRIPT;
}

char *runtime_run_identity(const char *host_id, const char *runtime, const char *front_door) {
    const char *parts[3] = {host_id, ".", runtime};
    return runtime_cache_text(parts, strcmp(front_door, runtime) == 0 ? 1 : 3);
}

/** spisor-host's settings values, NULL when it is not registered usably, and NULL with *out_of_memory set when memory runs out. */
static cJSON *ecosystem_settings(const char *state, const char *portable, int *out_of_memory, cJSON **refusal) {
    cJSON *unregistered = NULL;
    cJSON *registration = runtime_usable_registration(state, RUNTIME_ECOSYSTEM_HOST_ID, &unregistered);
    cJSON *settings = registration == NULL ? NULL
                                           : runtime_ecosystem_settings_values(text_of(registration, "home"), portable,
                                                                               refusal);
    *out_of_memory = registration == NULL ? unregistered == NULL : (settings == NULL && *refusal == NULL);
    cJSON_Delete(unregistered);
    cJSON_Delete(registration);
    return settings;
}

cJSON *runtime_layer_catalog(const char *state, const char *cache, const char *portable, const cJSON *settings,
                             cJSON **refusal) {
    int out_of_memory = 0;
    *refusal = NULL;
    cJSON *ecosystem = ecosystem_settings(state, portable, &out_of_memory, refusal);
    int unreadable = ecosystem == NULL && *refusal != NULL;
    cJSON *sources = out_of_memory || unreadable ? NULL
                                                 : runtime_updates_sources(ecosystem == NULL ? settings : ecosystem,
                                                                           refusal);
    cJSON *catalog = sources == NULL ? NULL : runtime_catalog_current(sources, cache, refusal);
    cJSON_Delete(sources);
    cJSON_Delete(ecosystem);
    return catalog;
}

static cJSON *settings_of(const cJSON *registration, const char *portable, cJSON **refusal) {
    const char *home = text_of(registration, "home");
    int failed = 0;
    if (!runtime_is_usable_home(home)) {
        *refusal = runtime_refusal(RUNTIME_UNUSABLE_HOME_MESSAGE, RUNTIME_UNUSABLE_HOME_FIX);
        return NULL;
    }
    return runtime_read_ecosystem_settings(home, NULL, portable, &failed);
}

/**
 * A refusal of a loader whose first required plugin runs on a runtime no layer runs, naming both; NULL when memory runs
 * out.
 */
static cJSON *doorless_refusal(const cJSON *standard, const char *runtime) {
    const char *plugin = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(standard, "plugins"), 0));
    const char *parts[5] = {plugin, ", the first plugin the loader requires, runs on ", runtime,
                            ", which no layer runs, so no layer can hold the loader's front door"};
    const char *fix_parts[3] = {"install a version of ", plugin, " written for javascript or native"};
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 4, fix);
    spisor_memory_release(fix);
    return refusal;
}

int runtime_read_loader_layers(const cJSON *places, const char *host_id, runtime_loader_layers *layers,
                               cJSON **refusal) {
    const char *cache = text_of(places, "cache");
    cJSON *resolving = NULL;
    int out_of_memory = 0;
    memset(layers, 0, sizeof *layers);
    *refusal = NULL;
    layers->registration = runtime_usable_registration(text_of(places, "state"), host_id, refusal);
    layers->settings = layers->registration == NULL ? NULL
                                                    : settings_of(layers->registration, text_of(places, "portable"),
                                                                  refusal);
    layers->resolved = layers->settings == NULL
                           ? NULL
                           : runtime_updates_resolve_plugins(layers->settings, NULL, cache, &resolving, refusal);
    cJSON_Delete(resolving);
    layers->plugins = cJSON_GetObjectItemCaseSensitive(layers->resolved, "run");
    layers->catalog = layers->resolved == NULL ? NULL
                                               : runtime_layer_catalog(text_of(places, "state"), cache,
                                                                       text_of(places, "portable"), layers->settings,
                                                                       refusal);
    layers->standard = layers->catalog == NULL ? NULL
                                                : runtime_loader_standard(cache, host_id,
                                                                          text_of(layers->registration, "version"),
                                                                          &out_of_memory);
    if (layers->catalog == NULL || out_of_memory) {
        runtime_free_loader_layers(layers);
        return 0;
    }
    layers->front_door = runtime_front_door(layers->catalog, layers->standard, layers->plugins);
    if (!runtime_is_layer_runtime(layers->front_door)) {
        *refusal = doorless_refusal(layers->standard, layers->front_door);
        runtime_free_loader_layers(layers);
        return 0;
    }
    return 1;
}

char *runtime_read_run_identity(const cJSON *places, const char *host_id, const char *runtime) {
    runtime_loader_layers layers;
    cJSON *refusal = NULL;
    char *identity = NULL;
    if (runtime_read_loader_layers(places, host_id, &layers, &refusal)) {
        identity = runtime_run_identity(host_id, runtime, layers.front_door);
        runtime_free_loader_layers(&layers);
    }
    cJSON_Delete(refusal);
    return identity;
}

void runtime_free_loader_layers(runtime_loader_layers *layers) {
    cJSON_Delete(layers->registration);
    cJSON_Delete(layers->settings);
    cJSON_Delete(layers->catalog);
    cJSON_Delete(layers->resolved);
    cJSON_Delete(layers->standard);
    memset(layers, 0, sizeof *layers);
}
