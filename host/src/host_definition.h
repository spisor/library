#ifndef HOST_DEFINITION_H
#define HOST_DEFINITION_H

#include "cJSON.h"
#include "runtime/cache.h"
#include "runtime/hosts.h"
#include "runtime/registry.h"
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

#endif
