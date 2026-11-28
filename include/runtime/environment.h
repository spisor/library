#ifndef RUNTIME_PUBLIC_ENVIRONMENT_H
#define RUNTIME_PUBLIC_ENVIRONMENT_H

/**
 * @file
 * An environment as runtime's services take one: an array of variables, each name given once, as a process environment
 * holds it. Every service refuses as MALFORMED a variable without a name or a value, a name that is empty or holds =,
 * and a name given twice, before it allocates anything. A name that begins with =, as Windows names a drive's current
 * directory, is no variable: every service leaves it out, as runtime's own read of the process environment does.
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
