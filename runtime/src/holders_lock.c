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
#include "process_alive.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

#include "wide_path.h"
#define current_process_id() _getpid()
#else
#include <time.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define STALE_LOCK_MILLISECONDS 10000.0
#define LOCK_RETRY_MILLISECONDS 25
#define LONGEST_WAIT_MILLISECONDS (3.0 * STALE_LOCK_MILLISECONDS)

struct runtime_holders_guard {
    char *path;
    char *token;
};

static volatile long token_number = 0;

static unsigned long next_token_number(void) {
#if defined(_WIN32)
    return (unsigned long)InterlockedIncrement(&token_number);
#else
    return (unsigned long)__atomic_add_fetch(&token_number, 1, __ATOMIC_RELAXED);
#endif
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
    char *token = spisor_memory_allocate(64);
    if (token != NULL) {
        snprintf(token, 64, "%d %lu-%.0f", (int)current_process_id(), next_token_number(),
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

static int link_back(const char *moved, const char *path) {
#if defined(_WIN32)
    wchar_t *wide_moved = spisor_wide_path(moved);
    wchar_t *wide_path = spisor_wide_path(path);
    int linked = wide_moved != NULL && wide_path != NULL && CreateHardLinkW(wide_path, wide_moved, NULL) != 0;
    spisor_memory_release(wide_moved);
    spisor_memory_release(wide_path);
    return linked;
#else
    return link(moved, path) == 0;
#endif
}

/** Moves the lock judged stale away, putting it back when what was moved is not the lock judged. */
static void remove_stale_lock(const char *path, const char *judged) {
    char *token = new_token();
    const char *parts[4] = {path, ".", token, ".stale"};
    char *moved = token == NULL ? NULL : runtime_cache_text(parts, 4);
    spisor_memory_release(token);
    if (moved == NULL || runtime_move_file_over(path, moved) != SPISOR_OK) {
        spisor_memory_release(moved);
        return;
    }
    int out_of_memory = 0;
    char *held = read_lock(moved, &out_of_memory);
    if (held == NULL || strcmp(held, judged) != 0) {
        link_back(moved, path);
    }
    spisor_memory_release(held);
    runtime_remove_file_entry(moved);
    spisor_memory_release(moved);
}

static void release_guard(runtime_holders_guard *guard) {
    spisor_memory_release(guard->path);
    spisor_memory_release(guard->token);
    spisor_memory_release(guard);
}

static runtime_holders_guard *guard_of(const char *path, char *token) {
    runtime_holders_guard *guard = spisor_memory_allocate(sizeof *guard);
    char *copy = guard == NULL ? NULL : spisor_memory_allocate(strlen(path) + 1);
    if (copy == NULL) {
        spisor_memory_release(guard);
        spisor_memory_release(token);
        return NULL;
    }
    memcpy(copy, path, strlen(path) + 1);
    guard->path = copy;
    guard->token = token;
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
    runtime_holders_guard *made = token == NULL ? NULL : guard_of(path, token);
    if (made == NULL) {
        return SPISOR_OUT_OF_MEMORY;
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
            remove_stale_lock(path, held);
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
    int out_of_memory = 0;
    char *held = read_lock(guard->path, &out_of_memory);
    if (held != NULL && strcmp(held, guard->token) == 0) {
        runtime_remove_file_entry(guard->path);
    }
    spisor_memory_release(held);
    release_guard(guard);
}
