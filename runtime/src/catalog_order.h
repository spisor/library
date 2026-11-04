#ifndef RUNTIME_CATALOG_ORDER_H
#define RUNTIME_CATALOG_ORDER_H

#include <stddef.h>

#include "cJSON.h"

/**
 * The members of object in the order JavaScript enumerates them: array-index keys ascending, then the
 * rest as the document lists them. Freed with free; NULL with *count 0 for an empty object, and NULL
 * with *count nonzero when memory runs out.
 *
 * @implNote The catalog's rules were written against JavaScript objects, so which of two faulty
 * members is reported, and which of two items wins a merge, follows this order rather than cJSON's.
 */
const cJSON **runtime_catalog_ordered_members(const cJSON *object, size_t *count);

#endif
