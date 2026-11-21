#include <stddef.h>

#include "check.h"
#include "log_queue.h"

/**
 * @file
 * The log's bounded queue on its own, no thread draining it: what it does with an entry when it is full and when it
 * is closed, and that closing it still answers every entry it took, in order, before it answers none.
 */

static void delivered(runtime_log_queued *entry) {
    (void)entry;
}

static runtime_log_queue queue = RUNTIME_LOG_QUEUE_INIT(2);

static void a_queue_takes_nothing_before_it_opens(void) {
    runtime_log_queued entry = {NULL, delivered};
    CHECK(runtime_log_queue_hand(&queue, &entry) == RUNTIME_LOG_CLOSED);
    CHECK(runtime_log_queue_take(&queue) == NULL);
}

static void a_full_queue_refuses_the_next_entry_and_keeps_what_it_holds(void) {
    runtime_log_queued first = {NULL, delivered};
    runtime_log_queued second = {NULL, delivered};
    runtime_log_queued third = {NULL, delivered};
    runtime_log_queue_open(&queue);
    CHECK(runtime_log_queue_hand(&queue, &first) == RUNTIME_LOG_TAKEN);
    CHECK(runtime_log_queue_hand(&queue, &second) == RUNTIME_LOG_TAKEN);
    CHECK(runtime_log_queue_hand(&queue, &third) == RUNTIME_LOG_FULL);
    CHECK(runtime_log_queue_take(&queue) == &first);
    CHECK(runtime_log_queue_hand(&queue, &third) == RUNTIME_LOG_TAKEN);
    runtime_log_queue_close(&queue);
    CHECK(runtime_log_queue_hand(&queue, &first) == RUNTIME_LOG_CLOSED);
    CHECK(runtime_log_queue_take(&queue) == &second);
    CHECK(runtime_log_queue_take(&queue) == &third);
    CHECK(runtime_log_queue_take(&queue) == NULL);
    CHECK(queue.count == 0 && queue.first == NULL && queue.last == NULL);
}

int main(void) {
    a_queue_takes_nothing_before_it_opens();
    a_full_queue_refuses_the_next_entry_and_keeps_what_it_holds();
    return check_summary("log_queue_test");
}
