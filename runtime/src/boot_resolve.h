#ifndef RUNTIME_BOOT_RESOLVE_H
#define RUNTIME_BOOT_RESOLVE_H

#include "cJSON.h"

/**
 * {executable, version, directory} for the version host_id's boot hint names in cache, for platform;
 * NULL with *refusal set to its {message, fix}, or with *refusal NULL when memory runs out.
 */
cJSON *runtime_resolve_booted_host(const char *cache, const char *host_id, const char *platform, cJSON **refusal);

#endif
