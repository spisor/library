#ifndef RUNTIME_HOSTS_LAYERS_H
#define RUNTIME_HOSTS_LAYERS_H

#include "cJSON.h"

#define RUNTIME_LAYER_JAVASCRIPT "javascript"
#define RUNTIME_LAYER_NATIVE "native"

/** The id of the language-host item that hosts plugins written in language, `<language>-host`; NULL out of memory. */
char *runtime_language_host_id(const char *language);

/** The language-host item the catalog lists for language, NULL when it lists none, or with *out_of_memory set. */
const cJSON *runtime_language_host_item(const cJSON *catalog, const char *language, int *out_of_memory);

/**
 * A refusal of a start of language's layer when the catalog lists no language-host item for it, naming the language
 * and the item; NULL when memory runs out.
 */
cJSON *runtime_unhosted_language_refusal(const char *language);

/**
 * The hint of layer item layer_id, {libraries, host}, refused with the reinstall fix unless it names the version the
 * layer runs at; NULL with *refusal set, or with it NULL when memory runs out.
 */
cJSON *runtime_layer_hint(const char *cache, const char *layer_id, cJSON **refusal);

/** The language the catalog lists plugin id in, NULL for an id it lists as no plugin or not at all. */
const char *runtime_plugin_language(const cJSON *catalog, const char *id);

/**
 * The loader standard bundles/<host_id>/<version>/host.json under cache carries, NULL when it cannot be read, with
 * *out_of_memory set when memory ran out.
 *
 * @implNote Read as JSON alone, not checked as loader.installed checks it: install refuses a loader whose standard
 * fails that check, and the layers read no more of it than its required plugins.
 */
cJSON *runtime_loader_standard(const char *cache, const char *host_id, const char *version, int *out_of_memory);

/**
 * The catalog sources a host's layers read their catalog from, as catalog.sources answers them: those spisor-host's
 * settings name, else those of settings, the host's own, when spisor-host is not registered or its registration
 * cannot be read. NULL with *refusal set, or with it NULL when memory runs out.
 */
cJSON *runtime_layer_sources(const char *state, const char *portable, const cJSON *settings, cJSON **refusal);

/**
 * The current catalog a host's layers read each plugin's language from: that of the runtime_layer_sources. NULL with
 * *refusal set, or with it NULL when memory runs out.
 *
 * @implNote install records a catalog source in spisor-host's own layer, which no other host reads, so a loader's own
 * settings name no catalog and no plugin would read as written in any language; updates' run lends that source to a
 * loader's check for the same reason.
 */
cJSON *runtime_layer_catalog(const char *state, const char *cache, const char *portable, const cJSON *settings,
                             cJSON **refusal);

/** Whether plugins, an object keyed by plugin id, names one the catalog lists in language. */
int runtime_names_plugin_in(const cJSON *catalog, const cJSON *plugins, const char *language);

/**
 * The identity a run of host_id's language layer publishes its ready record and is held under: host_id for the layer
 * holding front_door, and for every layer when front_door is NULL, `<host_id>.<language>` for any other; NULL when
 * memory runs out.
 */
char *runtime_run_identity(const char *host_id, const char *language, const char *front_door);

/**
 * What a host's layers stand on: the `sources` runtime_layer_sources answers, the `catalog` they hold and how many
 * of them are `yielding` it, the `plugins` a start of it resolves, an object of id to version, its `standard` at its
 * registered version, NULL when it carries none, and the language its `front_door` is in once
 * runtime_read_front_door has read it. Freed with runtime_free_loader_layers.
 */
typedef struct runtime_loader_layers {
    cJSON *registration;
    cJSON *settings;
    cJSON *sources;
    cJSON *catalog;
    size_t yielding;
    cJSON *resolved;
    cJSON *standard;
    const cJSON *plugins;
    const char *front_door;
} runtime_loader_layers;

/**
 * Reads the layers of host_id under places, {state, cache, portable}, resolving its plugins as a start does from the
 * versions installed, writing nothing and leaving its front door unread; 1, or 0 with *refusal set when it is not
 * registered or its settings or catalog cannot be read, or with it NULL when memory runs out, layers then holding
 * nothing.
 */
int runtime_read_loader_layers(const cJSON *places, const char *host_id, runtime_loader_layers *layers,
                               cJSON **refusal);

/**
 * Sets layers' front door: the language of the first plugin its standard requires, else the one language the catalog
 * lists its plugins in, NULL when it lists them in none. 1, or 0 with *refusal set when that first plugin is listed in
 * no language, or none is required and the plugins are written in two languages or more, naming them, so no language
 * can hold its front door. Layers still hold what they read either way.
 *
 * @implNote Kept out of runtime_read_loader_layers because only a start, and install's entry for one, needs a front
 * door: ui.session lists a loader's plugins whatever state its front door is in, the interface being where the user
 * fixes it.
 */
int runtime_read_front_door(runtime_loader_layers *layers, cJSON **refusal);

/**
 * The identity a run of host_id's language layer runs as, read as runtime_read_front_door reads the front door; NULL
 * when that read refuses or memory runs out.
 */
char *runtime_read_run_identity(const cJSON *places, const char *host_id, const char *language);

void runtime_free_loader_layers(runtime_loader_layers *layers);

#endif
