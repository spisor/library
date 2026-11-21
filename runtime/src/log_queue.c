#include "log_queue.h"

void runtime_log_queue_open(runtime_log_queue *queue) {
    runtime_mutex_lock(&queue->lock);
    queue->closed = 0;
    runtime_mutex_unlock(&queue->lock);
}

void runtime_log_queue_close(runtime_log_queue *queue) {
    runtime_mutex_lock(&queue->lock);
    queue->closed = 1;
    runtime_condition_wake_all(&queue->changed);
    runtime_mutex_unlock(&queue->lock);
}

static void append(runtime_log_queue *queue, runtime_log_queued *entry) {
    entry->next = NULL;
    if (queue->last == NULL) {
        queue->first = entry;
    } else {
        queue->last->next = entry;
    }
    queue->last = entry;
    queue->count++;
}

runtime_log_hand_off runtime_log_queue_hand(runtime_log_queue *queue, runtime_log_queued *entry) {
    runtime_log_hand_off handed = RUNTIME_LOG_TAKEN;
    runtime_mutex_lock(&queue->lock);
    if (queue->closed) {
        handed = RUNTIME_LOG_CLOSED;
    } else if (queue->count >= queue->capacity) {
        handed = RUNTIME_LOG_FULL;
    } else {
        append(queue, entry);
        runtime_condition_wake_all(&queue->changed);
    }
    runtime_mutex_unlock(&queue->lock);
    return handed;
}

static runtime_log_queued *remove_first(runtime_log_queue *queue) {
    runtime_log_queued *entry = queue->first;
    queue->first = entry->next;
    if (queue->first == NULL) {
        queue->last = NULL;
    }
    queue->count--;
    entry->next = NULL;
    return entry;
}

runtime_log_queued *runtime_log_queue_take(runtime_log_queue *queue) {
    runtime_log_queued *entry = NULL;
    runtime_mutex_lock(&queue->lock);
    while (queue->first == NULL && !queue->closed) {
        runtime_condition_wait(&queue->changed, &queue->lock);
    }
    if (queue->first != NULL) {
        entry = remove_first(queue);
    }
    runtime_mutex_unlock(&queue->lock);
    return entry;
}
