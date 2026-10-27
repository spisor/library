#ifndef RUNTIME_MERGE_H
#define RUNTIME_MERGE_H

#include "cJSON.h"

/**
 * The defaults with the file applied over them, as a new tree freed with cJSON_Delete, or NULL when
 * memory runs out. Objects merge key by key and the file wins at every leaf; any other file value,
 * an array or null included, replaces the default whole. Either argument may be NULL.
 */
cJSON *runtime_merge(const cJSON *defaults, const cJSON *file);

/**
 * The value a dot path reaches through objects, and through arrays by index as JavaScript property
 * access does, or NULL when absent. The result belongs to tree.
 *
 * @implNote An array is indexed only by a canonical non-negative integer below its length, so "01",
 * "-1" and "1.0" are absent, and so is "length", which is not emulated. On an object every key is
 * an ordinary property, "length" included.
 */
const cJSON *runtime_walk(const cJSON *tree, const char *key);

#endif
