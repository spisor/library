#ifndef RUNTIME_CACHE_INSTALLED_H
#define RUNTIME_CACHE_INSTALLED_H

#include "cJSON.h"

/**
 * Each bundle id's versions whose install completed and whose files are all present, {id: [version]},
 * read from the folder names and markers under cache/bundles; ids with none are left out. NULL when
 * memory runs out.
 *
 * @implNote Files are not re-hashed here; a damaged version is found when cache.check verifies it.
 */
cJSON *runtime_cache_installed_bundles(const char *cache);

/** Each library id's versions whose install for platform completed, as runtime_cache_installed_bundles answers. */
cJSON *runtime_cache_installed_libraries(const char *cache, const char *platform);

/**
 * Each bundle id's versions carrying a readable completion marker, present files or not: what the sweep
 * weighs, so a version whose files a sweep half removed is still swept.
 */
cJSON *runtime_cache_marked_bundles(const char *cache);

/** Each library id's versions for platform carrying a readable completion marker, as runtime_cache_marked_bundles answers. */
cJSON *runtime_cache_marked_libraries(const char *cache, const char *platform);

/** Each platform key with any folder under a library version in the cache, sorted. NULL when memory runs out. */
cJSON *runtime_cache_library_platforms(const char *cache);

#endif
