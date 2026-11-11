#ifndef RUNTIME_INSTALL_HOSTS_H
#define RUNTIME_INSTALL_HOSTS_H

#include "cJSON.h"

/**
 * How the host bundle id at version in cache was installed: from a catalog, now, at
 * cache/bundles/<id>/<version>, as hosts.provenance writes a provenance. NULL with *refusal set, or with
 * none when memory runs out.
 */
cJSON *runtime_install_catalog_provenance(const char *cache, const char *id, const char *version, cJSON **refusal);

/**
 * Registers a host that loads into no application, such as a background host or spisor-host itself:
 * no commands, its home its own folder state/home/<id>, start when not NULL. Answers 1, or 0 with
 * *refusal set, or with none when memory runs out.
 *
 * @implNote It writes nothing onto the PATH, so its registration reads no environment; the first
 * registration wins, so a reinstall keeps the first install's time.
 */
int runtime_install_register_own_host(const cJSON *places, const char *id, const char *version, const cJSON *start,
                                      cJSON **refusal);

/** state/home/<id>, the home of a host that loads into no application; NULL when memory runs out. */
char *runtime_install_own_home(const char *state, const char *id);

#endif
