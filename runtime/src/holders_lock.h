#ifndef RUNTIME_HOLDERS_LOCK_H
#define RUNTIME_HOLDERS_LOCK_H

typedef struct runtime_holders_guard runtime_holders_guard;

/**
 * Takes the lock file at path, so one process or thread at a time reads and writes one background
 * host's holders, waiting while another holds it; its folder must exist. NULL when it cannot be
 * created, when a directory stands at path, or after thirty seconds without it, three times as long as
 * any live lock can hold out before it counts as stale, so a lock that can never be read or taken
 * refuses rather than waits for ever. Release with runtime_unlock_holders.
 *
 * @implNote The lock is a file created only if absent, holding the locker's pid, a space and a token of its own. A lock whose pid is not
 * alive, or that is older than ten seconds, is stale and taken: it is moved aside, and put back by a
 * hard link (which refuses to replace a newer lock) when what was moved is not the lock judged. An
 * empty lock is judged by its age alone, since its creator may not have written its token yet.
 */
runtime_holders_guard *runtime_lock_holders(const char *path);

/** Removes the lock, but only while it still holds this guard's token, and frees the guard. */
void runtime_unlock_holders(runtime_holders_guard *guard);

#endif
