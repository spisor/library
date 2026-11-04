#ifndef HOST_CHILD_H
#define HOST_CHILD_H

#include "cJSON.h"

/** Told the pid of the child once it runs. */
typedef void (*host_child_started)(double pid, void *context);

/** What a run tells its caller: started with the pid once the child runs, and tick every interval while it does. */
typedef struct host_child_watch {
    host_child_started started;
    void (*tick)(void *context);
    unsigned interval_milliseconds;
    void *context;
} host_child_watch;

/**
 * Runs executable with arguments, an array of strings, as this process's child sharing its terminal and
 * standard streams, under environment (this process's own when NULL), and answers 1 with its exit code in
 * *exit_code: 128 plus the signal's number when a signal ended it. watch, when not NULL, is told the pid and
 * ticked while the child runs. 0 with *refusal set when it could not be started, or with none when memory ran out.
 *
 * @implNote While the child runs this process outlives every stop the terminal sends: SIGINT and SIGHUP
 * reach the child from the terminal itself, so only a SIGTERM, which is sent to this process alone, is
 * passed on; on Windows the console delivers Ctrl+C to the child too. A .cmd or .bat file runs through the
 * command shell, which Windows needs to start one.
 */
int host_run_child(const char *executable, const cJSON *arguments, const cJSON *environment, const host_child_watch *watch,
                   int *exit_code, cJSON **refusal);

/**
 * Runs executable with arguments in this process's place under this process's environment and never
 * returns, but with the refusal when it cannot start: execv on POSIX, so the pid stays; on Windows, where a
 * process cannot be replaced, as host_run_child, exiting with its code.
 */
cJSON *host_replace_process(const char *executable, const cJSON *arguments);

#endif
