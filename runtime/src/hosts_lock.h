#ifndef RUNTIME_HOSTS_LOCK_H
#define RUNTIME_HOSTS_LOCK_H

typedef struct runtime_hosts_guard runtime_hosts_guard;

/**
 * Takes the one lock every read-modify-write of a registration under state/hosts holds, waiting
 * until no other thread or process holds it, and creates state/hosts/.lock when it is missing.
 * NULL when the lock file cannot be opened or locked. Release with runtime_hosts_unlock. Hold it
 * around one check-act-store step, never around an unbounded operation, and never take it twice
 * in one thread: it is not reentrant, and runtime_registry_load and runtime_registry_store do not
 * take it themselves.
 *
 * @implNote An OS file lock (LockFileEx on Windows, flock on POSIX) on an open handle, never the
 * presence of a file, so a crash cannot leave it held: the OS releases it when the process exits.
 * flock rather than fcntl because fcntl locks belong to the process and would not exclude two
 * threads. The .lock file stays, and registry.list ignores it because it does not end in .json.
 */
runtime_hosts_guard *runtime_hosts_lock(const char *state);

void runtime_hosts_unlock(runtime_hosts_guard *guard);

#endif
