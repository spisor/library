#ifndef RUNTIME_PATHS_H
#define RUNTIME_PATHS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "spisor/status.h"

#if defined(_WIN32)
#define RUNTIME_HOST_IS_WINDOWS 1
#else
#define RUNTIME_HOST_IS_WINDOWS 0
#endif

#if defined(__APPLE__)
#define RUNTIME_HOST_IS_MACOS 1
#else
#define RUNTIME_HOST_IS_MACOS 0
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
 * Whether home can anchor the paths every service writes: present, not empty, and absolute by the host OS's
 * rule, so nothing ever lands under the host's current directory (rev 146).
 */
int runtime_is_usable_home(const char *home);

#define RUNTIME_UNUSABLE_HOME_MESSAGE "the home is empty or not an absolute path"
#define RUNTIME_UNUSABLE_HOME_FIX                                                                              \
    "send the application's home as an absolute directory path; a relative one would resolve against the " \
    "host's current directory"

/** The MALFORMED refusal every service answers for a home runtime_is_usable_home refuses. */
spisor_status runtime_refuse_home(spisor_refusal *refusal);

/** The MALFORMED refusal every service answers for a portable place runtime_is_usable_home refuses. */
spisor_status runtime_refuse_portable(spisor_refusal *refusal);

/** A plugin's own folder in a place, place/plugins/folder_name, freed with free; NULL when memory runs out. */
char *runtime_plugin_folder(const char *place, const char *folder_name);

#endif
