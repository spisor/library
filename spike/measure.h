#ifndef SPIKE_MEASURE_H
#define SPIKE_MEASURE_H

/**
 * @file
 * The kit's RUNNER.md timing, copied for the spike from weivel/c's benchmarks module: a discarded warm-up, then rounds
 * of one batch per path, the path that goes first moving on by one each round, each batch timed as one and divided by
 * its calls; two paths are compared by the median over rounds of their batches' ratio in the same round.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define SPIKE_MOST_PATHS 8
#define SPIKE_ROUNDS 60
#define SPIKE_BATCH_NANOSECONDS 4e6

typedef int (*spike_call)(void *data);

typedef struct spike_path {
    const char *name;
    spike_call call;
    void *data;
} spike_path;

static double spike_now(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1e9 / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1e9 + (double)now.tv_nsec;
#endif
}

static double spike_batch(const spike_path *path, uint32_t calls, int *failed) {
    double start = spike_now();
    for (uint32_t call = 0; call < calls; call++) {
        if (!path->call(path->data)) {
            *failed = 1;
        }
    }
    return (spike_now() - start) / calls;
}

static int spike_compare(const void *left, const void *right) {
    double difference = *(const double *)left - *(const double *)right;
    return (difference > 0) - (difference < 0);
}

static double spike_percentile(double *values, uint32_t count, double fraction) {
    qsort(values, count, sizeof values[0], spike_compare);
    double rank = ceil(fraction * count) - 1;
    return values[rank < 0 ? 0 : (uint32_t)rank];
}

/** Calls per batch so the first path's batch takes about SPIKE_BATCH_NANOSECONDS. */
static uint32_t spike_calibrate(const spike_path *path, int *failed) {
    uint32_t calls = 1;
    double per_call = spike_batch(path, 16, failed);
    if (per_call > 0) {
        calls = (uint32_t)(SPIKE_BATCH_NANOSECONDS / per_call);
    }
    return calls < 1 ? 1 : calls;
}

/**
 * Samples every path and prints each one's median nanoseconds per call and each path's per-round ratio over the
 * first path (median, quartiles); answers 0 when a call failed.
 */
static int spike_measure(const char *function, const spike_path *paths, uint32_t path_count) {
    static double times[SPIKE_MOST_PATHS][SPIKE_ROUNDS];
    double scratch[SPIKE_ROUNDS];
    int failed = 0;
    uint32_t calls = spike_calibrate(&paths[0], &failed);
    for (uint32_t path = 0; path < path_count; path++) {
        spike_batch(&paths[path], calls, &failed);
    }
    for (uint32_t round = 0; round < SPIKE_ROUNDS; round++) {
        for (uint32_t turn = 0; turn < path_count; turn++) {
            uint32_t path = (round + turn) % path_count;
            times[path][round] = spike_batch(&paths[path], calls, &failed);
        }
    }
    printf("SPIKE %s: %u rounds of %u calls per path after one discarded batch each\n", function, SPIKE_ROUNDS, calls);
    for (uint32_t path = 0; path < path_count; path++) {
        memcpy(scratch, times[path], sizeof scratch);
        double median = spike_percentile(scratch, SPIKE_ROUNDS, 0.5);
        for (uint32_t round = 0; round < SPIKE_ROUNDS; round++) {
            scratch[round] = times[path][round] / times[0][round];
        }
        double ratio_median = spike_percentile(scratch, SPIKE_ROUNDS, 0.5);
        double ratio_lower = spike_percentile(scratch, SPIKE_ROUNDS, 0.25);
        double ratio_upper = spike_percentile(scratch, SPIKE_ROUNDS, 0.75);
        printf("SPIKE %s %-28s median %10.1f ns/call   ratio to %s: median %.3f (quartiles %.3f to %.3f)\n", function,
               paths[path].name, median, paths[0].name, ratio_median, ratio_lower, ratio_upper);
    }
    fflush(stdout);
    return !failed;
}

#endif
