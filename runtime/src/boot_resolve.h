#ifndef RUNTIME_BOOT_RESOLVE_H
#define RUNTIME_BOOT_RESOLVE_H

#include "cJSON.h"

/**
 * {executable, version, directory} for the version host_id's boot hint names in cache, for platform;
 * NULL with *refusal set to its {message, fix}, or with *refusal NULL when memory runs out.
 */
cJSON *runtime_resolve_booted_host(const char *cache, const char *host_id, const char *platform, cJSON **refusal);

/**
 * cache/bundles/<host_id>/<version>, freed, refused with the reinstall fix unless version names a folder strictly
 * inside cache/bundles/<host_id>, so a recorded version never reaches outside it; NULL with *refusal set, or with it
 * NULL when memory runs out.
 */
char *runtime_bundle_version_directory(const char *cache, const char *host_id, const char *version, cJSON **refusal);

#endif
