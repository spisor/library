#include "allocation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPISOR_TESTING

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static uint64_t countdown;
static uint64_t attempts;
static int failed;
static int sticky;
static int64_t live_blocks;

static int suspended;

/**
 * Guards every count, since a library under test may free on a thread of its own. Only the thread that last set a
 * failure point has its allocations counted as attempts and failed, so another thread's work never moves the point.
 */
#ifdef _WIN32
static SRWLOCK counting_lock = SRWLOCK_INIT;
static DWORD armed_thread;

static void lock_counts(void) {
    AcquireSRWLockExclusive(&counting_lock);
}

static void unlock_counts(void) {
    ReleaseSRWLockExclusive(&counting_lock);
}

static void arm_this_thread(void) {
    armed_thread = GetCurrentThreadId();
}

static int is_armed_thread(void) {
    return armed_thread == GetCurrentThreadId();
}
#else
static pthread_mutex_t counting_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t armed_thread;
static int is_armed;

static void lock_counts(void) {
    pthread_mutex_lock(&counting_lock);
}

static void unlock_counts(void) {
    pthread_mutex_unlock(&counting_lock);
}

static void arm_this_thread(void) {
    armed_thread = pthread_self();
    is_armed = 1;
}

static int is_armed_thread(void) {
    return is_armed && pthread_equal(armed_thread, pthread_self());
}
#endif

static int counted_failure(void) {
    if (suspended || !is_armed_thread()) {
        return 0;
    }
    attempts++;
    if (failed && sticky) {
        return 1;
    }
    if (countdown == 0) {
        return 0;
    }
    countdown--;
    if (countdown > 0) {
        return 0;
    }
    failed = 1;
    return 1;
}

static int fails_now(void) {
    int fails;
    lock_counts();
    fails = counted_failure();
    unlock_counts();
    return fails;
}

static void count_blocks(int64_t change) {
    lock_counts();
    live_blocks += change;
    unlock_counts();
}

void *spisor_memory_allocate(size_t bytes) {
    void *block;
    if (fails_now()) {
        return NULL;
    }
    block = malloc(bytes);
    if (block != NULL) {
        count_blocks(1);
    }
    return block;
}

void spisor_memory_release(void *block) {
    if (block != NULL) {
        count_blocks(-1);
    }
    free(block);
}

void *spisor_memory_reallocate(void *block, size_t bytes) {
    void *grown;
    if (fails_now()) {
        return NULL;
    }
    grown = realloc(block, bytes);
    if (grown != NULL && block == NULL) {
        count_blocks(1);
    }
    return grown;
}

static void set_failure_point(uint64_t attempt, int sticks) {
    lock_counts();
    arm_this_thread();
    countdown = attempt;
    attempts = 0;
    failed = 0;
    sticky = sticks;
    unlock_counts();
}

void spisor_memory_fail_at(uint64_t attempt) {
    set_failure_point(attempt, 0);
}

void spisor_memory_fail_from(uint64_t attempt) {
    set_failure_point(attempt, 1);
}

int spisor_memory_failed(void) {
    int answer;
    lock_counts();
    answer = failed;
    unlock_counts();
    return answer;
}

void spisor_memory_suspend_failures(int suspend) {
    lock_counts();
    suspended = suspend;
    unlock_counts();
}

uint64_t spisor_memory_attempts(void) {
    uint64_t answer;
    lock_counts();
    answer = attempts;
    unlock_counts();
    return answer;
}

int64_t spisor_memory_live_blocks(void) {
    int64_t answer;
    lock_counts();
    answer = live_blocks;
    unlock_counts();
    return answer;
}

#else

void *spisor_memory_allocate(size_t bytes) {
    return malloc(bytes);
}

void spisor_memory_release(void *block) {
    free(block);
}

void *spisor_memory_reallocate(void *block, size_t bytes) {
    return realloc(block, bytes);
}

#endif

void *spisor_memory_allocate_zeroed(size_t count, size_t size) {
    void *block;
    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    block = spisor_memory_allocate(count * size);
    if (block != NULL) {
        memset(block, 0, count * size);
    }
    return block;
}

char *spisor_memory_duplicate_text(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = spisor_memory_allocate(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}
