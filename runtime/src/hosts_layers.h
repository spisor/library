#ifndef RUNTIME_HOSTS_LAYERS_H
#define RUNTIME_HOSTS_LAYERS_H

#include "cJSON.h"

#define RUNTIME_LAYER_JAVASCRIPT "javascript"
#define RUNTIME_LAYER_NATIVE "native"

/** Whether runtime names a language layer a host runs plugins in. */
int runtime_is_layer_runtime(const char *runtime);

/** The layer item whose boot hint names the libraries a layer of runtime loads, NULL for no layer's runtime. */
const char *runtime_layer_item_of(const char *runtime);

/**
 * The hint of layer item layer_id, {libraries, host}, refused with the reinstall fix unless it names the version the
 * layer runs at; NULL with *refusal set, or with it NULL when memory runs out.
 */
cJSON *runtime_layer_hint(const char *cache, const char *layer_id, cJSON **refusal);

/** The runtime the catalog lists plugin id on, javascript for a plugin it does not list, as the catalog defaults one. */
const char *runtime_plugin_runtime(const cJSON *catalog, const char *id);

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
 * The current catalog a host's layers read each plugin's runtime from: that of the runtime_layer_sources. NULL with
 * *refusal set, or with it NULL when memory runs out.
 *
 * @implNote install records a catalog source in spisor-host's own layer, which no other host reads, so a loader's own
 * settings name no catalog and every plugin would read as javascript; updates' run lends that source to a loader's
 * check for the same reason.
 */
cJSON *runtime_layer_catalog(const char *state, const char *cache, const char *portable, const cJSON *settings,
                             cJSON **refusal);

/** Whether plugins, an object keyed by plugin id, names one the catalog lists on runtime. */
int runtime_names_plugin_on(const cJSON *catalog, const cJSON *plugins, const char *runtime);

/**
 * The runtime of the layer holding a host's front door: that of the first plugin standard, the host's loader standard
 * or NULL, requires, else javascript unless plugins, an object keyed by the plugin ids its start runs, names native
 * ones alone.
 */
const char *runtime_front_door(const cJSON *catalog, const cJSON *standard, const cJSON *plugins);

/**
 * The identity a run of host_id's runtime layer publishes its ready record and is held under: host_id for the layer
 * holding front_door, `<host_id>.<runtime>` for any other; NULL when memory runs out.
 */
char *runtime_run_identity(const char *host_id, const char *runtime, const char *front_door);

/**
 * What a host's layers stand on: the `sources` runtime_layer_sources answers and the `catalog` they hold, the
 * `plugins` a start of it resolves, an object of id to version, its `standard` at its registered version, NULL when it
 * carries none, and the runtime its `front_door` is in. Freed with runtime_free_loader_layers.
 */
typedef struct runtime_loader_layers {
    cJSON *registration;
    cJSON *settings;
    cJSON *sources;
    cJSON *catalog;
    cJSON *resolved;
    cJSON *standard;
    const cJSON *plugins;
    const char *front_door;
} runtime_loader_layers;

/**
 * Reads the layers of host_id under places, {state, cache, portable}, resolving its plugins as a start does from the
 * versions installed, writing nothing; 1, or 0 with *refusal set when it is not registered, its settings or catalog
 * cannot be read, or the first plugin its standard requires runs on a runtime no layer runs, so no layer can hold its
 * front door; or with it NULL when memory runs out, layers then holding nothing.
 */
int runtime_read_loader_layers(const cJSON *places, const char *host_id, runtime_loader_layers *layers,
                               cJSON **refusal);

/**
 * The identity a run of host_id's runtime layer runs as, read as runtime_read_loader_layers reads the front door; NULL
 * when that read refuses or memory runs out.
 */
char *runtime_read_run_identity(const cJSON *places, const char *host_id, const char *runtime);

void runtime_free_loader_layers(runtime_loader_layers *layers);

#endif
