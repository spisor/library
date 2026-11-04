#ifndef RUNTIME_CACHE_MARKER_H
#define RUNTIME_CACHE_MARKER_H

#include "cJSON.h"

/** The file an install writes last, so its presence means every file it names was stored and verified. */
#define RUNTIME_INSTALL_MARKER ".installed.json"

/** Whether directory holds a readable completion marker, without re-hashing the files it names. */
int runtime_cache_has_marker(const char *directory);

/**
 * Writes directory's completion marker naming hashes, an object of file keys to SHA-256s, as
 * JSON.stringify({files}, null, 2) and a newline. 1 on success.
 *
 * @implNote Written beside its name and renamed; when the rename fails because another host sharing the
 * cache holds the marker it just wrote, a marker already holding these bytes is accepted.
 */
int runtime_cache_write_marker(const char *directory, const cJSON *hashes);

/**
 * Whether the version in directory finished installing and every file still has its recorded hash:
 * {kind: absent}, {kind: intact} or {kind: damaged, detail, fix, file?}. NULL when memory runs out.
 */
cJSON *runtime_cache_check_version(const char *directory);

#endif
