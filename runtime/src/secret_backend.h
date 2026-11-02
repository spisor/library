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

/**
 * The most bytes of UTF-8 a secret's value may hold, in the store and in the file alike, on every OS:
 * 1000 Windows credential blobs of CRED_MAX_CREDENTIAL_BLOB_SIZE (2560) bytes. secrets.c refuses a
 * longer value before either place is touched, so a secret set on one OS can be set on every other.
 */
#define RUNTIME_SECRET_VALUE_CAP_BYTES 2560000u

/** Frees every name and the array, leaving names empty. */
void runtime_free_secret_names(runtime_secret_names *names);

/**
 * Whether a store is reachable at all, without prompting.
 *
 * @implNote This and the four calls below are the one seam every OS store implements. Exactly one
 * implementation is linked: secret_windows.c, secret_macos.c or secret_linux.c, chosen at build
 * time, or the tests' fake. An entry is the full name "spisor/<plugin id>/<name>" and a value is a
 * NUL terminated UTF-8 string of at most RUNTIME_SECRET_VALUE_CAP_BYTES, which a store with a smaller
 * size limit splits and joins.
 */
int runtime_secret_store_available(void);

/** On DONE, *value holds a copy freed with free; otherwise *value is NULL. */
runtime_secret_answer runtime_secret_store_get(const char *entry, char **value);

/**
 * Creates or replaces the entry. Answers DONE (stored), DECLINED, UNAVAILABLE or FAILED. A failed set
 * leaves the previous value readable. The Windows store also refuses, with FAILED, a value over
 * RUNTIME_SECRET_VALUE_CAP_BYTES, which is the most its chunks can carry.
 */
runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value);

runtime_secret_answer runtime_secret_store_delete(const char *entry);

/**
 * On DONE, names holds every logical entry whose name starts with prefix; free it with
 * runtime_free_secret_names. A store that splits a long value into extra entries keeps those under a
 * separate root, so they are never listed, and deleting a logical entry deletes them.
 */
runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names);

#endif
