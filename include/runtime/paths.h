#ifndef RUNTIME_PUBLIC_PATHS_H
#define RUNTIME_PUBLIC_PATHS_H

/**
 * @file
 * runtime's `paths` service: where an application keeps its repos, plugin, cache and config folders under its home.
 * A size-prefixed struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for
 * the call; what an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The two places a plugin's own folders lie under, as places' `resolve` answers them: both absolute. */
typedef struct runtime_plugin_places {
    const char *portable;
    const char *cache;
} runtime_plugin_places;

/**
 * `home` is the application's home, an absolute directory. `names` is the plugin's own `names` declaration, a record
 * whose text members `repos`, `plugin`, `cache` and `config` rename those folders, or NULL for the defaults. `places`
 * NULL answers no plugin folders; set, it needs `plugin`, the manifest id. First shape ends at `places`.
 */
typedef struct runtime_paths_resolve_argument {
    uint32_t size;
    const char *home;
    const spisor_value *names;
    const char *plugin;
    const runtime_plugin_places *places;
} runtime_paths_resolve_argument;

/**
 * Every folder as an absolute path. `portable` and `plugin_cache` are the plugin's own folders in those places, NULL
 * when the argument carried no places. First shape ends at `storage`.
 */
typedef struct runtime_paths_answer {
    uint32_t size;
    const char *home;
    const char *repos;
    const char *plugin;
    const char *cache;
    const char *config;
    const char *portable;
    const char *plugin_cache;
    void *storage;
} runtime_paths_answer;

/** The paths service. First shape ends at `release_refusal`. */
typedef struct runtime_paths_definition {
    uint32_t size;
    void *context;
    /**
     * Resolves every folder. A declared name is used only when it is one portable path segment, and two kinds may
     * resolve to the same path.
     *
     * @param answer the consumer's empty answer. Required.
     * @param refusal the consumer's empty refusal, or NULL for none.
     * @return OK; MALFORMED for a home that is not absolute, `names` that is not a record, or places without
     * absolute folders and a plugin id that is one path segment; OUT_OF_MEMORY.
     */
    spisor_status (*resolve)(void *context, const runtime_paths_resolve_argument *argument,
                             runtime_paths_answer *answer, spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_paths_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_paths_definition;

/** The static runtime_paths_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_paths_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
