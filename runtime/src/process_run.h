#ifndef RUNTIME_PROCESS_RUN_H
#define RUNTIME_PROCESS_RUN_H

#include "cJSON.h"

/** At most this much of a run's standard output is kept; the rest is read and dropped. */
#define RUNTIME_RUN_OUTPUT_LIMIT 4096

/**
 * Runs the absolute executable with arguments, an array of strings or NULL, under the caller's
 * environment with its standard input and error on the null device, and waits for it to exit: 1 with
 * *exit_code set, 0 with *problem set (freed) when it could not be started, or did not exit
 * within timeout_milliseconds and was terminated, and 0 with *problem NULL when memory ran out. With
 * output not NULL its standard output, up to
 * RUNTIME_RUN_OUTPUT_LIMIT bytes, is answered there (freed); without, it goes to the null
 * device too.
 *
 * @implNote The child inherits only its three standard handles on Windows (a handle list, as
 * process_start.c starts a host), so a run never carries another of the caller's handles away.
 */
int runtime_run_to_exit(const char *executable, const cJSON *arguments, unsigned timeout_milliseconds, int *exit_code,
                        char **output, char **problem);

/**
 * runtime_run_to_exit with the file input as the run's standard input and its standard output written
 * to the file output, created or emptied first; no output is answered.
 *
 * @implNote Both files are opened here, through the wide APIs on Windows, so a path the child could not
 * take as an argument never reaches its command line: System32's bsdtar reads its arguments in the ANSI
 * codepage, so a letter outside it, or a path deeper than MAX_PATH, is refused there.
 */
int runtime_run_between_files(const char *executable, const cJSON *arguments, unsigned timeout_milliseconds,
                              const char *input, const char *output, int *exit_code, char **problem);

/**
 * Runs the absolute executable with arguments, an array of strings, under environment, an object of strings that
 * replaces the caller's, its standard input and output on the null device, and waits for it to exit however long it
 * runs: 1 with *exit_code set and its standard error, up to RUNTIME_RUN_OUTPUT_LIMIT bytes, answered in *errors
 * (freed); 0 with *problem set (freed) when it could not be started, and with *problem NULL when memory ran out.
 *
 * @implNote On Windows it starts with CREATE_NO_WINDOW, as every run here does, so a caller with no console, such as an
 * interface started as a window, opens no console window for it.
 */
int runtime_run_reporting_errors(const char *executable, const cJSON *arguments, const cJSON *environment,
                                 int *exit_code, char **errors, char **problem);

#endif
