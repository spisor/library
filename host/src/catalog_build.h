#ifndef HOST_CATALOG_BUILD_H
#define HOST_CATALOG_BUILD_H

#include <stddef.h>

#include "cJSON.h"

/** One built item for a local catalog, per platform when platform names one. */
typedef struct catalog_input {
    const char *kind;
    char *id;
    char *version;
    char *platform;
    cJSON *files;
    cJSON *application;
    char *install_method;
    char *runtime;
} catalog_input;

/** The inputs a catalog build lists, in the order given. */
typedef struct catalog_inputs {
    catalog_input *items;
    size_t count;
    size_t capacity;
} catalog_inputs;

/** Adds a directory forebay-host bundle wrote: its plugin.json, the entry it names and that entry's module marker. */
int host_read_plugin_directory(catalog_inputs *inputs, const char *directory, cJSON **refusal);

/**
 * Adds a host bundle directory, for platform when not NULL: every file in it with its id, version and
 * runtime from host.json, or for a loader, whose host.json declares an application, that one data-only
 * file and the application.
 */
int host_read_host_directory(catalog_inputs *inputs, const char *directory, const char *platform, cJSON **refusal);

/** Adds a library directory for platform: every file in it, its version from library.json, else package.json. */
int host_read_library_directory(catalog_inputs *inputs, const char *id, const char *directory, const char *platform,
                                cJSON **refusal);

void host_free_catalog_inputs(catalog_inputs *inputs);

/**
 * Copies each input under <out>/files/<id>/<version>/[<platform>/] and writes <out>/catalog.json listing
 * them by relative url, latest by semver, each item's runtime copied from its manifest; answers the index's
 * path, or NULL with *refusal set.
 *
 * @implNote Every input is checked before anything is copied, so a refusal leaves no catalog behind.
 */
char *host_build_local_catalog(const catalog_inputs *inputs, const char *out, cJSON **refusal);

#endif
