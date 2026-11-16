#ifndef RUNTIME_PUBLIC_MARKETPLACE_H
#define RUNTIME_PUBLIC_MARKETPLACE_H

/**
 * @file
 * runtime's `marketplace` service: marketplace lists, the copies built from them, where the copies come from, and the
 * name a user installs by. A size-prefixed struct is read only up to its `size`. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the provider's until the release
 * for its kind.
 *
 * A marketplace list and a copy are documents their owner writes, checked against the published
 * marketplace.schema.json and marketplace-copy.schema.json, so each crosses as one spisor_value record; so do the
 * merged settings sources are read from.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A list to build a copy of: `built` the RFC 3339 time the copy states, from the caller so the same inputs build the
 * same copy; `previous` the copy this build replaces, or NULL; `token` an access token sent only to `token_host`, the
 * one host the list's ids name, both NULL or both set; `api_base` replaces each host's API root and is refused unless
 * the library was built with its fetch test hooks. First shape ends at `api_base`.
 */
typedef struct runtime_marketplace_build_argument {
    uint32_t size;
    const spisor_value *list;
    const char *built;
    const spisor_value *previous;
    const char *token;
    const char *token_host;
    const char *api_base;
} runtime_marketplace_build_argument;

/** An entry left out of a copy, and why. An element of an array, so it has no size. */
typedef struct runtime_marketplace_skipped_entry {
    const char *id;
    const char *reason;
} runtime_marketplace_skipped_entry;

/** The copy built, and the entries left out of it. First shape ends at `storage`. */
typedef struct runtime_marketplace_build_answer {
    uint32_t size;
    const spisor_value *copy;
    const runtime_marketplace_skipped_entry *skipped;
    size_t skipped_count;
    void *storage;
} runtime_marketplace_build_answer;

/** The merged settings, whose `marketplaces` member lists the sources. First shape ends at `settings`. */
typedef struct runtime_marketplace_sources_argument {
    uint32_t size;
    const spisor_value *settings;
} runtime_marketplace_sources_argument;

/** Where a source's copy comes from. Append-only: a published value keeps its number. */
typedef enum runtime_marketplace_source_kind {
    /** `url`, a file: or https: url this library reads. */
    RUNTIME_MARKETPLACE_SOURCE_URL = 1,
    /** `provider`, a plugin the host asks for the copy, which this library never fetches. */
    RUNTIME_MARKETPLACE_SOURCE_PROVIDER = 2
} runtime_marketplace_source_kind;

/**
 * One source: its id, an import name, and per its runtime_marketplace_source_kind either `url` or `provider`, the
 * other NULL. An element of an array, so it has no size.
 */
typedef struct runtime_marketplace_source {
    const char *id;
    uint32_t kind;
    const char *url;
    const char *provider;
} runtime_marketplace_source;

/** A source the settings list that was left out: its index in the list, and why. An element of an array. */
typedef struct runtime_marketplace_skipped_source {
    size_t index;
    const char *detail;
} runtime_marketplace_skipped_source;

/** The usable sources in order, and the ones left out. First shape ends at `storage`. */
typedef struct runtime_marketplace_sources_answer {
    uint32_t size;
    const runtime_marketplace_source *sources;
    size_t source_count;
    const runtime_marketplace_skipped_source *skipped;
    size_t skipped_count;
    void *storage;
} runtime_marketplace_sources_answer;

/**
 * One source's copy to read: the source's `id` and `url`, and either `text`, the body the host already fetched, or
 * `destination`, an absolute path that does not exist yet, which the url is fetched into and which is removed once
 * read. First shape ends at `destination`.
 */
typedef struct runtime_marketplace_read_argument {
    uint32_t size;
    const char *id;
    const char *url;
    const char *text;
    const char *destination;
} runtime_marketplace_read_argument;

/** A copy as read and checked. First shape ends at `storage`. */
typedef struct runtime_marketplace_copy_answer {
    uint32_t size;
    const spisor_value *copy;
    void *storage;
} runtime_marketplace_copy_answer;

/** One source's copy: the source's id and the copy. An element of an array, so it has no size. */
typedef struct runtime_marketplace_copy {
    const char *source;
    const spisor_value *copy;
} runtime_marketplace_copy;

/**
 * The copies to search, in order, and `name`, a bare name searched in every copy, or marketplace/name searched in that
 * marketplace's copy only. First shape ends at `name`.
 */
typedef struct runtime_marketplace_lookup_argument {
    uint32_t size;
    const runtime_marketplace_copy *copies;
    size_t copy_count;
    const char *name;
} runtime_marketplace_lookup_argument;

/** What a lookup found. Append-only: a published value keeps its number. */
typedef enum runtime_marketplace_match {
    /** One entry: `id`, first found in the source `marketplace`. */
    RUNTIME_MARKETPLACE_FOUND = 1,
    /** A bare name under more than one id: `ambiguous` lists every match as the marketplace/name that settles it. */
    RUNTIME_MARKETPLACE_AMBIGUOUS = 2
} runtime_marketplace_match;

/** What a name means, per `match`, a runtime_marketplace_match. First shape ends at `storage`. */
typedef struct runtime_marketplace_lookup_answer {
    uint32_t size;
    uint32_t match;
    const char *id;
    const char *marketplace;
    const char *const *ambiguous;
    size_t ambiguous_count;
    void *storage;
} runtime_marketplace_lookup_answer;

/**
 * The id a name was given: the `state` place holding the record; `marketplace` the source it came from, or NULL to
 * remember the bare name; `accept` the id the user accepted in place of the remembered one, or NULL; and `check_only` 1
 * to answer without writing. First shape ends at `check_only`.
 */
typedef struct runtime_marketplace_remember_argument {
    uint32_t size;
    const char *state;
    const char *marketplace;
    const char *name;
    const char *id;
    const char *accept;
    uint32_t check_only;
} runtime_marketplace_remember_argument;

/** What remembering found. Append-only: a published value keeps its number. */
typedef enum runtime_marketplace_memory {
    /** The id is the one on record now, recorded when the host had none or the user accepted it. */
    RUNTIME_MARKETPLACE_REMEMBERED = 1,
    /** A check only: recording would have remembered it. */
    RUNTIME_MARKETPLACE_SETTLED = 2,
    /** The host remembers `was` for the name, and the name now gives `now`; nothing was recorded. */
    RUNTIME_MARKETPLACE_NEEDS_USER_ACCEPTANCE = 3
} runtime_marketplace_memory;

/**
 * What remembering found, a runtime_marketplace_memory, with `was` and `now` for NEEDS_USER_ACCEPTANCE. First shape
 * ends at `storage`.
 */
typedef struct runtime_marketplace_remember_answer {
    uint32_t size;
    uint32_t memory;
    const char *was;
    const char *now;
    void *storage;
} runtime_marketplace_remember_answer;

/**
 * The marketplace service. The answer arguments are required and the refusal may be NULL for none. Every operation
 * answers MALFORMED for an argument without what it needs or holding a value that is not well-formed, REFUSED with the
 * refusal saying what to repair, and OUT_OF_MEMORY. First shape ends at `release_refusal`.
 */
typedef struct runtime_marketplace_definition {
    uint32_t size;
    void *context;
    /**
     * Builds the copy of a list: each entry carries the full name its repository has now and a name, the list's alias
     * or else the repository's or its folder's name, derived as an import name. An entry whose repository cannot be
     * looked up is carried over from `previous`, or left out; an entry whose name cannot be made an import name is left
     * out. The token is never written into the copy or a refusal.
     *
     * @return OK; REFUSED for a list or previous copy that breaks its schema, a built that is not RFC 3339, a local or
     * repeated id, two entries of one name, a token for another host, or a list of entries none of which a copy would
     * hold.
     */
    spisor_status (*build)(void *context, const runtime_marketplace_build_argument *argument,
                           runtime_marketplace_build_answer *answer, spisor_refusal *refusal);
    /**
     * The sources the settings list: an entry that is no record, has an id that is no import name, or has not exactly
     * one of a usable url and a provider is left out.
     *
     * @return OK; REFUSED for two sources of one id.
     */
    spisor_status (*sources)(void *context, const runtime_marketplace_sources_argument *argument,
                             runtime_marketplace_sources_answer *answer, spisor_refusal *refusal);
    /**
     * One source's copy, checked.
     *
     * @return OK; REFUSED for a body over 8 MiB, a web page, no JSON, a marketplace list where a copy is expected, a
     * copy that breaks its schema or names two entries alike, or a url that cannot be fetched.
     */
    spisor_status (*read)(void *context, const runtime_marketplace_read_argument *argument,
                          runtime_marketplace_copy_answer *answer, spisor_refusal *refusal);
    /**
     * What a name means in the copies. The same id in several marketplaces is the same plugin and settles.
     *
     * @return OK; NONE when nothing has the name, the answer left empty; REFUSED for a copy that breaks its schema or
     * two copies from one source.
     */
    spisor_status (*lookup)(void *context, const runtime_marketplace_lookup_argument *argument,
                            runtime_marketplace_lookup_answer *answer, spisor_refusal *refusal);
    /**
     * Keeps the id a name was given in the state place's marketplace-names.json, keyed by marketplace and name together
     * and by the bare name, so a name quietly pointed elsewhere needs the user to accept it.
     *
     * @return OK; REFUSED for a record that is not that file's shape, which is left as it is, or one that cannot be
     * written.
     */
    spisor_status (*remember)(void *context, const runtime_marketplace_remember_argument *argument,
                              runtime_marketplace_remember_answer *answer, spisor_refusal *refusal);
    void (*release_build_answer)(void *context, runtime_marketplace_build_answer *answer);
    void (*release_sources_answer)(void *context, runtime_marketplace_sources_answer *answer);
    void (*release_copy_answer)(void *context, runtime_marketplace_copy_answer *answer);
    void (*release_lookup_answer)(void *context, runtime_marketplace_lookup_answer *answer);
    void (*release_remember_answer)(void *context, runtime_marketplace_remember_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_marketplace_definition;

/** The static runtime_marketplace_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_marketplace_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
