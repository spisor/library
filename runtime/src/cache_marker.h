#ifndef RUNTIME_CACHE_MARKER_H
#define RUNTIME_CACHE_MARKER_H

#include "cJSON.h"

/** The file an install writes last, so its presence means every file it names was stored and verified. */
#define RUNTIME_INSTALL_MARKER ".installed.json"

/** Whether directory holds a readable completion marker, without re-hashing the files it names. */
int runtime_cache_has_marker(const char *directory);

/**
 * Whether directory holds a readable completion marker and every file it names is present inside
 * directory, without re-hashing them.
 *
 * @implNote A sweep that removes a version while an install of it is completing can leave the marker
 * behind its files, so a marker alone does not mean the version can run.
 */
int runtime_cache_has_files(const char *directory);

/**
 * Writes directory's completion marker naming hashes, an object of file keys to SHA-256s, and sizes, an
 * object of the same keys to byte counts, as JSON.stringify({files, sizes}, null, 2) and a newline. 1 on
 * success.
 *
 * @implNote Written beside its name and renamed; when the rename fails because another host sharing the
 * cache holds the marker it just wrote, a marker already holding these bytes is accepted.
 */
int runtime_cache_write_marker(const char *directory, const cJSON *hashes, const cJSON *sizes);

/**
 * Whether the version in directory finished installing and every file still has its recorded hash:
 * {kind: absent}, {kind: intact} or {kind: damaged, detail, fix, file?}. NULL when memory runs out.
 */
cJSON *runtime_cache_check_version(const char *directory);

/**
 * runtime_cache_check_version's answer from sizes alone: whether the marker is readable and every file it
 * names is present inside directory with the size its install recorded, a marker recording none being
 * damaged.
 *
 * @implNote A start runs this rather than hashing: hashing every file of spisor-host's version cost each
 * SessionStart about 55 ms on Windows. A same-size corruption is left to install, update and status,
 * which hash.
 */
cJSON *runtime_cache_check_version_sizes(const char *directory);

#endif
