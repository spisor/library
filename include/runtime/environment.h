#ifndef RUNTIME_PUBLIC_ENVIRONMENT_H
#define RUNTIME_PUBLIC_ENVIRONMENT_H

/**
 * @file
 * An environment as runtime's services take one: an array of variables, read in order, the first of a name answering
 * for it, as a process environment holds each name once.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** One variable: a name and its value, both NUL-terminated UTF-8. An element of an array, so it has no size. */
typedef struct runtime_variable {
    const char *name;
    const char *value;
} runtime_variable;

#ifdef __cplusplus
}
#endif

#endif
