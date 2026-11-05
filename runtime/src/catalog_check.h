#ifndef RUNTIME_CATALOG_CHECK_H
#define RUNTIME_CATALOG_CHECK_H

#include <stddef.h>

#include "cJSON.h"
#include "catalog_read.h"

typedef struct runtime_catalog_checker runtime_catalog_checker;

/** One member of a schema object's properties, and how the checker checks its value. */
typedef struct runtime_catalog_property_rule {
    const char *name;
    int (*rule)(runtime_catalog_checker *check, const cJSON *value);
} runtime_catalog_property_rule;

/** The properties the checker visits for one schema object, in schema order, ending with a NULL name. */
typedef struct runtime_catalog_schema_properties {
    const char *pointer;
    const runtime_catalog_property_rule *rules;
} runtime_catalog_schema_properties;

/** Every properties object of catalog.schema.json outside an if clause; the pointer ends in /properties. */
extern const runtime_catalog_schema_properties runtime_catalog_schema_property_sets[];
extern const size_t runtime_catalog_schema_property_set_count;

/**
 * Whether document satisfies catalog.schema.json; when it does not, the refusal ajv's first error gives.
 *
 * @implNote A hand-written check, not a schema interpreter: it visits keywords in the order ajv's
 * compiled draft-07 validator does (keywords of any type, then type, then the type's own keywords:
 * required, propertyNames, additionalProperties, properties in schema order), so of two faults the same
 * one is named. An anyOf that fails is reported as the anyOf itself, an if as its then's first fault, and
 * propertyNames as the pattern, with the key appended raw. Every value it holds of the schema is in
 * catalog_schema.h or the property sets above, which a test compares against the schema file.
 */
int runtime_catalog_check_schema(const cJSON *document, runtime_catalog_refusal *refusal);

#endif
