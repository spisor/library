#ifndef SPISOR_TESTING_DEFINITION_CALLS_H
#define SPISOR_TESTING_DEFINITION_CALLS_H

/**
 * @file
 * Checks every operation of every definition a shared library exports, through the table cmake/definition_calls.cmake
 * generates from the headers, so an operation added to a header is checked without a test changing.
 */

#include <stddef.h>

#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One parameter between an operation's context and its refusal: a size-prefixed struct the caller owns. */
typedef struct spisor_definition_parameter {
    const char *type;
    size_t size;
    size_t shape;
    int is_answer;
    const size_t *count_offsets;
    const char *const *count_names;
    size_t count_field_count;
} spisor_definition_parameter;

/** Calls an operation of `definition` with one pointer per parameter, in the order its signature takes them. */
typedef spisor_status (*spisor_definition_call)(const void *definition, void *const *parameters,
                                                spisor_refusal *refusal);

typedef void (*spisor_definition_refusal_release)(const void *definition, spisor_refusal *refusal);

typedef struct spisor_definition_operation {
    const char *symbol;
    const char *name;
    spisor_definition_call call;
    spisor_definition_refusal_release release_refusal;
    const spisor_definition_parameter *parameters;
    size_t parameter_count;
} spisor_definition_operation;

/** The table the generated source defines. */
extern const spisor_definition_operation spisor_definition_operations[];
extern const size_t spisor_definition_operation_count;

/**
 * Calls every operation once per parameter, with that parameter's `size` one below its first shape and every other
 * parameter empty, and requires MALFORMED naming a short shape with every answer as it was.
 *
 * @return the number of failures, each printed.
 */
int spisor_check_short_shapes(const char *library_path);

/**
 * Calls every operation once per count field of each answer, with that count nonzero beside NULL storage and every
 * other parameter empty, and requires MALFORMED naming a non-empty answer with every answer as it was.
 *
 * @return the number of failures, each printed.
 */
int spisor_check_counted_answers(const char *library_path);

#ifdef __cplusplus
}
#endif

#endif
