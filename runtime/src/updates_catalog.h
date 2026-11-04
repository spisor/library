#ifndef RUNTIME_UPDATES_CATALOG_H
#define RUNTIME_UPDATES_CATALOG_H

#include "cJSON.h"

/**
 * The catalog sources settings name, as catalog.sources answers them. Like every call here, NULL with
 * *refusal set to the service's own {message, fix} when it refuses, or with *refusal untouched when memory
 * runs out.
 */
cJSON *runtime_updates_sources(const cJSON *settings, cJSON **refusal);

/** Every source's current index merged, as catalog.current answers it, with no request. */
cJSON *runtime_updates_current_catalog(const cJSON *sources, const char *cache, cJSON **refusal);

/** catalog.resolve's {run, fetch} for candidates of kind against the installed versions. */
cJSON *runtime_updates_resolve(const cJSON *catalog, const cJSON *candidates, const cJSON *installed, const char *kind,
                               cJSON **refusal);

/** One source checked as catalog.check answers it, {kind, detail?}, the catalog it read left out. */
cJSON *runtime_updates_check_source(const cJSON *source, const char *cache, const char *now, double interval_hours,
                                    cJSON **refusal);

/** The item id lists, or NULL. */
const cJSON *runtime_updates_item(const cJSON *catalog, const char *id);

/** Whether the item id lists version. */
int runtime_updates_lists(const cJSON *catalog, const char *id, const char *version);

#endif
