#ifndef RUNTIME_PUBLIC_LOADER_H
#define RUNTIME_PUBLIC_LOADER_H

/**
 * @file
 * runtime's `loader` service: whether a record is a loader standard, where the application a loader standard describes
 * keeps its files, and the standard an installed loader bundle carries. A size-prefixed struct is read only up to its
 * `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points
 * at is the provider's until the release for its kind. A place is an absolute directory, as places' `resolve` answers
 * it.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/environment.h"
#include "runtime/item_version.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The record to check as a loader standard, a well-formed value tree of any kind. First shape ends at `standard`. */
typedef struct runtime_loader_check_argument {
    uint32_t size;
    const spisor_value *standard;
} runtime_loader_check_argument;

/**
 * Why a record is no loader standard: the slash-separated `path` of the member at fault, empty for the record itself,
 * what is wrong with it and how to fix it. An element of an array, so it has no size.
 */
typedef struct runtime_loader_issue {
    const char *path;
    const char *detail;
    const char *fix;
} runtime_loader_issue;

/** The record's issues in the order the checks run, none for a loader standard. First shape ends at `storage`. */
typedef struct runtime_loader_check_answer {
    uint32_t size;
    const runtime_loader_issue *issues;
    size_t issue_count;
    void *storage;
} runtime_loader_check_answer;

/**
 * The home a loader standard declares for its application, and the user's `environment`: `env_override` the variable
 * that overrides the home, `native_env` the application's own variable, `xdg_subdir` its folder under XDG_CONFIG_HOME,
 * each NULL for none, and the `candidate_count` `candidates`, at least one, each an absolute path or one starting with
 * ~/. First shape ends at `environment_count`.
 */
typedef struct runtime_loader_home_argument {
    uint32_t size;
    const char *env_override;
    const char *native_env;
    const char *xdg_subdir;
    const char *const *candidates;
    size_t candidate_count;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_loader_home_argument;

/** Which rule found an application's home. Append-only: a published value keeps its number. */
typedef enum runtime_loader_home_source {
    RUNTIME_LOADER_HOME_OVERRIDE = 1,
    RUNTIME_LOADER_HOME_NATIVE = 2,
    RUNTIME_LOADER_HOME_XDG = 3,
    RUNTIME_LOADER_HOME_CANDIDATE = 4,
    RUNTIME_LOADER_HOME_DEFAULT = 5
} runtime_loader_home_source;

/** The application's home, absolute, and its `source`, a runtime_loader_home_source. First shape ends at `storage`. */
typedef struct runtime_loader_home_answer {
    uint32_t size;
    const char *home;
    uint32_t source;
    void *storage;
} runtime_loader_home_answer;

/**
 * The `loader` installed at its version in the `cache` place, an absolute path; the loader's id and version are each a
 * plain file name. First shape ends at `loader`.
 */
typedef struct runtime_loader_installed_argument {
    uint32_t size;
    const char *cache;
    const runtime_item_version *loader;
} runtime_loader_installed_argument;

/** The loader standard the installed bundle carries. First shape ends at `storage`. */
typedef struct runtime_loader_installed_answer {
    uint32_t size;
    const spisor_value *standard;
    void *storage;
} runtime_loader_installed_answer;

/**
 * The loader service. It writes nothing. Every operation answers MALFORMED for an argument short of its first shape,
 * a place that is not absolute, a required text that is NULL or empty, an array whose count has no elements or holds
 * a NULL or empty text, a value that is not well-formed, or an environment as runtime/environment.h refuses one; and
 * OUT_OF_MEMORY, which never reads as a file that is missing or not a loader standard. The answer arguments are
 * required and the refusal may be NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_loader_definition {
    uint32_t size;
    void *context;
    /**
     * Checks the record as a loader standard, which carries an application, the PATH commands it asks for, the
     * background hosts it keeps running and the plugins it cannot run without.
     *
     * @return OK, with no issue exactly for a loader standard; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*check)(void *context, const runtime_loader_check_argument *argument,
                           runtime_loader_check_answer *answer, spisor_refusal *refusal);
    /**
     * Where the application keeps its files: the override variable, then the native one, then `xdg_subdir` under an
     * absolute XDG_CONFIG_HOME, then the first candidate that is a directory, else the first candidate. An empty
     * variable counts as unset, and a relative XDG_CONFIG_HOME is ignored.
     *
     * @return OK; MALFORMED; REFUSED when a variable names a relative path, or a ~/ candidate has no user home to
     * expand against; OUT_OF_MEMORY.
     */
    spisor_status (*home)(void *context, const runtime_loader_home_argument *argument,
                          runtime_loader_home_answer *answer, spisor_refusal *refusal);
    /**
     * The loader standard in <cache>/bundles/<id>/<version>/host.json, read from the installed bundle since the
     * catalog and the registration carry only the application.
     *
     * @return OK; MALFORMED, also for an id or version that is no plain file name; REFUSED when the file is missing,
     * unreadable, not JSON or no loader standard, naming its first issue; OUT_OF_MEMORY.
     */
    spisor_status (*installed)(void *context, const runtime_loader_installed_argument *argument,
                               runtime_loader_installed_answer *answer, spisor_refusal *refusal);
    void (*release_check_answer)(void *context, runtime_loader_check_answer *answer);
    void (*release_home_answer)(void *context, runtime_loader_home_answer *answer);
    void (*release_installed_answer)(void *context, runtime_loader_installed_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_loader_definition;

/** The static runtime_loader_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_loader_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
