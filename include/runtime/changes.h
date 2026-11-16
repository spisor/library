#ifndef RUNTIME_PUBLIC_CHANGES_H
#define RUNTIME_PUBLIC_CHANGES_H

/**
 * @file
 * runtime's `changes` service: the changes a registered host makes outside its own folders, each recorded in the
 * host's registration before it is made, so every one can be reversed. A size-prefixed struct is read only up to its
 * `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at
 * is the provider's until the release for its kind. A place is an absolute directory, as places' `resolve` answers it.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a change makes. Append-only: a published value keeps its number. */
typedef enum runtime_change_kind {
    /** A file at `path` holding `content`, or a copy of the file `source`, made runnable when `executable` is 1. */
    RUNTIME_CHANGE_KIND_FILE = 1,
    /** A directory at `path`. */
    RUNTIME_CHANGE_KIND_DIRECTORY = 2,
    /**
     * The member at `key`, a path of `key_count` member names, in the JSON file `file` set to `value`; `create` 1
     * makes the file when it is missing.
     */
    RUNTIME_CHANGE_KIND_JSON_KEY = 3,
    /** The one line `line` in the shell profile `file`; `create` 1 makes the profile when it is missing. */
    RUNTIME_CHANGE_KIND_PROFILE_LINE = 4,
    /**
     * The directory `directory` on the user's PATH: on Windows in the user's Path value, elsewhere as a line in each
     * profile under `home`, with `shell` naming the login shell or NULL.
     */
    RUNTIME_CHANGE_KIND_PATH_ENTRY = 5
} runtime_change_kind;

/**
 * One change: `kind` a runtime_change_kind, and the fields that kind names, every other one NULL or 0. Paths are
 * absolute. `value` is data whose shape belongs to the file's owner. First shape ends at `shell`.
 */
typedef struct runtime_change {
    uint32_t size;
    uint32_t kind;
    const char *path;
    const char *content;
    uint32_t executable;
    const char *file;
    const char *const *key;
    size_t key_count;
    const spisor_value *value;
    uint32_t create;
    const char *line;
    const char *directory;
    const char *home;
    const char *source;
    const char *shell;
} runtime_change;

/** A change for the registered host `host`, recorded in the `state` place. First shape ends at `change`. */
typedef struct runtime_changes_apply_argument {
    uint32_t size;
    const char *state;
    const char *host;
    const runtime_change *change;
} runtime_changes_apply_argument;

/**
 * Whether the change was recorded: `recorded` 1 for a new record numbered `sequence`; 0 when nothing was recorded,
 * either because the disk already held what was asked and no host holds it, `sequence` then 0, or because this host
 * already holds the same change, numbered `sequence`. It owns nothing. First shape ends at `sequence`.
 */
typedef struct runtime_changes_apply_answer {
    uint32_t size;
    uint32_t recorded;
    uint64_t sequence;
} runtime_changes_apply_answer;

/** Every change the host `host` recorded in the `state` place. First shape ends at `host`. */
typedef struct runtime_changes_reverse_argument {
    uint32_t size;
    const char *state;
    const char *host;
} runtime_changes_reverse_argument;

/**
 * A reversed change whose target no longer held what the host made, left as found: its sequence, its kind as the
 * record names it, and what drifted. An element of an array, so it has no size.
 */
typedef struct runtime_change_drift {
    uint64_t sequence;
    const char *kind;
    const char *detail;
} runtime_change_drift;

/** How many changes were reversed, and those of them that had drifted. First shape ends at `storage`. */
typedef struct runtime_changes_reverse_answer {
    uint32_t size;
    uint64_t reversed;
    const runtime_change_drift *drifted;
    size_t drifted_count;
    void *storage;
} runtime_changes_reverse_answer;

/**
 * The changes service. The answer arguments are required and the refusal may be NULL for none. First shape ends at
 * `release_refusal`.
 */
typedef struct runtime_changes_definition {
    uint32_t size;
    void *context;
    /**
     * Records the change as pending, makes it, then marks it applied, so an interruption leaves a record of a change
     * that may have happened, never a change with no record; the next apply or reverse settles it against the disk.
     * A file this host already holds that is asked for with new content is rewritten under its record.
     *
     * @return OK; MALFORMED for a place that is not absolute, no host, or a change without the fields its kind needs;
     * REFUSED when the host is not registered, its registration is not readable, the change cannot be made or its
     * record cannot be written, nothing then changed or recorded; OUT_OF_MEMORY, nothing changed either, though once
     * the pending record was written its number is not handed out again.
     */
    spisor_status (*apply)(void *context, const runtime_changes_apply_argument *argument,
                           runtime_changes_apply_answer *answer, spisor_refusal *refusal);
    /**
     * Reverses the host's changes, newest first, each under the registry lock and stored as reversed before the next,
     * so an interruption leaves every change either reversed or still recorded. A change another host also holds is
     * left in place.
     *
     * @return OK, also for a host with no registration; MALFORMED for a place that is not absolute or no host; REFUSED
     * when a change cannot be reversed or the registration cannot be read or written, the newer ones staying
     * reversed; OUT_OF_MEMORY, the change it was reversing left as recorded.
     */
    spisor_status (*reverse)(void *context, const runtime_changes_reverse_argument *argument,
                             runtime_changes_reverse_answer *answer, spisor_refusal *refusal);
    void (*release_reverse_answer)(void *context, runtime_changes_reverse_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_changes_definition;

/** The static runtime_changes_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_changes_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
