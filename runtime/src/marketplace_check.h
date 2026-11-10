#ifndef RUNTIME_MARKETPLACE_CHECK_H
#define RUNTIME_MARKETPLACE_CHECK_H

#include <stddef.h>

#include "cJSON.h"

/** The entry limit of marketplace.schema.json and marketplace-copy.schema.json. */
#define RUNTIME_MARKETPLACE_MAXIMUM_ENTRIES 10000

/** One value the checkers hold of a schema, named by its path of member names into that schema. */
typedef struct runtime_marketplace_held_value {
    const char *schema_file;
    const char *path;
    const char *value;
} runtime_marketplace_held_value;

/**
 * Every pattern, limit and required list the checkers restate by hand. A string schema value is held as its
 * text and any other as its compact JSON, so a test reads each schema and compares them all.
 */
extern const runtime_marketplace_held_value runtime_marketplace_held_values[];
extern const size_t runtime_marketplace_held_value_count;

/**
 * Whether list satisfies marketplace.schema.json; 0 with a sentence naming the first thing wrong written into
 * problem, of size bytes.
 *
 * @implNote The schemas' patterns are hand-written here as the id grammar, the import name grammar and the two
 * checks below, since the library carries no regular expression engine.
 */
int runtime_marketplace_check_list(const cJSON *list, char *problem, size_t size);

/** Whether copy satisfies marketplace-copy.schema.json, answering as runtime_marketplace_check_list does. */
int runtime_marketplace_check_copy(const cJSON *copy, char *problem, size_t size);

/** Whether text is an owner/name such as "spisor/runtime", the fullName pattern of the copy schema. */
int runtime_marketplace_is_full_name(const char *text);

/** Whether text is an RFC 3339 timestamp, the built pattern of the copy schema. */
int runtime_marketplace_is_built_time(const char *text);

#endif
