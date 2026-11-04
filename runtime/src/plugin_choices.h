#ifndef RUNTIME_PLUGIN_CHOICES_H
#define RUNTIME_PLUGIN_CHOICES_H

#include "cJSON.h"

/**
 * What the merged ecosystem settings choose under plugins.<id>, every id in sorted order: the
 * candidates a start runs, [{id, pin?}], being each id whose enabled is true, then each host default
 * (a value of defaults, an object of binding to plugin id) that the settings neither enable nor
 * disable, each held to its plugins.<id>.pin. NULL when memory runs out.
 *
 * @implNote A host default may start too, so it is resolved with the enabled set and honours a pin set
 * on a plugin that is not enabled; one the settings set enabled to false never starts.
 */
cJSON *runtime_plugin_candidates(const cJSON *settings, const cJSON *defaults);

/**
 * ids, an array of strings, as candidates [{id, pin?}], each held to its plugins.<id>.pin, in order and
 * once each. NULL when memory runs out.
 */
cJSON *runtime_pinned_candidates(const cJSON *ids, const cJSON *settings);

#endif
