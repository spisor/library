#ifndef RUNTIME_PUBLIC_IDENTITY_H
#define RUNTIME_PUBLIC_IDENTITY_H

/**
 * @file
 * runtime's `identity` service: a plugin's id, as `github:<number>[/<path>]`, `<resolver>@<host>:<number>[/<path>]` or
 * `local:<name>`; the repo it names on its host; and the set of copies of one repo that agree on a primary. A
 * size-prefixed struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the
 * call; what an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One id. First shape ends at `id`. */
typedef struct runtime_identity_id_argument {
    uint32_t size;
    const char *id;
} runtime_identity_id_argument;

/**
 * A valid id's parts: `resolver` always, `host` only for a host-qualified id, `number` (a digit string) for any id but
 * a local one, and `path` for an id with a path or a local id, whose name it is; each NULL when the id has none. First
 * shape ends at `storage`.
 */
typedef struct runtime_identity_parts {
    uint32_t size;
    const char *resolver;
    const char *host;
    const char *number;
    const char *path;
    void *storage;
} runtime_identity_parts;

/** The folder an id is kept in: the id with ":" as "+" and "/" as "=". First shape ends at `storage`. */
typedef struct runtime_identity_folder_answer {
    uint32_t size;
    const char *folder;
    void *storage;
} runtime_identity_folder_answer;

/**
 * A repo to look up. `last_known` is an owner/name a previous lookup saw, or NULL; `token` an access token sent to the
 * host the id names and to no other, or NULL; `api_base` replaces the host's API root, or NULL, and is refused unless
 * the library was built with its fetch test hooks, so only a test can aim a lookup. First shape ends at `api_base`.
 */
typedef struct runtime_identity_resolve_argument {
    uint32_t size;
    const char *id;
    const char *last_known;
    const char *token;
    const char *api_base;
} runtime_identity_resolve_argument;

/** The repo an id names: its name, its owner/name, and its default branch or NULL. First shape ends at `storage`. */
typedef struct runtime_identity_repository {
    uint32_t size;
    const char *name;
    const char *full_name;
    const char *default_branch;
    void *storage;
} runtime_identity_repository;

/**
 * One copy's also-at file, as a reader found it: `file` the file's parsed content, NULL when the copy has no file or
 * could not be reached, or `read_failed` 1 when reading it failed in any other way. An element of an array, so it has
 * no size.
 */
typedef struct runtime_identity_copy {
    const char *id;
    uint32_t read_failed;
    const spisor_value *file;
} runtime_identity_copy;

/** The repo `id` and the also-at files of it and of its copies, each id once. First shape ends at `copy_count`. */
typedef struct runtime_identity_agree_argument {
    uint32_t size;
    const char *id;
    const runtime_identity_copy *copies;
    size_t copy_count;
} runtime_identity_agree_argument;

/**
 * A repo whose copies to read, `api_base` as runtime_identity_resolve_argument has it. First shape ends at
 * `api_base`.
 */
typedef struct runtime_identity_also_at_argument {
    uint32_t size;
    const char *id;
    const char *api_base;
} runtime_identity_also_at_argument;

/**
 * The copies that agree: `ids` the starting id first and then each counted copy, `primary` the primary the starting
 * file names (the starting id when it names none), `primary_confirmed` 1 when a counted id is that primary,
 * `start_file_malformed` 1 when the starting file was not usable, and `start_file_unavailable` 1 when it could not be
 * read. First shape ends at `storage`.
 */
typedef struct runtime_identity_agreement {
    uint32_t size;
    const char *const *ids;
    size_t id_count;
    const char *primary;
    uint32_t primary_confirmed;
    uint32_t start_file_malformed;
    uint32_t start_file_unavailable;
    void *storage;
} runtime_identity_agreement;

/** The primary recorded at install and the one a new version comes from. First shape ends at `primary`. */
typedef struct runtime_identity_version_argument {
    uint32_t size;
    const char *recorded;
    const char *primary;
} runtime_identity_version_argument;

/**
 * Whether a new version's primary is accepted without asking: 1 when it is the recorded one, else 0, and then only the
 * user accepts it. It owns nothing. First shape ends at `accepted`.
 */
typedef struct runtime_identity_version_answer {
    uint32_t size;
    uint32_t accepted;
} runtime_identity_version_answer;

/**
 * The identity service. Every answer and refusal argument is required but the refusal, which may be NULL for none, and
 * a repo id is a valid id that is not local and names no folder. First shape ends at `release_refusal`.
 */
typedef struct runtime_identity_definition {
    uint32_t size;
    void *context;
    /**
     * Splits an id into its parts.
     *
     * @implNote The whitespace a local name refuses is ASCII whitespace only; a byte above 0x7F is allowed.
     * @return OK; NONE for an id outside the id grammar, the answer left empty; MALFORMED for no id; OUT_OF_MEMORY.
     */
    spisor_status (*parse)(void *context, const runtime_identity_id_argument *argument,
                           runtime_identity_parts *answer, spisor_refusal *refusal);
    /**
     * The folder an id is kept in.
     *
     * @return OK; MALFORMED for an id outside the grammar; REFUSED for a local id whose folder another id would share,
     * or that no portable folder name can carry; OUT_OF_MEMORY.
     */
    spisor_status (*folder)(void *context, const runtime_identity_id_argument *argument,
                            runtime_identity_folder_answer *answer, spisor_refusal *refusal);
    /**
     * Looks the repo up on its host by number, which survives a rename; a local id answers from its own text. Only
     * when the host cannot answer by number is `last_known` asked, and its answer is accepted only with the same
     * number.
     *
     * @return OK; MALFORMED for an id outside the grammar or a token that is not one to 512 printable ASCII characters
     * without a space; REFUSED when the host cannot be asked, answers no such repo, or `api_base` is set in a shipped
     * library; OUT_OF_MEMORY.
     */
    spisor_status (*resolve)(void *context, const runtime_identity_resolve_argument *argument,
                             runtime_identity_repository *answer, spisor_refusal *refusal);
    /**
     * Which copies agree, from files already read; it reads nothing. An id is counted only when the starting file
     * lists it and its own file lists the starting id back, and only ids the starting file lists are considered. A file
     * that is not a record of a repo id `primary` and at most 16 repo ids `ids` is malformed: on a copy that leaves the
     * copy out, and on the starting id it counts nothing beyond the id itself.
     *
     * @return OK; MALFORMED for an id that is not a repo id, a copy without an id or listed twice, or a file that is
     * not a well-formed value; REFUSED when counted copies name different primaries; OUT_OF_MEMORY.
     */
    spisor_status (*agree)(void *context, const runtime_identity_agree_argument *argument,
                           runtime_identity_agreement *answer, spisor_refusal *refusal);
    /**
     * Reads `.weivel/also-at.json` at the default branch of the repo and of each copy it lists, then answers as
     * `agree`. A copy that cannot be reached is left out.
     *
     * @return OK; MALFORMED for an id that is not a repo id; REFUSED when the repo itself cannot be resolved, or as
     * `agree` refuses; OUT_OF_MEMORY.
     */
    spisor_status (*also_at)(void *context, const runtime_identity_also_at_argument *argument,
                             runtime_identity_agreement *answer, spisor_refusal *refusal);
    /**
     * Whether a new version's primary is accepted without the user.
     *
     * @return OK; MALFORMED for a recorded or new primary that is not a repo id.
     */
    spisor_status (*accepts_version)(void *context, const runtime_identity_version_argument *argument,
                                     runtime_identity_version_answer *answer, spisor_refusal *refusal);
    void (*release_parts)(void *context, runtime_identity_parts *answer);
    void (*release_folder_answer)(void *context, runtime_identity_folder_answer *answer);
    void (*release_repository)(void *context, runtime_identity_repository *answer);
    void (*release_agreement)(void *context, runtime_identity_agreement *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_identity_definition;

/** The static runtime_identity_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_identity_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
