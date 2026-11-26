#ifndef RUNTIME_TESTING_HOME_H
#define RUNTIME_TESTING_HOME_H

/**
 * @file
 * runtime-testing's `runtime.testing` service: a home laid out by runtime's own writers, as an install leaves one, for
 * a test of a host or a plugin in any language. It is a test-only library and never ships. A size-prefixed struct is
 * read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or
 * a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/boot.h"
#include "runtime/environment.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(RUNTIME_TESTING_BUILDING)
#define RUNTIME_TESTING_API __declspec(dllexport)
#elif defined(_WIN32)
#define RUNTIME_TESTING_API
#else
#define RUNTIME_TESTING_API __attribute__((visibility("default")))
#endif

/**
 * One plugin installed in the home: `manifest` the plugin.json record, its id, version and entry required;
 * `entry_source` the absolute file copied to the manifest's entry; `settings` its stored settings, a RECORD set key
 * by key through config's set, or NULL for none. An element of an array, so it has no size.
 */
typedef struct runtime_testing_install {
    const spisor_value *manifest;
    const char *entry_source;
    const spisor_value *settings;
} runtime_testing_install;

/**
 * Lays a home out at `base`, which must be absolute, exist and be empty; nothing is written outside it. `host_id` is
 * the host registered and held. `libraries` are those the javascript-host's hint names, a NULL version read from the
 * library.json in its directory, and no other library is hinted beside them. `holder` is the
 * pid recorded as the host's one holder, 0 for no processes record. First shape ends at `holder`.
 */
typedef struct runtime_testing_home_argument {
    uint32_t size;
    const char *base;
    const char *host_id;
    const runtime_testing_install *installs;
    size_t install_count;
    const runtime_boot_library *libraries;
    size_t library_count;
    int64_t holder;
} runtime_testing_home_argument;

/**
 * Where one install landed: its `bundle`, the version folder, and the `settings_file` its config reads. An element of an
 * array, so it has no size.
 */
typedef struct runtime_testing_placed {
    const char *id;
    const char *bundle;
    const char *settings_file;
} runtime_testing_placed;

/**
 * The `variables` to set while the home lives (SPISOR_HOME, SPISOR_STATE, SPISOR_CACHE, SPISOR_SECRETS, HOME and
 * USERPROFILE), the host's application `home`, the `processes` folder, and where each install landed, in the order
 * given. First shape ends at `storage`.
 */
typedef struct runtime_testing_home_answer {
    uint32_t size;
    const runtime_variable *variables;
    size_t variable_count;
    const char *home;
    const char *processes;
    const runtime_testing_placed *installs;
    size_t install_count;
    void *storage;
} runtime_testing_home_answer;

/** The environment the places resolve from, before any home moves it. First shape ends at `environment_count`. */
typedef struct runtime_testing_real_places_argument {
    uint32_t size;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_testing_real_places_argument;

/**
 * The places and the secrets file, absolute, leaving out each that repeats one listed before it or lies inside another,
 * as the secrets file lies in the state place unless SPISOR_SECRETS moves it. First shape ends at `storage`.
 */
typedef struct runtime_testing_real_places_answer {
    uint32_t size;
    const char *const *paths;
    size_t path_count;
    void *storage;
} runtime_testing_real_places_answer;

/** The runtime.testing service. First shape ends at `release_refusal`. */
typedef struct runtime_testing_definition {
    uint32_t size;
    void *context;
    /**
     * Lays the home out.
     *
     * @return OK; MALFORMED for a short shape, a base not absolute, missing or not empty, a host id that is no plain
     * file name, an install without a manifest record carrying id, version and entry or without an entry source, an
     * entry or a bundle climbing out of its folder, an entry naming the bundle's plugin.json or completion marker, settings that are no record, a library without an id or a
     * directory, or a version neither given nor declared, or a negative holder; REFUSED when a file cannot be written;
     * what config's set answers for a setting; OUT_OF_MEMORY. A call that does not answer OK removes what it wrote,
     * unless that removal itself fails, as it can while memory stays out.
     */
    spisor_status (*lay_out_home)(void *context, const runtime_testing_home_argument *argument,
                                  runtime_testing_home_answer *answer, spisor_refusal *refusal);
    /**
     * Resolves through runtime's places, as a host given this environment and no home would.
     *
     * @return OK; MALFORMED for a short shape or what places' resolve refuses; OUT_OF_MEMORY.
     */
    spisor_status (*real_places)(void *context, const runtime_testing_real_places_argument *argument,
                                 runtime_testing_real_places_answer *answer, spisor_refusal *refusal);
    void (*release_home_answer)(void *context, runtime_testing_home_answer *answer);
    void (*release_real_places_answer)(void *context, runtime_testing_real_places_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_testing_definition;

/** The static runtime_testing_definition, living until the library is unloaded. */
RUNTIME_TESTING_API const void *runtime_testing_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
