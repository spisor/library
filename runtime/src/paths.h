#ifndef RUNTIME_PATHS_H
#define RUNTIME_PATHS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

#if defined(_WIN32)
#define RUNTIME_HOST_IS_WINDOWS 1
#else
#define RUNTIME_HOST_IS_WINDOWS 0
#endif

typedef struct runtime_paths {
    char *home;
    char *repos;
    char *plugin;
    char *cache;
    char *config;
} runtime_paths;

/**
 * Fills paths with the home joined, for the host OS, to each kind's name: the name declared in names
 * when it is a usable portable segment, else the default (repos, plugin, cache, config). Answers 0 on
 * success and nonzero when memory runs out, leaving paths empty.
 *
 * @implNote Two kinds may resolve to the same path; that is not de-duplicated, as the declaration is
 * the plugin author's to get right.
 */
int runtime_resolve_paths(const char *home, const cJSON *names, runtime_paths *paths);

void runtime_free_paths(runtime_paths *paths);

/**
 * Whether home can anchor the paths every service writes: not empty, and absolute by the host OS's
 * rule, so nothing ever lands under the host's current directory (rev 146).
 */
int runtime_is_usable_home(const char *home);

/** The failure object every service answers for a home runtime_is_usable_home refuses. */
int32_t runtime_refuse_home(uint8_t **out, size_t *out_len);

RUNTIME_EXPORT int32_t runtime_paths_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
