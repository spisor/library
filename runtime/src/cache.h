#ifndef RUNTIME_CACHE_H
#define RUNTIME_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * cache.install: {cache, id, item, version, platform?} stores every file the item's version lists for
 * platform (this machine's when omitted) at cache/bundles/id/version/, or for a library at
 * cache/libraries/id/version/platform/, and answers {id, version, directory, fetched, cached}. A file
 * already stored with its hash is not fetched again.
 *
 * @implNote Every destination is checked before anything is written, and the completion marker
 * .installed.json is written last, so a refusal part way leaves no version anything runs; the next
 * install of the same version stores only what is still missing.
 */
RUNTIME_EXPORT int32_t runtime_cache_install(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * cache.installed: {cache, platform?} answers {bundles, libraries}, each id's versions whose install
 * completed and whose recorded files are all present, read from the folder names and markers on disk
 * without re-hashing; libraries for platform.
 */
RUNTIME_EXPORT int32_t runtime_cache_installed(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * cache.check: {directory} answers {kind: absent} with no completion marker, {kind: intact} when every
 * file the marker names still has its recorded hash, and {kind: damaged, detail, fix, file?} otherwise.
 * Local and network-free, so a host can afford it on every load.
 */
RUNTIME_EXPORT int32_t runtime_cache_check(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * cache.damaged: {cache, platform?} answers {damaged: [{id, version, detail, fix, file?}]}, every installed
 * bundle version and every library version for platform (this machine's when absent) that a start would
 * refuse on its sizes or whose files no longer carry their hashes. Local and network-free, but it hashes
 * every installed file, so status runs it and a start does not.
 */
RUNTIME_EXPORT int32_t runtime_cache_damaged(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/** cache.platform: {} answers {platform}, this build's key as Node spells it, process.platform-process.arch. */
RUNTIME_EXPORT int32_t runtime_cache_platform(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * cache.sweep: {cache, state} deletes every bundle version, and every library version per platform,
 * that no reference file under state/bundles, no live run record under state/bundles/runs and no boot
 * hint under cache/hosts names, keeping each id's newest installed version as the rollback, and
 * answers {removed}.
 *
 * @implNote Runs under the registry lock. A version with no completion marker is left alone while
 * anything in it changed within the last day, since another host sharing the cache may be installing
 * it; older, it is a failed install. A version that cannot be removed is skipped for the next sweep.
 */
RUNTIME_EXPORT int32_t runtime_cache_sweep(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
