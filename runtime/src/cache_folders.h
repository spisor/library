#ifndef RUNTIME_CACHE_FOLDERS_H
#define RUNTIME_CACHE_FOLDERS_H

#include "cJSON.h"

/**
 * The names of the real directories directly inside the path joined from segments, sorted, as an array
 * of strings; empty when it cannot be read, NULL when memory runs out. A link is never listed.
 */
cJSON *runtime_cache_folder_names(const char *const *segments, size_t count);

/** The path segments joined for this platform, freed with free; NULL when memory runs out. */
char *runtime_cache_joined(const char *const *segments, size_t count);

#endif
