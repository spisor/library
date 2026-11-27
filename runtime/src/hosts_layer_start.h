#ifndef RUNTIME_HOSTS_LAYER_START_H
#define RUNTIME_HOSTS_LAYER_START_H

#include <stdint.h>

#include "cJSON.h"

/**
 * The start of the entry of bundle `id` at `version` under `cache` on `runtime`, javascript or native, in
 * `environment`, an object of variable names to values: `entry` the file inside the bundle to start, NULL for the one
 * its host.json names, and `arguments` the array of strings it is given.
 */
typedef struct runtime_entry_start_request {
    const char *cache;
    const cJSON *environment;
    const char *id;
    const char *version;
    const char *runtime;
    const char *entry;
    const cJSON *arguments;
} runtime_entry_start_request;

/**
 * How to start an entry, as processes.hold takes it: {executable, arguments, environment}. A native entry is its
 * executable, made runnable; a javascript one is run by the JavaScript runtime the cache records, else the one found
 * or acquired, the entry its first argument. The bundle's sizes are checked, never its hashes, as spisor-boot checks
 * them at every start. NULL with *refusal set, with the reinstall fix, when the version is not installed intact, the
 * entry lies outside its bundle or cannot be made runnable, or runtime is none of the catalog's runtimes, or as finding
 * the JavaScript runtime refuses; with a fix of its own when runtime is jvm, which no start exists for yet; NULL with
 * it NULL when memory runs out.
 */
cJSON *runtime_entry_start(const runtime_entry_start_request *request, cJSON **refusal);

/**
 * What hosts.layer_start starts: `loader_id`'s layers in `environment`, an object of variable names to values, in
 * `mode`, a runtime_hosts_layer_mode, and for an ACTION the `plugin`, `action` and `input` text it runs.
 */
typedef struct runtime_hosts_layer_start_request {
    const cJSON *environment;
    const char *loader_id;
    uint32_t mode;
    const char *plugin;
    const char *action;
    const char *input;
    const char *result;
} runtime_hosts_layer_start_request;

/**
 * hosts.layer_start: {layers: [{runtime, hostId, executable, arguments, environment}]}, as hosts.h says, each start as
 * runtime_entry_start answers it; NULL with *refusal set, or with it NULL when memory runs out.
 */
cJSON *runtime_hosts_layer_start(const runtime_hosts_layer_start_request *request, cJSON **refusal);

#endif
