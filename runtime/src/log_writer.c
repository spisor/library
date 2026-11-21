#include "log_writer.h"

/** Guards `running` and `writer`, which only start and stop touch. */
static runtime_mutex writer_lock = RUNTIME_MUTEX_INIT;
static runtime_log_queue queue = RUNTIME_LOG_QUEUE_INIT(RUNTIME_LOG_WRITER_CAPACITY);
static runtime_thread writer;
static int running;

static void drain(void *drained) {
    runtime_log_queued *entry;
    while ((entry = runtime_log_queue_take(drained)) != NULL) {
        entry->deliver(entry);
    }
}

spisor_status runtime_log_writer_start(void) {
    spisor_status status = SPISOR_OK;
    runtime_mutex_lock(&writer_lock);
    if (!running) {
        runtime_log_queue_open(&queue);
        running = runtime_thread_start(&writer, drain, &queue);
        if (!running) {
            runtime_log_queue_close(&queue);
            status = SPISOR_REFUSED;
        }
    }
    runtime_mutex_unlock(&writer_lock);
    return status;
}

void runtime_log_writer_stop(void) {
    runtime_mutex_lock(&writer_lock);
    if (running) {
        runtime_log_queue_close(&queue);
        runtime_thread_join(&writer);
        running = 0;
    }
    runtime_mutex_unlock(&writer_lock);
}

runtime_log_hand_off runtime_log_writer_hand(runtime_log_queued *entry) {
    return runtime_log_queue_hand(&queue, entry);
}
