#ifndef SPISOR_LOCK_H
#define SPISOR_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spisor_lock spisor_lock;

/**
 * Takes an exclusive lock on the file at path, creating it, and waits until no other thread or process
 * holds it. NULL when the file cannot be opened or locked, or memory runs out; its folder must exist.
 * Release with spisor_unlock. It is not reentrant: a thread that holds it and takes it again waits on
 * itself for ever.
 *
 * @implNote An OS file lock (LockFileEx on Windows, flock on POSIX) on an open handle, never the
 * presence of a file, so a crash cannot leave it held: the OS releases it when the process exits.
 * flock rather than fcntl because fcntl locks belong to the process and would not exclude two
 * threads. The file stays after the unlock and holds nothing.
 */
spisor_lock *spisor_lock_file(const char *path);

/** Releases the lock and closes the file; NULL is accepted and does nothing. */
void spisor_unlock(spisor_lock *lock);

#ifdef __cplusplus
}
#endif

#endif
