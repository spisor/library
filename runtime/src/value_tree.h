#ifndef RUNTIME_VALUE_TREE_H
#define RUNTIME_VALUE_TREE_H

#include "cJSON.h"
#include "spisor/value.h"

/**
 * The cJSON tree of a value a caller handed in, for the settings file it is written to; NULL with *malformed set when
 * the value is no value (a kind it does not know, NONE, a NULL text or array, a number that is not finite), or with it
 * clear when memory runs out.
 */
cJSON *runtime_tree_of_value(const spisor_value *value, int *malformed);

/** What an answered value points at: the tree its texts live in, and the one block of its nodes. */
typedef struct runtime_value_storage {
    cJSON *tree;
    void *nodes;
} runtime_value_storage;

/**
 * Fills value over tree, which it takes: texts point into the tree, and every node lives in one block. Answers the
 * storage to free with runtime_value_storage_free, or NULL when memory runs out, with the tree freed.
 */
runtime_value_storage *runtime_value_of_tree(cJSON *tree, spisor_value *value);

void runtime_value_storage_free(runtime_value_storage *storage);

#endif
