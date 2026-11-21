#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "holders_lock.h"
#include "allocation.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "directory_tree.h"
#include "file_stream.h"
#include "file_times.h"
#include "prepared_undo.h"
#include "process_alive.h"
#include "spisor/file.h"
#include "thread.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

#define current_process_id() _getpid()
#else
#include <time.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define STALE_LOCK_MILLISECONDS 10000.0
#define LOCK_RETRY_MILLISECONDS 25
#define LONGEST_WAIT_MILLISECONDS (3.0 * STALE_LOCK_MILLISECONDS)

#define TOKEN_CAPACITY 64

/**
 * The lock's token, with the lock and its stale name prepared at lock time, so neither unlocking nor moving a stale
 * lock aside and back allocates (prepared_undo.h).
 */
struct runtime_holders_guard {
    char *token;
    spisor_prepared_path lock;
    spisor_prepared_path stale;
};

static unsigned long next_token_number(void) {
    static volatile long token_number = 0;
    return (unsigned long)runtime_atomic_increment(&token_number);
}

static void pause_milliseconds(long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

/** A text no other taker of any lock writes: the pid, a space, then a number and the time. */
static char *new_token(void) {
    char *token = spisor_memory_allocate(TOKEN_CAPACITY);
    if (token != NULL) {
        snprintf(token, TOKEN_CAPACITY, "%d %lu-%.0f", (int)current_process_id(), next_token_number(),
                 floor(runtime_now_milliseconds()));
    }
    return token;
}

/** The lock's token, freed; NULL when there is none to read, with *out_of_memory set when memory ran out. */
static char *read_lock(const char *path, int *out_of_memory) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    char *token = (char *)spisor_read_file_reporting(path, &length, &outcome);
    *out_of_memory = outcome == SPISOR_READ_OUT_OF_MEMORY;
    return token;
}

/** parseInt(token, 10): leading whitespace, an optional sign, then digits; NaN when there are none. */
static double leading_integer(const char *token) {
    const char *cursor = token;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    int negative = *cursor == '-';
    cursor += *cursor == '-' || *cursor == '+';
    if (!isdigit((unsigned char)*cursor)) {
        return NAN;
    }
    double value = 0;
    while (isdigit((unsigned char)*cursor)) {
        value = value * 10 + (*cursor++ - '0');
    }
    return negative ? -value : value;
}

static int is_old(const char *path) {
    double changed = 0;
    return runtime_newest_change(path, &changed) && runtime_now_milliseconds() - changed > STALE_LOCK_MILLISECONDS;
}

static int is_stale(const char *path, const char *held) {
    if (*held != 0 && !runtime_process_is_alive(leading_integer(held))) {
        return 1;
    }
    return is_old(path);
}

/** Whether the prepared file holds exactly text, read without allocating. */
static int holds_text(const spisor_prepared_path *file, const char *text) {
    uint8_t held[TOKEN_CAPACITY];
    size_t length = 0;
    return spisor_read_prepared_file(file, held, sizeof held, &length) && length == strlen(text) &&
           memcmp(held, text, length) == 0;
}

/** Moves the lock judged stale away, putting it back when what was moved is not the lock judged. */
static void remove_stale_lock(const runtime_holders_guard *guard, const char *judged) {
    if (!spisor_move_prepared_file(&guard->lock, &guard->stale)) {
        return;
    }
    if (!holds_text(&guard->stale, judged)) {
        spisor_link_prepared_file(&guard->stale, &guard->lock);
    }
    spisor_remove_prepared_file(&guard->stale);
}

static void release_guard(runtime_holders_guard *guard) {
    spisor_release_prepared_path(&guard->lock);
    spisor_release_prepared_path(&guard->stale);
    spisor_memory_release(guard->token);
    spisor_memory_release(guard);
}

static spisor_status prepare_stale_name(const char *path, runtime_holders_guard *guard) {
    const char *parts[4] = {path, ".", guard->token, ".stale"};
    char *stale = runtime_cache_text(parts, 4);
    spisor_status prepared = stale == NULL ? SPISOR_OUT_OF_MEMORY : spisor_prepare_path(stale, &guard->stale);
    spisor_memory_release(stale);
    return prepared;
}

/** The guard for path, taking token: NULL holding nothing when memory ran out, *status saying why. */
static runtime_holders_guard *guard_of(const char *path, char *token, spisor_status *status) {
    runtime_holders_guard *guard = spisor_memory_allocate_zeroed(1, sizeof *guard);
    if (guard == NULL) {
        spisor_memory_release(token);
        *status = SPISOR_OUT_OF_MEMORY;
        return NULL;
    }
    guard->token = token;
    *status = spisor_prepare_path(path, &guard->lock);
    if (*status == SPISOR_OK) {
        *status = prepare_stale_name(path, guard);
    }
    if (*status != SPISOR_OK) {
        release_guard(guard);
        return NULL;
    }
    return guard;
}

/**
 * Whether waiting on longer cannot help: something not a file stands at path, or the wait outlived any lock;
 * SPISOR_OUT_OF_MEMORY when memory ran out asking, SPISOR_OK to wait on.
 */
static spisor_status hopelessness(const char *path, double started) {
    runtime_entry_kind found = runtime_probe_entry(path);
    if (found == RUNTIME_ENTRY_PROBE_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    int hopeless = found == RUNTIME_ENTRY_DIRECTORY || runtime_now_milliseconds() - started > LONGEST_WAIT_MILLISECONDS;
    return hopeless ? SPISOR_REFUSED : SPISOR_OK;
}

static spisor_status refused_creating(runtime_entry_outcome created, const char *path, double started) {
    if (created == RUNTIME_ENTRY_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return created == RUNTIME_ENTRY_EXISTS ? hopelessness(path, started) : SPISOR_REFUSED;
}

/** @implNote The guard is made before the lock file, so memory running out never leaves a lock nobody holds. */
spisor_status runtime_lock_holders(const char *path, runtime_holders_guard **guard) {
    *guard = NULL;
    char *token = new_token();
    spisor_status prepared = SPISOR_OUT_OF_MEMORY;
    runtime_holders_guard *made = token == NULL ? NULL : guard_of(path, token, &prepared);
    if (made == NULL) {
        return prepared;
    }
    double started = runtime_now_milliseconds();
    for (;;) {
        runtime_entry_outcome created = runtime_create_new_file(path, (const uint8_t *)token, strlen(token), 0);
        if (created == RUNTIME_ENTRY_DONE) {
            *guard = made;
            return SPISOR_OK;
        }
        spisor_status refused = refused_creating(created, path, started);
        if (refused != SPISOR_OK) {
            release_guard(made);
            return refused;
        }
        int out_of_memory = 0;
        char *held = read_lock(path, &out_of_memory);
        if (out_of_memory) {
            release_guard(made);
            return SPISOR_OUT_OF_MEMORY;
        }
        int stale = held != NULL && is_stale(path, held);
        if (stale) {
            remove_stale_lock(made, held);
        }
        spisor_memory_release(held);
        if (!stale) {
            pause_milliseconds(LOCK_RETRY_MILLISECONDS);
        }
    }
}

void runtime_unlock_holders(runtime_holders_guard *guard) {
    if (guard == NULL) {
        return;
    }
    if (holds_text(&guard->lock, guard->token)) {
        spisor_remove_prepared_file(&guard->lock);
    }
    release_guard(guard);
}
