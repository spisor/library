#ifndef RUNTIME_HOSTS_LOCK_H
#define RUNTIME_HOSTS_LOCK_H

#include "spisor/lock.h"

/**
 * Takes the one lock every read-modify-write of a registration under state/hosts, of a host's
 * references under state/bundles, and the cache sweep hold, waiting until no other thread or process
 * holds it, and creates state/hosts/.lock when it is missing. NULL when the lock file cannot be opened
 * or locked. Release with spisor_unlock. Hold it around one check-act-store step, never around
 * an unbounded operation: no install or download is ever done while it is held. An install records the
 * version it is about to store as a reference first, under the lock, so a sweep keeps that version while
 * its files arrive unlocked; installing never makes a start or a registration wait.
 *
 * It is not reentrant: a thread that holds it and takes it again waits on itself for ever. So code
 * holding it never calls a json function that takes it (cache.sweep, registry.register, the updates
 * functions); it calls the internal step instead, runtime_cache_sweep_versions for a sweep, and
 * runtime_registry_load and runtime_registry_store, which do not take it themselves.
 *
 * @implNote The lock itself is spisor_lock_file; this only names the file, and registry.list ignores it
 * because it does not end in .json.
 */
spisor_lock *runtime_hosts_lock(const char *state);

#endif
