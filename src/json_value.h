#ifndef SPISOR_JSON_VALUE_H
#define SPISOR_JSON_VALUE_H

#include "cJSON.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Builds a tree from a cJSON tree into an empty one, every node and text copied into one allocation, so the tree
 * outlives `json`. For the JSON a library reads from its own files and a test reads from its vectors, never for a
 * value crossing a definition. The tree must come from spisor_parse_json_reporting, which refuses a string holding a
 * NUL: cJSON keeps no length, so a string with an escaped NUL from any other parse would be read short.
 *
 * @param tree the caller's empty tree. Required.
 * @return OK; MALFORMED, with `tree` left as it was, for a NULL or raw cJSON item, a string that is not well-formed
 * UTF-8, a key held twice in one object, nesting past SPISOR_VALUE_DEPTH_LIMIT, or a tree that is not empty;
 * OUT_OF_MEMORY with `tree` left as it was.
 */
spisor_status spisor_value_of_json(const cJSON *json, spisor_value_tree *tree);

/**
 * Builds a new cJSON tree from a value tree, for a value crossing a definition that the library keeps as JSON in its
 * own files. The cJSON tree is the caller's, freed with cJSON_Delete.
 *
 * @param json where the new tree goes, NULL whenever the answer is not OK. Required.
 * @return OK; MALFORMED for a value spisor_value_check refuses or a root of kind NONE, which JSON cannot hold;
 * OUT_OF_MEMORY.
 */
spisor_status spisor_json_of_value(const spisor_value *value, cJSON **json);

#ifdef __cplusplus
}
#endif

#endif
