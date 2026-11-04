#ifndef RUNTIME_CACHE_INSTALL_H
#define RUNTIME_CACHE_INSTALL_H

#include "cJSON.h"

/**
 * Installs version of the catalog item id into cache for platform, answering {id, version, directory,
 * fetched, cached}. A refusal answers NULL with *refusal set to its {message, fix}; NULL with *refusal
 * NULL means memory ran out.
 *
 * @implNote The catalog schema already refuses escaping ids and file keys; every destination is resolved
 * and checked again because cache.install takes any item, not only one catalog.read produced.
 */
cJSON *runtime_cache_install_version(const char *cache, const char *id, const cJSON *item, const char *version,
                                     const char *platform, cJSON **refusal);

#endif
