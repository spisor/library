#ifndef SPISOR_VALUE_EQUAL_H
#define SPISOR_VALUE_EQUAL_H

#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Whether two well-formed trees hold the same value: the same kinds, booleans, numbers and texts, lists item by item
 * in order, and records with the same keys each holding the same value, in any order, since each key is there once.
 * Test code only; 0 also when memory runs out.
 */
int spisor_value_equal(const spisor_value *left, const spisor_value *right);

#ifdef __cplusplus
}
#endif

#endif
