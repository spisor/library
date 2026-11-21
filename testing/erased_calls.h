#ifndef SPISOR_TESTING_ERASED_CALLS_H
#define SPISOR_TESTING_ERASED_CALLS_H

/**
 * @file
 * Marks a function that calls a library's function through a type other than the one it was defined with: an
 * operation with every parameter erased to `void *`, as a binding's FFI calls it, or a definition getter answering
 * its own struct through `const void *(*)(void)`.
 *
 * @implNote Clang's -fsanitize=function reports each such call, though every ABI passes the two types the same.
 */

#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define SPISOR_CALLS_THROUGH_ERASED_TYPES __attribute__((no_sanitize("function")))
#endif
#endif
#ifndef SPISOR_CALLS_THROUGH_ERASED_TYPES
#define SPISOR_CALLS_THROUGH_ERASED_TYPES
#endif

#endif
