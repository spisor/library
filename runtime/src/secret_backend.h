#ifndef RUNTIME_SECRET_BACKEND_H
#define RUNTIME_SECRET_BACKEND_H

#include <stddef.h>

/**
 * What an OS credential store answered. DONE is success for every call; ABSENT only for get and
 * delete, when no entry has that name; DECLINED when the user refused a prompt or the store is
 * locked and stays locked; UNAVAILABLE when this machine has no store (a Linux without Secret
 * Service, say); FAILED when the store was there and answered with an error.
 */
typedef enum runtime_secret_answer {
    RUNTIME_SECRET_DONE,
    RUNTIME_SECRET_ABSENT,
    RUNTIME_SECRET_DECLINED,
    RUNTIME_SECRET_UNAVAILABLE,
    RUNTIME_SECRET_FAILED
} runtime_secret_answer;

typedef struct runtime_secret_names {
    char **names;
    size_t count;
} runtime_secret_names;

/** Frees every name and the array, leaving names empty. */
void runtime_free_secret_names(runtime_secret_names *names);

/**
 * Whether a store is reachable at all, without prompting.
 *
 * @implNote This and the four calls below are the one seam every OS store implements. Exactly one
 * implementation is linked: secret_windows.c, secret_macos.c or secret_linux.c, chosen at build
 * time, or the tests' fake. An entry is the full name "spisor/<plugin id>/<name>" and a value is a
 * NUL terminated UTF-8 string of any length, so a store with a size limit splits and joins it.
 */
int runtime_secret_store_available(void);

/** On DONE, *value holds a copy freed with free; otherwise *value is NULL. */
runtime_secret_answer runtime_secret_store_get(const char *entry, char **value);

/** Creates or replaces the entry. Answers DONE (stored), DECLINED, UNAVAILABLE or FAILED. */
runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value);

runtime_secret_answer runtime_secret_store_delete(const char *entry);

/**
 * On DONE, names holds every logical entry whose name starts with prefix; free it with
 * runtime_free_secret_names. A store that splits a long value into extra entries keeps those under a
 * separate root, so they are never listed, and deleting a logical entry deletes them.
 */
runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names);

#endif
