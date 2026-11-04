#ifndef RUNTIME_CATALOG_SCHEMA_H
#define RUNTIME_CATALOG_SCHEMA_H

#include <stddef.h>

/**
 * What the catalog checker holds of catalog.schema.json, each value named by its JSON pointer into the
 * schema, so a test reads the schema file and compares every one; the checker reads these tables and
 * restates none of their values.
 */

/** A pattern, as the schema words it; the pointer ends in /pattern. */
typedef struct runtime_catalog_schema_text {
    const char *pointer;
    const char *value;
} runtime_catalog_schema_text;

/** A required array or an enum, NULL-terminated, in the schema's order; the pointer ends in /required or /enum. */
typedef struct runtime_catalog_schema_list {
    const char *pointer;
    const char *const *values;
} runtime_catalog_schema_list;

/** A minLength, minimum or minItems; the pointer ends in the keyword. */
typedef struct runtime_catalog_schema_bound {
    const char *pointer;
    double value;
} runtime_catalog_schema_bound;

typedef enum runtime_catalog_pattern_index {
    PATTERN_ITEM_ID,
    PATTERN_VERSION_KEY,
    PATTERN_LATEST,
    PATTERN_FILE_KEY,
    PATTERN_SHA256,
    PATTERN_BINARY,
    PATTERN_XDG_SUBDIR,
    PATTERN_CANDIDATE,
    PATTERN_PACKAGE_NAME,
    PATTERN_COUNT
} runtime_catalog_pattern_index;

typedef enum runtime_catalog_list_index {
    REQUIRED_ROOT,
    REQUIRED_ITEM,
    REQUIRED_VERSION_FILES,
    REQUIRED_VERSION_PLATFORMS,
    REQUIRED_PLATFORM,
    REQUIRED_FILE_REFERENCE,
    REQUIRED_APPLICATION,
    REQUIRED_HOME,
    REQUIRED_PACKAGE,
    REQUIRED_APPLICATION_RULE_IF,
    REQUIRED_APPLICATION_RULE_NOT,
    REQUIRED_RUNTIME_RULE_IF,
    REQUIRED_RUNTIME_RULE_THEN,
    ENUM_KIND,
    ENUM_RUNTIME,
    ENUM_APPLICATION_RULE_KINDS,
    LIST_COUNT
} runtime_catalog_list_index;

typedef enum runtime_catalog_bound_index {
    MIN_LENGTH_URL,
    MIN_LENGTH_INSTALL_METHOD,
    MIN_LENGTH_APPLICATION_ID,
    MIN_LENGTH_APPLICATION_LABEL,
    MIN_LENGTH_ENV_OVERRIDE,
    MIN_LENGTH_NATIVE_ENV,
    MINIMUM_SIZE,
    MIN_ITEMS_CANDIDATES,
    BOUND_COUNT
} runtime_catalog_bound_index;

typedef enum runtime_catalog_constant_index {
    CONSTANT_FORMAT,
    CONSTANT_RUNTIME_RULE_KIND,
    CONSTANT_COUNT
} runtime_catalog_constant_index;

/** Indexed by runtime_catalog_pattern_index. */
extern const runtime_catalog_schema_text runtime_catalog_schema_patterns[PATTERN_COUNT];

/** Indexed by runtime_catalog_list_index. */
extern const runtime_catalog_schema_list runtime_catalog_schema_lists[LIST_COUNT];

/** Indexed by runtime_catalog_bound_index. */
extern const runtime_catalog_schema_bound runtime_catalog_schema_bounds[BOUND_COUNT];

/** Each const as JSON text; indexed by runtime_catalog_constant_index. */
extern const runtime_catalog_schema_text runtime_catalog_schema_constants[CONSTANT_COUNT];

/** How many if-then rules the checker applies to an item, the length of the item's allOf. */
#define RUNTIME_CATALOG_ITEM_RULE_COUNT 2

#endif
