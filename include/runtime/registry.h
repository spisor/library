#ifndef RUNTIME_PUBLIC_REGISTRY_H
#define RUNTIME_PUBLIC_REGISTRY_H

/**
 * @file
 * runtime's `registry` service: the hosts registered on this machine, one record each under the state place's `hosts`
 * folder, written under one lock. A size-prefixed struct is read only up to its `size`. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the provider's until the
 * release for its kind. A place is an absolute directory, as places' `resolve` answers it, and a host id is one
 * portable path segment.
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
 * Where and how a host was installed: `method` and `version` the installer's, `location` the absolute path it lives at,
 * `os` and `architecture` the platform's, and `installed_at` an ISO 8601 UTC time such as 2026-11-02T10:00:00Z.
 * `package` is the package the method installed, or NULL; `options` the method's options, data whose shape belongs to
 * that method, or NULL. First shape ends at `options`.
 */
typedef struct runtime_registry_provenance {
    uint32_t size;
    const char *method;
    const char *version;
    const char *location;
    const char *os;
    const char *architecture;
    const char *installed_at;
    const char *package;
    const spisor_value *options;
} runtime_registry_provenance;

/** The language runtime a launcher starts a host with. Append-only: a published value keeps its number. */
typedef enum runtime_registry_start_runtime {
    RUNTIME_REGISTRY_START_JAVASCRIPT = 1,
    RUNTIME_REGISTRY_START_JVM = 2,
    RUNTIME_REGISTRY_START_NATIVE = 3
} runtime_registry_start_runtime;

/**
 * How a launcher starts the host: `runtime` a runtime_registry_start_runtime, and `entry` a relative path inside the
 * host's installed bundle that does not climb out of it. First shape ends at `entry`.
 */
typedef struct runtime_registry_start {
    uint32_t size;
    uint32_t runtime;
    const char *entry;
} runtime_registry_start;

/**
 * One host's registration. `version` is non-empty and `home` absolute; `application` is the application the host's
 * loader describes, data whose shape belongs to that loader, or NULL; `requires_command` is 1 when the host needs a
 * command on the PATH; `start` is NULL for a host no launcher starts. An answered registration holds what the record
 * stores, so a field a record written by hand lacks is NULL. First shape ends at `start`.
 */
typedef struct runtime_registration {
    uint32_t size;
    const char *id;
    const char *version;
    const char *home;
    const spisor_value *application;
    const runtime_registry_provenance *provenance;
    uint32_t requires_command;
    const runtime_registry_start *start;
} runtime_registration;

/** A registration to record in the `state` place. First shape ends at `registration`. */
typedef struct runtime_registry_register_argument {
    uint32_t size;
    const char *state;
    const runtime_registration *registration;
} runtime_registry_register_argument;

/** One host in the `state` place. First shape ends at `id`. */
typedef struct runtime_registry_host_argument {
    uint32_t size;
    const char *state;
    const char *id;
} runtime_registry_host_argument;

/** The hosts of the `state` place. First shape ends at `state`. */
typedef struct runtime_registry_list_argument {
    uint32_t size;
    const char *state;
} runtime_registry_list_argument;

/** One host to forget, with its folders under both the `state` and the `cache` place. First shape ends at `id`. */
typedef struct runtime_registry_forget_argument {
    uint32_t size;
    const char *state;
    const char *cache;
    const char *id;
} runtime_registry_forget_argument;

/** One registration, as recorded. First shape ends at `storage`. */
typedef struct runtime_registry_registration_answer {
    uint32_t size;
    const runtime_registration *registration;
    void *storage;
} runtime_registry_registration_answer;

/**
 * One listed host: its registration, and `orphan` 1 when its provenance location is missing or no longer on disk. An
 * element of an array, so it has no size.
 */
typedef struct runtime_registry_host {
    const runtime_registration *registration;
    uint32_t orphan;
} runtime_registry_host;

/**
 * Every readable registration in id order, and the ids of the registrations that are not readable. First shape ends at
 * `storage`.
 */
typedef struct runtime_registry_listing {
    uint32_t size;
    const runtime_registry_host *hosts;
    size_t host_count;
    const char *const *unreadable;
    size_t unreadable_count;
    void *storage;
} runtime_registry_listing;

/** Whether a registration was there to forget. It owns nothing. First shape ends at `forgotten`. */
typedef struct runtime_registry_forget_answer {
    uint32_t size;
    uint32_t forgotten;
} runtime_registry_forget_answer;

/**
 * The registry service. Every operation answers MALFORMED for a place that is not absolute or an id that is not one
 * portable path segment; REFUSED, with the refusal saying what to repair, when the registry lock cannot be taken or
 * the host's record exists but is not readable, which is then never written over; and OUT_OF_MEMORY. The answer
 * arguments are required and the refusal may be NULL for none. First shape ends at `release_refusal`.
 *
 * @implNote `register_host` and not `register`, which is a C keyword.
 */
typedef struct runtime_registry_definition {
    uint32_t size;
    void *context;
    /**
     * Records the registration, keeping the commands and changes the stored record holds, since those belong to the
     * services that record them.
     *
     * @return OK with the record as stored; MALFORMED for a registration, provenance or start short of its first
     * shape, or holding a field outside the rules above; REFUSED when the record cannot be written.
     */
    spisor_status (*register_host)(void *context, const runtime_registry_register_argument *argument,
                                   runtime_registry_registration_answer *answer, spisor_refusal *refusal);
    /**
     * One host's registration.
     *
     * @return OK; NONE when the host has none, the answer left empty.
     */
    spisor_status (*get)(void *context, const runtime_registry_host_argument *argument,
                         runtime_registry_registration_answer *answer, spisor_refusal *refusal);
    /**
     * Every host registered, an absent hosts folder holding none.
     *
     * @return OK; REFUSED when the hosts folder cannot be read.
     */
    spisor_status (*list)(void *context, const runtime_registry_list_argument *argument,
                          runtime_registry_listing *answer, spisor_refusal *refusal);
    /**
     * Removes the host's folders under both places, then its registration, last, so nothing is orphaned.
     *
     * @return OK, also when the host had no registration; REFUSED while the host holds a recorded change not yet
     * reversed, or when a folder cannot be removed.
     */
    spisor_status (*forget)(void *context, const runtime_registry_forget_argument *argument,
                            runtime_registry_forget_answer *answer, spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_registry_registration_answer *answer);
    void (*release_listing)(void *context, runtime_registry_listing *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_registry_definition;

/** The static runtime_registry_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_registry_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
