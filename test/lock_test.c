#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/lock.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#define WORKER_ARGUMENT "worker"
#define WORKERS 2
#define INCREMENTS 300
#define LOCK_PATH SPISOR_SCRATCH_PATH "/lock_test.lock"
#define COUNTER_PATH SPISOR_SCRATCH_PATH "/lock_test.counter"

static long read_counter(void) {
    long value = 0;
    FILE *file = fopen(COUNTER_PATH, "rb");
    if (file != NULL) {
        if (fscanf(file, "%ld", &value) != 1) {
            value = 0;
        }
        fclose(file);
    }
    return value;
}

static void write_counter(long value) {
    FILE *file = fopen(COUNTER_PATH, "wb");
    if (file != NULL) {
        fprintf(file, "%ld", value);
        fclose(file);
    }
}

static int run_worker(void) {
    for (int step = 0; step < INCREMENTS; step++) {
        spisor_lock *lock = spisor_lock_file(LOCK_PATH);
        if (lock == NULL) {
            return 1;
        }
        write_counter(read_counter() + 1);
        spisor_unlock(lock);
    }
    return 0;
}

static int start_worker(const char *self, void **started) {
#if defined(_WIN32)
    char quoted[1100];
    snprintf(quoted, sizeof(quoted), "\"%s\"", self);
    intptr_t handle = _spawnl(_P_NOWAIT, self, quoted, WORKER_ARGUMENT, (char *)NULL);
    *started = (void *)handle;
    return handle != -1;
#else
    pid_t child = fork();
    if (child == 0) {
        execl(self, self, WORKER_ARGUMENT, (char *)NULL);
        _exit(127);
    }
    *started = (void *)(intptr_t)child;
    return child > 0;
#endif
}

static int finish_worker(void *started) {
    int status = -1;
#if defined(_WIN32)
    return _cwait(&status, (intptr_t)started, 0) == -1 ? -1 : status;
#else
    if (waitpid((pid_t)(intptr_t)started, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static void processes_exclude_each_other(const char *self) {
    write_counter(0);
    void *workers[WORKERS];
    for (int index = 0; index < WORKERS; index++) {
        CHECK(start_worker(self, &workers[index]));
    }
    for (int index = 0; index < WORKERS; index++) {
        CHECK(finish_worker(workers[index]) == 0);
    }
    CHECK(read_counter() == (long)WORKERS * INCREMENTS);
}

static void a_lock_can_be_taken_again_once_released(void) {
    spisor_lock *first = spisor_lock_file(LOCK_PATH);
    CHECK(first != NULL);
    spisor_unlock(first);
    spisor_lock *second = spisor_lock_file(LOCK_PATH);
    CHECK(second != NULL);
    spisor_unlock(second);
}

static void a_lock_in_a_missing_folder_is_refused(void) {
    CHECK(spisor_lock_file(SPISOR_SCRATCH_PATH "/no-such-folder/x.lock") == NULL);
    CHECK(spisor_lock_file(NULL) == NULL);
    spisor_unlock(NULL);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], WORKER_ARGUMENT) == 0) {
        return run_worker();
    }
    a_lock_can_be_taken_again_once_released();
    a_lock_in_a_missing_folder_is_refused();
    processes_exclude_each_other(argv[0]);
    return check_summary("lock_test");
}
