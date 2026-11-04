#ifndef RUNTIME_BOOT_HINT_H
#define RUNTIME_BOOT_HINT_H

#include "cJSON.h"

/**
 * cache/hosts/<host_id>.json, freed with free; NULL with *problem set (freed with free) when host_id is
 * no plain file name, or with *problem NULL when memory runs out.
 */
char *runtime_boot_hint_path(const char *cache, const char *host_id, char **problem);

/**
 * The hint host_id last recorded, {libraries, host?}: its libraries' string entries and its host when
 * that is a string. NULL when there is none, it is not a readable JSON object, host_id is no plain file
 * name, or memory runs out.
 */
cJSON *runtime_read_boot_hint(const char *cache, const char *host_id);

/**
 * Records for the next start of host_id the libraries to load and the host bundle version to run, as
 * JSON.stringify({libraries, host?}, null, 2) and a newline; a NULL host keeps the one already recorded.
 * 1 on success; 0 with *problem set as runtime_boot_hint_path sets it, or naming the file that could not
 * be written.
 *
 * @implNote Replaced at once, so a start never reads a half-written hint. The format is plan four's,
 * which cache.sweep reads.
 */
int runtime_write_boot_hint(const char *cache, const char *host_id, const cJSON *libraries, const char *host,
                            char **problem);

/**
 * Makes version the host bundle the next start of host_id runs, keeping the libraries its hint names. 1
 * when that is recorded, and when host_id has no readable hint, which is left as it is.
 *
 * @implNote forebay/host's hintHostVersion also left a hint alone that named no runtime and koffi,
 * because only a JavaScript host could boot through one; a native host boots through a hint whose
 * libraries may be empty, so any readable hint is switched. A host with no hint at all has never been
 * installed to boot through one, and none is invented for it.
 */
int runtime_hint_host_version(const char *cache, const char *host_id, const char *version);

#endif
