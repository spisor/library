#ifndef RUNTIME_CACHE_REFERENCES_H
#define RUNTIME_CACHE_REFERENCES_H

#include "cJSON.h"

/**
 * Every id@version something still needs, as the member names of an object: what each host's
 * state/bundles/<host>.json names as run, next or rollback, what each live start's run record under
 * state/bundles/runs/<host>/ names, and what each boot hint cache/hosts/<host>.json names, its libraries
 * and its host bundle under the host id. NULL when memory runs out.
 *
 * @implNote The formats are plan three's, read as forebay/host wrote them. A file that is not the shape
 * it should be names nothing. A run record whose process is gone, or which stopped being stamped for
 * three of its intervals, is deleted as it is read, by the same liveness a background host's holders
 * are judged by.
 */
cJSON *runtime_cache_referenced(const char *cache, const char *state, double now);

#endif
