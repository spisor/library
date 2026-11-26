#ifndef RUNTIME_PUBLIC_IDENTITY_H
#define RUNTIME_PUBLIC_IDENTITY_H

/**
 * @file
 * runtime's `identity` service: a plugin's id, as `git:<root commit>[/<path>]` or `local:<name>`, read by naula; and
 * the homes that speak for a repo, which naula confirms from the places a consumer recorded for it (`at`), such as
 * `github:<number>`, `gitea@git.example.com:<number>` or `web:example.org`. A size-prefixed struct is read only up to
 * its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal
 * points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One id. First shape ends at `id`. */
typedef struct runtime_identity_id_argument {
    uint32_t size;
    const char *id;
} runtime_identity_id_argument;

/**
 * A valid id's parts: `kind`, "git" or "local"; `commit`, the root commit's hex of a git id; `name`, a local id's
 * name; and `path`, the folders below the repo joined by "/". Each NULL when the id has none. First shape ends at
 * `storage`.
 */
typedef struct runtime_identity_parts {
    uint32_t size;
    const char *kind;
    const char *commit;
    const char *name;
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
 * A repo to resolve: its repo `id` and the `at` places the consumer recorded for it, at least one. `cache` is the
 * absolute cache place naula keeps its confirmations in, under `identities`, or NULL to keep none. `token` is an access
 * token sent only to `token_host`, which names the host it belongs to (github.com for GitHub), or both NULL.
 * `accept_primary` is the place the user accepted as the repo's new primary, or NULL; `accept_number` the disputed
 * numbers the user accepted, each a dispute's `accept` text, joined by commas, or NULL. First shape ends at
 * `accept_number`.
 */
typedef struct runtime_identity_resolve_argument {
    uint32_t size;
    const char *id;
    const char *const *at;
    size_t at_count;
    const char *cache;
    const char *token;
    const char *token_host;
    const char *accept_primary;
    const char *accept_number;
} runtime_identity_resolve_argument;

/**
 * An account whose number this repo's records disagree on: `account` as `<provider>[@host]:<account>`,
 * `recorded_number` the place a record confirmed it at, `offered_number` the place it answers now, and `accept`, the
 * text accept_number takes to accept the offered number for this account alone. An element of an array, so it has no
 * size.
 */
typedef struct runtime_identity_dispute {
    const char *account;
    const char *recorded_number;
    const char *offered_number;
    const char *accept;
} runtime_identity_dispute;

/**
 * The homes that speak for a repo. `state` is "confirmed", "stale" (answered from the cache, since a home could not
 * be heard) or "domain-only"; `homes` are those homes as places, in the confirmed homes' own order with `at` breaking
 * a tie only between places that do not list each other, and `primary` the one new versions come from; `full_name`
 * (account/name), `name` and `default_branch` are the primary's, NULL when it is a domain or the answer is stale.
 *
 * When the repo's primary changed and the user has not accepted it, `needs_user_acceptance` is 1, `recorded_primary`
 * names the primary confirmed before, `primary` the new one, and nothing else is set. When records of this repo that
 * the consumer's `at` grew from (its lineage, read when that `at` has no record of its own) hold different numbers for
 * an account, nothing local can tell a deleted and re-registered account from a genuine change, so the user is asked:
 * `needs_number_acceptance` is 1, `disputes` lists every such account once, and nothing else is set. A refusal that is
 * neither waits for no acceptance and is answered as a refusal. First shape ends at `storage`.
 */
typedef struct runtime_identity_resolution {
    uint32_t size;
    const char *state;
    const char *const *homes;
    size_t home_count;
    const char *primary;
    const char *full_name;
    const char *name;
    const char *default_branch;
    uint32_t needs_user_acceptance;
    const char *recorded_primary;
    uint32_t needs_number_acceptance;
    const runtime_identity_dispute *disputes;
    size_t dispute_count;
    void *storage;
} runtime_identity_resolution;

/**
 * The identity service. Every answer and refusal argument is required but the refusal, which may be NULL for none, and
 * a repo id is a git id that names no folder. First shape ends at `release_refusal`.
 */
typedef struct runtime_identity_definition {
    uint32_t size;
    void *context;
    /**
     * Splits an id into its parts.
     *
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
     * Resolves a repo through naula: from `at`, never from a name, to every home linked both ways to the place of `at`
     * that confirms and comes first in the confirmed homes' own order, so a rename, a transfer, a fork or a copy at an
     * old name cannot redirect it. A provider word with no provider is refused with the fix to install a plugin that
     * provides a resolver.
     *
     * @return OK, also when the user's acceptance is needed; MALFORMED for an id that is not a repo id, no `at`, a
     * place that is not one, an accepted primary or number that is not one, a relative cache, or a token that is not one to 512 printable ASCII characters without a
     * space or that has no token host; REFUSED with naula's code and fix when nothing confirms the id; OUT_OF_MEMORY.
     */
    spisor_status (*resolve)(void *context, const runtime_identity_resolve_argument *argument,
                             runtime_identity_resolution *answer, spisor_refusal *refusal);
    void (*release_parts)(void *context, runtime_identity_parts *answer);
    void (*release_folder_answer)(void *context, runtime_identity_folder_answer *answer);
    void (*release_resolution)(void *context, runtime_identity_resolution *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_identity_definition;

/** The static runtime_identity_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_identity_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
