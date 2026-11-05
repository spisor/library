#ifndef RUNTIME_CACHE_LOCATE_H
#define RUNTIME_CACHE_LOCATE_H

#include "cJSON.h"

/**
 * Where each plugin of plugins, an id-to-version object, is installed in cache, as cache.locate answers
 * it: {located: [{id, version, directory, manifest, entry?}], problems: [{id, version, detail, fix, file?}]},
 * each in id order, entry being left out when plugin.json names none that is there. A version with no
 * completion marker or no plugin.json is in neither. NULL when memory runs out.
 *
 * @implNote Checks sizes only, as every start does: hashing is for install, update and status.
 */
cJSON *runtime_cache_locate_plugins(const char *cache, const cJSON *plugins);

#endif
