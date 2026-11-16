#ifndef RUNTIME_PUBLIC_SECRETS_H
#define RUNTIME_PUBLIC_SECRETS_H

/**
 * @file
 * runtime's `secrets` service: a plugin's secrets, kept in the OS credential store or, when the store declines or is
 * absent, in the private secrets file, each entry named `spisor/<plugin id>/<name>` in both. A size-prefixed struct is
 * read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or
 * a refusal points at is the provider's until the release for its kind.
 *
 * Unlike runtime's other services, these touch process-wide OS state: the user's credential store, which may prompt,
 * and whose entries outlive the process. On Linux the Secret Service backend also reads DBUS_SESSION_BUS_ADDRESS and
 * XDG_RUNTIME_DIR, and its first use loads libsecret with glib, gobject and gio into the process.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Where secrets live, as places' `resolve` answers them: `secrets_file` an absolute path, and `force_file_store` 1 to
 * skip the credential store so not one store call is made. First shape ends at `force_file_store`.
 */
typedef struct runtime_secret_places {
    uint32_t size;
    const char *secrets_file;
    uint32_t force_file_store;
} runtime_secret_places;

/** Where a set put the secret. Append-only: a published value keeps its number. */
typedef enum runtime_secret_location {
    RUNTIME_SECRET_LOCATION_STORE = 1,
    RUNTIME_SECRET_LOCATION_FILE = 2
} runtime_secret_location;

/** One entry: `plugin` a non-empty manifest id without a slash, `name` non-empty. First shape ends at `name`. */
typedef struct runtime_secrets_entry_argument {
    uint32_t size;
    const runtime_secret_places *places;
    const char *plugin;
    const char *name;
} runtime_secrets_entry_argument;

/** One entry and the value to store under it. First shape ends at `value`. */
typedef struct runtime_secrets_set_argument {
    uint32_t size;
    const runtime_secret_places *places;
    const char *plugin;
    const char *name;
    const char *value;
} runtime_secrets_set_argument;

/** Every secret of one plugin. First shape ends at `plugin`. */
typedef struct runtime_secrets_clear_argument {
    uint32_t size;
    const runtime_secret_places *places;
    const char *plugin;
} runtime_secrets_clear_argument;

/** The secrets of every plugin. First shape ends at `places`. */
typedef struct runtime_secrets_reconcile_argument {
    uint32_t size;
    const runtime_secret_places *places;
} runtime_secrets_reconcile_argument;

/** One secret's value, `length` bytes and a NUL; its bytes are overwritten when released. First shape ends at `storage`. */
typedef struct runtime_secrets_get_answer {
    uint32_t size;
    const char *value;
    size_t length;
    void *storage;
} runtime_secrets_get_answer;

/** Where a set put the secret, a runtime_secret_location. It owns nothing. First shape ends at `location`. */
typedef struct runtime_secrets_set_answer {
    uint32_t size;
    uint32_t location;
} runtime_secrets_set_answer;

/** How many secrets a reconcile moved from the file into the store. It owns nothing. First shape ends at `moved`. */
typedef struct runtime_secrets_reconcile_answer {
    uint32_t size;
    uint64_t moved;
} runtime_secrets_reconcile_answer;

/**
 * The secrets service. Every operation answers MALFORMED for places short of their first shape or without an absolute
 * secrets file, a plugin id that
 * is empty or holds a slash, or an empty name; REFUSED, with the refusal saying what to repair, when the secrets file
 * is not a readable JSON object, is a symbolic link (a secret is never written through one) or cannot be written, or
 * when the credential store declines or fails; and OUT_OF_MEMORY. First shape ends at `release_refusal`.
 *
 * @implNote `remove` and not `delete`, so the header stays includable from C++, where `delete` is a keyword.
 */
typedef struct runtime_secrets_definition {
    uint32_t size;
    void *context;
    /**
     * One secret, from the file, else from the store; it never writes to the store. A file entry is always the newer
     * copy, since one is written only while the store refuses and every later store write empties the file.
     *
     * @param answer the consumer's empty answer. Required.
     * @return OK; NONE when neither place holds it; REFUSED when neither holds it and the store declined or failed,
     * since the store may hold it.
     */
    spisor_status (*get)(void *context, const runtime_secrets_entry_argument *argument,
                         runtime_secrets_get_answer *answer, spisor_refusal *refusal);
    /**
     * Stores one secret. A value the store took is followed by every file-held secret, one at a time and each written
     * to the store before it leaves the file, so an interruption never loses one. A declined or absent store sends the
     * value to the file, and the set still succeeds.
     *
     * @param answer the consumer's answer. Required.
     * @return OK; REFUSED for a value over 2560000 bytes, before either place is touched, or when the file's older copy
     * of the entry cannot leave the file, since a later get would answer it.
     */
    spisor_status (*set)(void *context, const runtime_secrets_set_argument *argument,
                         runtime_secrets_set_answer *answer, spisor_refusal *refusal);
    /**
     * Removes one secret from both places. A store that declines or fails keeps its copy, and then the file's copy is
     * kept too, since removing the newer copy alone would make the store's older value readable again.
     *
     * @return OK, also when neither place held it.
     */
    spisor_status (*remove)(void *context, const runtime_secrets_entry_argument *argument, spisor_refusal *refusal);
    /** Removes every secret of one plugin from both places, as an uninstall does. */
    spisor_status (*clear)(void *context, const runtime_secrets_clear_argument *argument, spisor_refusal *refusal);
    /**
     * Moves every secret the file holds into the store when the store is available, as a host does once at start, so
     * a store installed since takes over what went to the file before it existed.
     *
     * @param answer the consumer's answer. Required.
     */
    spisor_status (*reconcile)(void *context, const runtime_secrets_reconcile_argument *argument,
                               runtime_secrets_reconcile_answer *answer, spisor_refusal *refusal);
    void (*release_get_answer)(void *context, runtime_secrets_get_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_secrets_definition;

/** The static runtime_secrets_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_secrets_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
