#ifndef SPISOR_TESTING_DESCRIBED_CHECKS_H
#define SPISOR_TESTING_DESCRIBED_CHECKS_H

/**
 * @file
 * Checks every operation of a definition through the description it answers (`weivel/description.h`), so an operation
 * added to a header is checked without a test changing, in any library whose definitions describe themselves.
 *
 * Each check calls an operation answering a status once per change it makes to otherwise empty parameters: every
 * size-prefixed struct its full `size` and zero beyond it, a handle or definition answer a NULL slot, a handle, text or
 * unsized struct input NULL, and an empty refusal. Each call must answer MALFORMED with a refusal whose `detail` names
 * what was changed, and leave every answer byte as it was.
 *
 * So a provider checks shapes before required inputs, which is spisor's rule for every definition: first the refusal,
 * then the `size` and emptiness of each argument and answer, and only then a required input left NULL. A provider that
 * refuses the NULL input first answers a detail naming that input, and the check fails it.
 *
 * An operation taking a parameter no rule here reads, an answer or refusal that carries no `size`, or a refusal its
 * description names no release for, is a failure rather than unchecked; an input struct needs no `size`.
 */

#include <stddef.h>

#include <weivel/description.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a check did to one definition, or to several summed: the calls it made and the failures, each printed. */
typedef struct spisor_check_tally {
    size_t calls;
    size_t failures;
} spisor_check_tally;

/** A definition as a library answers it, beside the description of it. */
typedef struct spisor_described_definition {
    const weivel_standard_description *description;
    const void *definition;
} spisor_described_definition;

typedef void (*spisor_described_check)(const weivel_standard_description *description, const void *definition,
                                       spisor_check_tally *tally);

/**
 * Calls every operation once per size-prefixed struct it takes, argument or answer, with that struct's `size` one
 * below its first shape, and requires MALFORMED naming a short shape. A description yielding no such call is a
 * failure, since it would check nothing.
 */
void spisor_check_short_shapes(const weivel_standard_description *description, const void *definition,
                               spisor_check_tally *tally);

/**
 * Calls every operation once per count of each answer it takes, a SIZE member some array, texts or bytes member of
 * the answer names as its `count`, at any depth of structs the answer holds by value, with that count nonzero beside
 * NULL storage, and requires MALFORMED naming a non-empty answer.
 */
void spisor_check_counted_answers(const weivel_standard_description *description, const void *definition,
                                  spisor_check_tally *tally);

/**
 * Runs `check` over each definition after the standard's own check of its description.
 *
 * @return the failures, plus one when there is no definition or `check` made no call at all.
 */
size_t spisor_check_described(const spisor_described_definition *definitions, size_t definition_count,
                              spisor_described_check check);

/**
 * Loads the shared library at `library_path` as a consumer would and, for every service its declaration at
 * `declaration_path` (`library.json`) gives a definition function, takes the description the service declares and the
 * definition the function answers, and runs spisor_check_described over all of them. A service with a definition and
 * no description, or a symbol the library does not export, is a failure.
 *
 * @return the failures, each printed.
 */
size_t spisor_check_library(const char *library_path, const char *declaration_path, spisor_described_check check);

#ifdef __cplusplus
}
#endif

#endif
