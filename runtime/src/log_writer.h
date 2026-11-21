#ifndef RUNTIME_LOG_WRITER_H
#define RUNTIME_LOG_WRITER_H

#include "log_queue.h"
#include "spisor/status.h"

/**
 * The log's writer: one thread draining one queue of RUNTIME_LOG_WRITER_CAPACITY entries, so a log write hands its
 * entry over and never waits on the file. The started host owns it: runtime_scopes_begin starts it, runtime_scopes_end
 * stops it.
 */
#define RUNTIME_LOG_WRITER_CAPACITY 1024

/**
 * Starts the writer when it is not running, `*started` saying whether this call started it.
 *
 * @return OK; REFUSED when its thread cannot start.
 */
spisor_status runtime_log_writer_start(int *started);

/** Stops the writer once it has delivered every entry handed to it; a writer not running is left as it is. */
void runtime_log_writer_stop(void);

/** Hands `entry` to the writer without waiting; FULL and CLOSED leave the entry the caller's. */
runtime_log_hand_off runtime_log_writer_hand(runtime_log_queued *entry);

#endif
