#ifndef RUNTIME_HOSTS_LOCK_H
#define RUNTIME_HOSTS_LOCK_H

typedef struct runtime_hosts_guard runtime_hosts_guard;

/**
 * Takes the one lock every read-modify-write of a registration under state/hosts, of a host's
 * references under state/bundles, and the cache sweep hold, waiting until no other thread or process
 * holds it, and creates state/hosts/.lock when it is missing. NULL when the lock file cannot be opened
 * or locked. Release with runtime_hosts_unlock. Hold it around one check-act-store step, never around
 * an unbounded operation: no install or download is ever done while it is held. An install records the
 * version it is about to store as a reference first, under the lock, so a sweep keeps that version while
 * its files arrive unlocked; installing never makes a start or a registration wait.
 *
 * It is not reentrant: a thread that holds it and takes it again waits on itself for ever. So code
 * holding it never calls a json function that takes it (cache.sweep, registry.register, the updates
 * functions); it calls the internal step instead, runtime_cache_sweep_versions for a sweep, and
 * runtime_registry_load and runtime_registry_store, which do not take it themselves.
 *
 * @implNote An OS file lock (LockFileEx on Windows, flock on POSIX) on an open handle, never the
 * presence of a file, so a crash cannot leave it held: the OS releases it when the process exits.
 * flock rather than fcntl because fcntl locks belong to the process and would not exclude two
 * threads. The .lock file stays, and registry.list ignores it because it does not end in .json.
 */
runtime_hosts_guard *runtime_hosts_lock(const char *state);

void runtime_hosts_unlock(runtime_hosts_guard *guard);

/**
 * Takes the same kind of OS file lock on the lock file at path, creating it, for another
 * read-modify-write that must exclude other threads and processes; its folder must exist. NULL when it
 * cannot be opened or locked. Release with runtime_hosts_unlock. Never the registry lock itself, so a
 * caller holding one may take the other.
 */
runtime_hosts_guard *runtime_lock_file(const char *path);

#endif
