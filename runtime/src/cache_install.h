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

/** An install with every file stored and its answer built, its completion marker not yet written. */
typedef struct runtime_cache_installation runtime_cache_installation;

/**
 * runtime_cache_install_version up to its completion marker, which it does not write, so a caller can take its own
 * answer's storage first. id and version are borrowed for the installation's life. NULL as that function answers it.
 */
runtime_cache_installation *runtime_cache_store_version(const char *cache, const char *id, const cJSON *item,
                                                        const char *version, const char *platform, cJSON **refusal);

/** {id, version, directory, fetched, cached}, owned by the installation. */
const cJSON *runtime_cache_installation_answer(const runtime_cache_installation *installation);

/** Writes the completion marker: 1, or 0 with *refusal set, or with it NULL when memory ran out. */
int runtime_cache_mark_installation(const runtime_cache_installation *installation, cJSON **refusal);

/** Marks installation complete and frees it, answering as runtime_cache_install_version does; NULL passes through. */
cJSON *runtime_cache_finish_installation(runtime_cache_installation *installation, cJSON **refusal);

void runtime_cache_free_installation(runtime_cache_installation *installation);

#endif
