#ifndef RUNTIME_PROCESS_RUN_H
#define RUNTIME_PROCESS_RUN_H

#include "cJSON.h"

/**
 * Runs the absolute executable with arguments, an array of strings or NULL, its standard streams on the
 * null device and the caller's environment, and waits for it to exit: 1 with *exit_code set, 0 with
 * *problem set (freed with free) when it could not be started or waited for.
 */
int runtime_run_to_exit(const char *executable, const cJSON *arguments, int *exit_code, char **problem);

#endif
