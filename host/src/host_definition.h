#ifndef HOST_DEFINITION_H
#define HOST_DEFINITION_H

#include "cJSON.h"
#include "runtime/boot.h"
#include "runtime/cache.h"
#include "runtime/environment.h"
#include "runtime/hosts.h"
#include "runtime/install.h"
#include "runtime/registry.h"
#include "runtime/updates.h"
#include "spisor/status.h"

/**
 * @file
 * What the host keeps of a definition's typed answer, as the JSON it prints and reads: every call into runtime goes
 * through a definition, and these turn the answer into the host's own representation afterwards. An answer of NULL
 * with *refusal NULL means memory ran out.
 */

/** The {message, fix} refusal a definition filled; NULL for an empty refusal, as OUT_OF_MEMORY leaves it. */
cJSON *host_refusal_json(const spisor_refusal *refusal);

/** A registration as the record registry stores it; NULL when memory runs out or a value cannot be written. */
cJSON *host_registration_json(const runtime_registration *registration);

/** The hosts of the state place as {hosts: [{registration, orphan}], unreadable: [id]}; NULL with *refusal set. */
cJSON *host_listing_of(const char *state, cJSON **refusal);

/** What the cache holds as {bundles: {id: [version]}, libraries: {id: [version]}}; NULL with *refusal set. */
cJSON *host_installed_of(const char *cache_place, cJSON **refusal);

/** The cache's damaged versions as [{id, version, detail, fix, file?}]; NULL with *refusal set. */
cJSON *host_damaged_of(const char *cache_place, cJSON **refusal);

/** Removes host id as {reversed, drifted: [{sequence, kind, detail}], forgotten}; NULL with *refusal set. */
cJSON *host_removal_of(const char *state, const char *cache_place, const char *id, cJSON **refusal);

/** Sweeps the cache as {removed: [id@version]}; NULL with *refusal set. */
cJSON *host_sweep_of(const char *cache_place, const char *state, cJSON **refusal);

/**
 * Checks every registered host at `now`, an ISO time, and updates each method-installed application, as
 * {checks: {id: {checked, fetched, refused, removed}}, applications: {id: {operation, version, unrecorded?}},
 * refused: [{message, fix, id}]}; NULL with *refusal set.
 */
cJSON *host_update_run(const char *portable, const char *state, const char *cache_place, const char *now,
                       cJSON **refusal);

/** The string members of an object as variables, borrowing its keys and strings; freed, NULL out of memory. */
runtime_variable *host_variables_of(const cJSON *object, size_t *count);

/** The strings of an array, borrowed; freed, NULL out of memory. */
const char **host_texts_of(const cJSON *array, size_t *count);

/** Variables as an object of strings, the first of a name given twice kept; NULL when memory runs out. */
cJSON *host_variables_json(const runtime_variable *variables, size_t count);

/** What an ecosystem install is asked, the parts the host holds as JSON borrowed from it. */
typedef struct host_ecosystem_call {
    const char *portable;
    const char *state;
    const char *cache;
    /** An object of strings. */
    const cJSON *environment;
    /** An array of {kind: local, path} or {kind: https, url}. */
    const cJSON *sources;
    /** An object of an application id to its method. */
    const cJSON *choices;
    /** An array of plugin ids. */
    const cJSON *requested;
} host_ecosystem_call;

/**
 * Installs the ecosystem, as the report {installed, cached, unavailable, enabled, applications, loaders, missing,
 * javascript?}; NULL with *refusal set.
 */
cJSON *host_install_ecosystem(const host_ecosystem_call *call, cJSON **refusal);

/** Puts spisor-boot in the cache place; 1, or 0 with *refusal set. */
int host_place_boot(const char *cache_place, cJSON **refusal);

#endif
