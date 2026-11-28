#ifndef RUNTIME_PROCESS_START_H
#define RUNTIME_PROCESS_START_H

#include "cJSON.h"

/**
 * What console a detached start has on Windows: NONE, for a background host, or HIDDEN, a console of its own that never
 * shows a window, which every console child it starts in turn shares, as an interface's children must. POSIX has no
 * consoles and reads neither.
 */
typedef enum runtime_detached_console {
    RUNTIME_DETACHED_NO_CONSOLE = 0,
    RUNTIME_DETACHED_HIDDEN_CONSOLE = 1
} runtime_detached_console;

/**
 * How to start a detached process: an absolute executable, its arguments, its environment, where it runs and its
 * console.
 */
typedef struct runtime_detached_start {
    const char *executable;
    const cJSON *arguments;
    const cJSON *environment;
    const char *directory;
    runtime_detached_console console;
} runtime_detached_start;

/**
 * A started host held before its program runs: its pid, whether it runs outside the caller's Windows job, 0 only when
 * that job forbade breaking away, and what lets it run or ends it, which only this file reads.
 */
typedef struct runtime_detached_host {
    double pid;
    int outside_job;
    const char *executable;
    void *process;
    void *thread;
    int go;
    int failure;
} runtime_detached_host;

/**
 * Starts the process detached, so it outlives the caller with no terminal of its own and its standard streams on
 * the null device, held before its program runs until runtime_release_detached or runtime_cancel_detached, one of
 * which the caller always calls. 1 with *host filled; 0 with *problem set (freed) when it could not start,
 * and NULL when memory ran out. arguments is an array of strings or NULL; environment, an object of strings, replaces
 * the caller's when not NULL.
 *
 * @implNote The hold lets a caller record the host before it runs and end it unrun when that record fails, through
 * what it holds rather than by a pid another process may have taken. On Windows the process is created suspended in
 * a new process group (CREATE_NEW_PROCESS_GROUP) with no console (DETACHED_PROCESS) or a hidden one (CREATE_NO_WINDOW),
 * as its console asks, breaking away from the caller's job (CREATE_BREAKAWAY_FROM_JOB): a console child of a process
 * with no console gets a console window of its own, one of a hidden console shares it; a job that forbids breakaway refuses that, so it starts again inside the job, where
 * closing the job takes it down. On POSIX a double fork with setsid, so it is no child of the caller, and the host
 * waits on a pipe before it execs: init reaps it, and nothing the caller's session or process group receives reaches
 * it.
 */
int runtime_start_detached(const runtime_detached_start *start, runtime_detached_host *host, char **problem);

/**
 * Lets the held host run its program. 1; 0 with *problem set when its program could not run, which on POSIX is only
 * known now, the host having ended, and with *problem NULL when memory ran out describing it.
 */
int runtime_release_detached(runtime_detached_host *host, char **problem);

/** Ends the held host without its program ever running. */
void runtime_cancel_detached(runtime_detached_host *host);

#endif
