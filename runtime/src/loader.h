#ifndef RUNTIME_LOADER_H
#define RUNTIME_LOADER_H

#include "cJSON.h"

/**
 * The issues of value as a loader standard, an array of { path, detail, fix } in the order the checks
 * run, empty when value is one; NULL when memory runs out.
 */
cJSON *runtime_loader_standard_issues(const cJSON *value);

/**
 * Where the application whose standard home is home keeps its files under environment, as
 * { home, source }: the override variable, then the native one, then the xdgSubdir of an absolute
 * XDG_CONFIG_HOME, then the first candidate that is a directory, else the first candidate. NULL with
 * *refusal set when a variable names a relative path or a ~/ candidate has no user home to expand
 * against; NULL with no refusal when memory runs out.
 *
 * @implNote The order mirrors how the application itself finds its home, because a loader records the
 * result and a different answer would point the registration at a directory the application never
 * reads. An empty variable counts as unset, as the applications treat it, and a relative
 * XDG_CONFIG_HOME is ignored rather than refused, as the XDG specification requires.
 */
cJSON *runtime_application_home(const cJSON *home, const cJSON *environment, cJSON **refusal);

/**
 * loader's installed: the loader standard the installed host bundle `id` at `version` carries in
 * <cache>/bundles/<id>/<version>/host.json, freed with cJSON_Delete; NULL with *refusal set when that file cannot be
 * read or is no loader standard, or with it NULL when memory runs out.
 *
 * @implNote Read from the installed bundle, since the catalog and the registration carry only the
 * application.
 */
cJSON *runtime_loader_installed(const char *cache, const char *id, const char *version, cJSON **refusal);

#endif
