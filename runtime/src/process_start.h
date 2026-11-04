#ifndef RUNTIME_PROCESS_START_H
#define RUNTIME_PROCESS_START_H

#include "cJSON.h"

/** How to start a background host: an absolute executable, its arguments, its environment and where it runs. */
typedef struct runtime_detached_start {
    const char *executable;
    const cJSON *arguments;
    const cJSON *environment;
    const char *directory;
} runtime_detached_start;

/**
 * Starts the process detached, so it outlives the caller with no terminal of its own and its standard
 * streams on the null device, and answers 1 with its pid in *pid; 0 with *problem set (freed with free)
 * when it could not start. arguments is an array of strings or NULL; environment, an object of strings,
 * replaces the caller's when not NULL. *outside_job is 0 only when it had to start inside the caller's
 * Windows job object.
 *
 * @implNote On Windows a new process group with no console (CREATE_NEW_PROCESS_GROUP, DETACHED_PROCESS)
 * that breaks away from the caller's job (CREATE_BREAKAWAY_FROM_JOB); a job that forbids breakaway
 * refuses that, so it starts again inside the job, where closing the job takes it down. On POSIX a
 * double fork with setsid, so it is no child of the caller: init reaps it, and nothing the caller's
 * session or process group receives reaches it.
 */
int runtime_start_detached(const runtime_detached_start *start, double *pid, int *outside_job, char **problem);

#endif
