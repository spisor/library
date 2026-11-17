#ifndef RUNTIME_PUBLIC_BOOT_H
#define RUNTIME_PUBLIC_BOOT_H

/**
 * @file
 * runtime's `boot` service: the boot hint cache/hosts/<host id>.json that names the libraries and the host version
 * the next start of a host boots, and the native executable that version runs as. A size-prefixed struct is read only
 * up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal
 * points at is the provider's until the release for its kind. A place is a directory's path, a host id a plain file
 * name, and a platform a key such as linux-x64, this machine's for NULL.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The hint of host `host_id` in the `cache` place. First shape ends at `host_id`. */
typedef struct runtime_boot_host_argument {
    uint32_t size;
    const char *cache;
    const char *host_id;
} runtime_boot_host_argument;

/**
 * A library a hint boots: its version, its absolute folder, and the binary inside that folder its declaration names,
 * NULL when it declares none. An element of an array, so it has no size.
 */
typedef struct runtime_boot_library {
    const char *id;
    const char *version;
    const char *directory;
    const char *binary;
} runtime_boot_library;

/**
 * Whether a readable hint is recorded, 0 or 1, and when one is, its libraries and the host version it names, NULL
 * when it names none. First shape ends at `storage`.
 */
typedef struct runtime_boot_hint_answer {
    uint32_t size;
    uint32_t hinted;
    const runtime_boot_library *libraries;
    size_t library_count;
    const char *host;
    void *storage;
} runtime_boot_hint_answer;

/** A library id and the version to boot. An element of an array, so it has no size. */
typedef struct runtime_boot_version {
    const char *id;
    const char *version;
} runtime_boot_version;

/**
 * The libraries the next start of `host_id` boots and its `host` version, NULL to keep the one recorded; with `state`,
 * the place whose registry lock the write holds, NULL for none. First shape ends at `platform`.
 */
typedef struct runtime_boot_write_argument {
    uint32_t size;
    const char *cache;
    const char *host_id;
    const runtime_boot_version *libraries;
    size_t library_count;
    const char *host;
    const char *state;
    const char *platform;
} runtime_boot_write_argument;

/** The host whose hinted version to run, with the arguments to pass it. First shape ends at `platform`. */
typedef struct runtime_boot_resolve_argument {
    uint32_t size;
    const char *cache;
    const char *host_id;
    const char *const *arguments;
    size_t argument_count;
    const char *platform;
} runtime_boot_resolve_argument;

/**
 * The executable to run with its arguments, and the version and folder it belongs to. First shape ends at `storage`.
 */
typedef struct runtime_boot_resolve_answer {
    uint32_t size;
    const char *executable;
    const char *const *arguments;
    size_t argument_count;
    const char *version;
    const char *directory;
    void *storage;
} runtime_boot_resolve_answer;

/** The `cache` place to put spisor-boot in. First shape ends at `cache`. */
typedef struct runtime_boot_place_argument {
    uint32_t size;
    const char *cache;
} runtime_boot_place_argument;

/** Whether this call put spisor-boot there, 0 or 1, and its path. First shape ends at `storage`. */
typedef struct runtime_boot_place_answer {
    uint32_t size;
    uint32_t placed;
    const char *path;
    void *storage;
} runtime_boot_place_answer;

/**
 * The boot service. Every operation answers MALFORMED for an argument short of its first shape, a place or a required
 * text that is NULL or empty, a host id that is not a plain file name, an array whose count has no elements or holds a
 * NULL or empty text or an id twice, or a platform that is not a key; such a call has written nothing. The answer
 * arguments are required and the refusal may be NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_boot_definition {
    uint32_t size;
    void *context;
    /**
     * The hint the host last recorded. It writes nothing.
     *
     * @return OK, `hinted` 0 when none is readable; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*read)(void *context, const runtime_boot_host_argument *argument,
                          runtime_boot_hint_answer *answer, spisor_refusal *refusal);
    /**
     * Records the libraries and the host version the next start boots, each library located in the cache for the
     * platform, keeping the recorded host version for a NULL host. Keeping it is a read-modify-write, so with a state
     * it holds the registry lock that updates holds while it switches a hint, and never reverts a switch. The answer
     * is taken before the one file is written.
     *
     * @return OK; MALFORMED; REFUSED when a library is not installed for the platform, the lock cannot be taken, or
     * the hint cannot be written; OUT_OF_MEMORY. A call that does not answer OK has written nothing.
     */
    spisor_status (*write)(void *context, const runtime_boot_write_argument *argument,
                           runtime_boot_hint_answer *answer, spisor_refusal *refusal);
    /**
     * The native executable of the version the host's hint names, cache/bundles/<host id>/<version>/<platform>/<host
     * id>, with .exe on a win32 platform, with the arguments passed through. No version is ever guessed. It writes
     * nothing.
     *
     * @return OK; MALFORMED; REFUSED, with the reinstall fix, when the hint is missing or unreadable, or its version is
     * not installed or no longer carries its recorded sizes; OUT_OF_MEMORY.
     */
    spisor_status (*resolve)(void *context, const runtime_boot_resolve_argument *argument,
                             runtime_boot_resolve_answer *answer, spisor_refusal *refusal);
    /**
     * Copies the spisor-boot the hinted spisor-host version carries to cache/boot when none is there yet, made
     * runnable, never replacing one that is there, since spisor-boot runs every command.
     *
     * @return OK, `placed` 0 when one was there or no hinted version carries one; MALFORMED; REFUSED when the copy
     * cannot be written; OUT_OF_MEMORY. A call that does not answer OK has put nothing in place.
     */
    spisor_status (*place)(void *context, const runtime_boot_place_argument *argument,
                           runtime_boot_place_answer *answer, spisor_refusal *refusal);
    void (*release_hint_answer)(void *context, runtime_boot_hint_answer *answer);
    void (*release_resolve_answer)(void *context, runtime_boot_resolve_answer *answer);
    void (*release_place_answer)(void *context, runtime_boot_place_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_boot_definition;

/** The static runtime_boot_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_boot_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
