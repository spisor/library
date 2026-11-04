#ifndef RUNTIME_CATALOG_CHECK_H
#define RUNTIME_CATALOG_CHECK_H

#include <stddef.h>

#include "cJSON.h"
#include "catalog_read.h"

/** A value the checker holds as catalog.schema.json does, named by its JSON pointer into the schema. */
typedef struct runtime_catalog_schema_value {
    const char *pointer;
    const char *value;
} runtime_catalog_schema_value;

/** Every pattern the checker tests, as the schema words it, so a test can compare the two. */
extern const runtime_catalog_schema_value runtime_catalog_schema_patterns[];
extern const size_t runtime_catalog_schema_pattern_count;

/** Every enum value the checker accepts, one entry per value, pointing at the enum it belongs to. */
extern const runtime_catalog_schema_value runtime_catalog_schema_enum_values[];
extern const size_t runtime_catalog_schema_enum_value_count;

/**
 * Whether document satisfies catalog.schema.json; when it does not, the refusal ajv's first error gives,
 * worded as forebay/host's readCatalog worded it.
 *
 * @implNote A hand-written check, not a schema interpreter: it visits keywords in the order ajv's
 * compiled draft-07 validator does (keywords of any type, then type, then the type's own keywords:
 * required, propertyNames, additionalProperties, properties in schema order), so of two faults the same
 * one is named. An anyOf that fails is reported as the anyOf itself, an if as its then's first fault, and
 * propertyNames as the pattern, with the key appended raw. Pinned by test/fixtures/catalog.
 */
int runtime_catalog_check_schema(const cJSON *document, runtime_catalog_refusal *refusal);

#endif
