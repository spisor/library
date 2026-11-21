#ifndef RUNTIME_LOG_QUEUE_H
#define RUNTIME_LOG_QUEUE_H

#include <stddef.h>

#include "thread.h"

typedef struct runtime_log_queued runtime_log_queued;

/**
 * One entry handed to a queue, the first member of whatever its producer allocated: `deliver` writes it and frees it,
 * on the thread draining the queue.
 */
struct runtime_log_queued {
    runtime_log_queued *next;
    void (*deliver)(runtime_log_queued *entry);
};

/** What a queue did with an entry handed to it. */
typedef enum runtime_log_hand_off {
    RUNTIME_LOG_TAKEN,
    /** The queue held `capacity` entries already, so this one was not taken. */
    RUNTIME_LOG_FULL,
    /** The queue is closed, so this one was not taken. */
    RUNTIME_LOG_CLOSED
} runtime_log_hand_off;

/**
 * A bounded queue of entries, taken in the order they were handed, safe from any thread. It is created closed, with
 * RUNTIME_LOG_QUEUE_INIT in static storage, and opened before its first entry.
 */
typedef struct runtime_log_queue {
    runtime_mutex lock;
    runtime_condition changed;
    runtime_log_queued *first;
    runtime_log_queued *last;
    size_t count;
    size_t capacity;
    int closed;
} runtime_log_queue;

#define RUNTIME_LOG_QUEUE_INIT(capacity) {RUNTIME_MUTEX_INIT, RUNTIME_CONDITION_INIT, NULL, NULL, 0, capacity, 1}

/** Opens a closed queue, so it takes entries again. */
void runtime_log_queue_open(runtime_log_queue *queue);

/** Closes the queue: it takes no further entry, and `take` answers what it holds, then NULL. */
void runtime_log_queue_close(runtime_log_queue *queue);

/** Hands `entry` to the queue without waiting; anything but TAKEN leaves the entry the caller's. */
runtime_log_hand_off runtime_log_queue_hand(runtime_log_queue *queue, runtime_log_queued *entry);

/** The oldest entry, waiting while the queue is open and empty; NULL once it is closed and empty. */
runtime_log_queued *runtime_log_queue_take(runtime_log_queue *queue);

#endif
