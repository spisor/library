#ifndef RUNTIME_PROCESS_RUN_H
#define RUNTIME_PROCESS_RUN_H

#include "cJSON.h"

/** At most this much of a run's standard output is kept; the rest is read and dropped. */
#define RUNTIME_RUN_OUTPUT_LIMIT 4096

/**
 * Runs the absolute executable with arguments, an array of strings or NULL, under the caller's
 * environment with its standard input and error on the null device, and waits for it to exit: 1 with
 * *exit_code set, 0 with *problem set (freed with free) when it could not be started, or did not exit
 * within timeout_milliseconds and was terminated. With output not NULL its standard output, up to
 * RUNTIME_RUN_OUTPUT_LIMIT bytes, is answered there (freed with free); without, it goes to the null
 * device too.
 *
 * @implNote The child inherits only its three standard handles on Windows (a handle list, as
 * process_start.c starts a host), so a run never carries another of the caller's handles away.
 */
int runtime_run_to_exit(const char *executable, const cJSON *arguments, unsigned timeout_milliseconds, int *exit_code,
                        char **output, char **problem);

#endif
