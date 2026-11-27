#include "hosts_layers.h"
#include "allocation.h"

#include <string.h>

#include "boot.h"
#include "cache_folders.h"
#include "cache_refusal.h"
#include "catalog.h"
#include "config.h"
#include "install_run.h"
#include "json_service.h"
#include "node_json_file.h"
#include "paths.h"
#include "registry.h"
#include "updates_catalog.h"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

char *runtime_language_host_id(const char *language) {
    const char *parts[2] = {language, "-host"};
    return runtime_cache_text(parts, 2);
}

const cJSON *runtime_language_host_item(const cJSON *catalog, const char *language, int *out_of_memory) {
    char *id = runtime_language_host_id(language);
    const cJSON *item = id == NULL ? NULL : runtime_updates_item(catalog, id);
    const char *kind = text_of(item, "kind");
    *out_of_memory = id == NULL;
    spisor_memory_release(id);
    return kind != NULL && strcmp(kind, "language-host") == 0 ? item : NULL;
}

cJSON *runtime_unhosted_language_refusal(const char *language) {
    char *id = runtime_language_host_id(language);
    const char *parts[5] = {"plugins written in ", language, " have no host: no catalog source lists ", id,
                            ", the language-host item that runs them"};
    const char *fix_parts[4] = {"add a catalog source listing ", id, ", or disable the plugins written in ", language};
    char *fix = id == NULL ? NULL : runtime_cache_text(fix_parts, 4);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 5, fix);
    spisor_memory_release(fix);
    spisor_memory_release(id);
    return refusal;
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

const char *runtime_plugin_language(const cJSON *catalog, const char *id) {
    const cJSON *item = runtime_updates_item(catalog, id);
    const char *kind = text_of(item, "kind");
    return kind != NULL && strcmp(kind, "plugin") == 0 ? text_of(item, "language") : NULL;
}

cJSON *runtime_loader_standard(const char *cache, const char *host_id, const char *version, int *out_of_memory) {
    const char *segments[5] = {cache, "bundles", host_id, version, "host.json"};
    char *file = version == NULL ? NULL : runtime_cache_joined(segments, 5);
    cJSON *standard = file == NULL ? NULL : runtime_read_node_json(file, out_of_memory);
    *out_of_memory = *out_of_memory || (version != NULL && file == NULL);
    spisor_memory_release(file);
    return standard;
}

int runtime_names_plugin_in(const cJSON *catalog, const cJSON *plugins, const char *language) {
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, plugins) {
        const char *written = runtime_plugin_language(catalog, plugin->string);
        if (written != NULL && strcmp(written, language) == 0) {
            return 1;
        }
    }
    return 0;
}

char *runtime_run_identity(const char *host_id, const char *language, const char *front_door) {
    const char *parts[3] = {host_id, ".", language};
    return runtime_cache_text(parts, front_door == NULL || strcmp(front_door, language) == 0 ? 1 : 3);
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

cJSON *runtime_layer_sources(const char *state, const char *portable, const cJSON *settings, cJSON **refusal) {
    int out_of_memory = 0;
    *refusal = NULL;
    cJSON *ecosystem = ecosystem_settings(state, portable, &out_of_memory, refusal);
    int unreadable = ecosystem == NULL && *refusal != NULL;
    cJSON *sources = out_of_memory || unreadable ? NULL
                                                 : runtime_updates_sources(ecosystem == NULL ? settings : ecosystem,
                                                                           refusal);
    cJSON_Delete(ecosystem);
    return sources;
}

cJSON *runtime_layer_catalog(const char *state, const char *cache, const char *portable, const cJSON *settings,
                             cJSON **refusal) {
    cJSON *sources = runtime_layer_sources(state, portable, settings, refusal);
    cJSON *catalog = sources == NULL ? NULL : runtime_catalog_current(sources, cache, refusal);
    cJSON_Delete(sources);
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

/** A refusal of a loader whose first required plugin the catalog lists in no language; NULL when memory runs out. */
static cJSON *doorless_refusal(const char *plugin) {
    const char *parts[2] = {plugin, ", the first plugin the loader requires, is listed in no catalog source as a "
                                    "plugin, so no language can hold the loader's front door"};
    const char *fix_parts[3] = {"add a catalog source listing ", plugin, ", then install the loader again"};
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 2, fix);
    spisor_memory_release(fix);
    return refusal;
}

/** A refusal of a loader requiring no plugin whose plugins are written in first and second; NULL out of memory. */
static cJSON *two_doors_refusal(const char *first, const char *second) {
    const char *parts[5] = {"the loader requires no plugin, and the plugins its settings enable are written in ",
                            first, " and ", second, ", so no one language holds its front door"};
    return runtime_cache_refusal(parts, 5, "require the plugin that holds the front door in the loader's standard, or "
                                           "enable plugins of one language only, then install the loader again");
}

int runtime_read_front_door(runtime_loader_layers *layers, cJSON **refusal) {
    const cJSON *required = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(layers->standard, "plugins"), 0);
    const cJSON *plugin;
    if (cJSON_IsString(required)) {
        layers->front_door = runtime_plugin_language(layers->catalog, required->valuestring);
        *refusal = layers->front_door == NULL ? doorless_refusal(required->valuestring) : NULL;
        return layers->front_door != NULL;
    }
    cJSON_ArrayForEach(plugin, layers->plugins) {
        const char *language = runtime_plugin_language(layers->catalog, plugin->string);
        if (language != NULL && layers->front_door != NULL && strcmp(language, layers->front_door) != 0) {
            *refusal = two_doors_refusal(layers->front_door, language);
            return 0;
        }
        layers->front_door = language == NULL ? layers->front_door : language;
    }
    return 1;
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
    layers->sources = layers->resolved == NULL ? NULL
                                               : runtime_layer_sources(text_of(places, "state"),
                                                                       text_of(places, "portable"), layers->settings,
                                                                       refusal);
    layers->catalog = layers->sources == NULL ? NULL
                                              : runtime_catalog_current_counted(layers->sources, cache,
                                                                                &layers->yielding, refusal);
    layers->standard = layers->catalog == NULL ? NULL
                                                : runtime_loader_standard(cache, host_id,
                                                                          text_of(layers->registration, "version"),
                                                                          &out_of_memory);
    if (layers->catalog == NULL || out_of_memory) {
        runtime_free_loader_layers(layers);
        return 0;
    }
    return 1;
}

char *runtime_read_run_identity(const cJSON *places, const char *host_id, const char *language) {
    runtime_loader_layers layers;
    cJSON *refusal = NULL;
    char *identity = NULL;
    if (runtime_read_loader_layers(places, host_id, &layers, &refusal)) {
        identity = runtime_read_front_door(&layers, &refusal)
                       ? runtime_run_identity(host_id, language, layers.front_door)
                       : NULL;
        runtime_free_loader_layers(&layers);
    }
    cJSON_Delete(refusal);
    return identity;
}

void runtime_free_loader_layers(runtime_loader_layers *layers) {
    cJSON_Delete(layers->registration);
    cJSON_Delete(layers->settings);
    cJSON_Delete(layers->sources);
    cJSON_Delete(layers->catalog);
    cJSON_Delete(layers->resolved);
    cJSON_Delete(layers->standard);
    memset(layers, 0, sizeof *layers);
}
