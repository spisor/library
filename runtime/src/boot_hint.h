#ifndef RUNTIME_BOOT_HINT_H
#define RUNTIME_BOOT_HINT_H

#include "cJSON.h"

/**
 * cache/hosts/<host_id>.json, freed with free; NULL with *problem set (freed with free) when host_id is
 * no plain file name, or with *problem NULL when memory runs out.
 */
char *runtime_boot_hint_path(const char *cache, const char *host_id, char **problem);

/**
 * The parsed text of a hint file read as a hint, {libraries: {<id>: {version, directory, binary?}}, host?}:
 * its host only when that is a string. NULL when recorded is no object, its libraries are no object, any
 * library entry has another shape, or memory runs out. The one reader of the format, which the sweep and
 * boot.read share.
 */
cJSON *runtime_boot_hint_of(const cJSON *recorded);

/**
 * The hint host_id last recorded, as runtime_boot_hint_of reads it. NULL when there is none, it is not a
 * readable hint, host_id is no plain file name, or memory runs out.
 */
cJSON *runtime_read_boot_hint(const char *cache, const char *host_id);

/**
 * The hint's libraries for versions, an object of library id to version string, on platform: each
 * {version, directory}, directory being the absolute cache/libraries/<id>/<version>/<platform>, and a
 * binary when that folder holds a library.json declaration: the one file directly inside it named with
 * the platform's suffix, .dll on win32-*, .dylib on darwin-*, .so otherwise. NULL with *problem set
 * (freed with free) naming the folder when a declared one holds no such file or several, or naming the
 * id and version when they name no folder inside cache/libraries; with *problem NULL when memory runs out.
 *
 * @implNote A folder with no declaration, such as koffi's, is located by its directory alone: its native
 * file is no library C opens, so naming it would put a rule about it in C.
 */
cJSON *runtime_boot_hint_libraries(const char *cache, const cJSON *versions, const char *platform, char **problem);

/**
 * Records for the next start of host_id the libraries to load, as runtime_boot_hint_libraries answers
 * them, and the host bundle version to run, as JSON.stringify({libraries, host?}, null, 2) and a newline;
 * a NULL host keeps the one already recorded.
 * 1 on success; 0 with *problem set as runtime_boot_hint_path sets it, or naming the file that could not
 * be written.
 *
 * @implNote Replaced at once, so a start never reads a half-written hint. cache.sweep reads this format
 * too, so the two change together.
 */
int runtime_write_boot_hint(const char *cache, const char *host_id, const cJSON *libraries, const char *host,
                            char **problem);

/**
 * Makes version the host bundle the next start of host_id runs, keeping the libraries its hint names. 1
 * when that is recorded, and when host_id has no readable hint, which is left as it is.
 *
 * @implNote Any readable hint is switched, whatever libraries it names: a native host boots through a
 * hint whose libraries may be empty. A host with no hint at all has never been
 * installed to boot through one, and none is invented for it.
 */
int runtime_hint_host_version(const char *cache, const char *host_id, const char *version);

#endif
