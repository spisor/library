#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "holders_lock.h"

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
    char *token = malloc(64);
    if (token != NULL) {
        snprintf(token, 64, "%d %lu-%.0f", (int)current_process_id(), next_token_number(),
                 floor(runtime_now_milliseconds()));
    }
    return token;
}

static char *read_lock(const char *path) {
    size_t length = 0;
    return (char *)spisor_read_file(path, &length);
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
    free(wide_moved);
    free(wide_path);
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
    free(token);
    if (moved == NULL || !runtime_move_file_over(path, moved)) {
        free(moved);
        return;
    }
    char *held = read_lock(moved);
    if (held == NULL || strcmp(held, judged) != 0) {
        link_back(moved, path);
    }
    free(held);
    runtime_remove_file_entry(moved);
    free(moved);
}

static runtime_holders_guard *guard_of(const char *path, char *token) {
    runtime_holders_guard *guard = malloc(sizeof *guard);
    char *copy = guard == NULL ? NULL : malloc(strlen(path) + 1);
    if (copy == NULL) {
        free(guard);
        free(token);
        return NULL;
    }
    memcpy(copy, path, strlen(path) + 1);
    guard->path = copy;
    guard->token = token;
    return guard;
}

runtime_holders_guard *runtime_lock_holders(const char *path) {
    char *token = new_token();
    if (token == NULL) {
        return NULL;
    }
    for (;;) {
        runtime_entry_outcome created = runtime_create_new_file(path, (const uint8_t *)token, strlen(token), 0);
        if (created == RUNTIME_ENTRY_DONE) {
            return guard_of(path, token);
        }
        if (created != RUNTIME_ENTRY_EXISTS) {
            free(token);
            return NULL;
        }
        char *held = read_lock(path);
        int stale = held != NULL && is_stale(path, held);
        if (stale) {
            remove_stale_lock(path, held);
        }
        free(held);
        if (!stale) {
            pause_milliseconds(LOCK_RETRY_MILLISECONDS);
        }
    }
}

void runtime_unlock_holders(runtime_holders_guard *guard) {
    if (guard == NULL) {
        return;
    }
    char *held = read_lock(guard->path);
    if (held != NULL && strcmp(held, guard->token) == 0) {
        runtime_remove_file_entry(guard->path);
    }
    free(held);
    free(guard->path);
    free(guard->token);
    free(guard);
}
