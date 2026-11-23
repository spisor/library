#ifndef RUNTIME_HOST_RUN_H
#define RUNTIME_HOST_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** The exit code of a run that failed: a plugin not activated, an action that threw or never ran, or a failed call. */
#define RUNTIME_HOST_EXIT_FAILED 1u

/** How far a run has come: planned by its start, running once its started answered OK, then ended. */
typedef enum runtime_host_phase { RUNTIME_HOST_PLANNED, RUNTIME_HOST_RUNNING, RUNTIME_HOST_ENDED } runtime_host_phase;

/**
 * A SERVE run's timers: when its next turn (the run record's stamp and, unless it runs in-process, the holders' prune) and its next update check
 * are due, both due before its first tick, and how many turns in a row have failed.
 */
typedef struct runtime_host_timers {
    uint32_t ticked;
    int64_t next_turn;
    int64_t next_check;
    uint32_t failed_turns;
} runtime_host_timers;

/**
 * One run of a loader's plugins as `weivel.host`'s start decided it, held until its stop: the `mode`, a
 * weivel_standard_host_mode; the loader's `host_id`; `started`, what hosts.start answered, whose plan names the run
 * record; `located`, cache.locate's answer over that plan's plugins, its problems the versions it could not run;
 * `libraries`, the plan's libraries as [{binary, declaration}]; for an ACTION run `action`, {plugin, name, service,
 * method, arguments}, the plugin's manifest path, the action's id, the service and method its declared call names and
 * the arguments ui.action merged, else NULL; its `phase`, a
 * runtime_host_phase; the `exit_code` its stop answers, 0 for a SERVE run and RUNTIME_HOST_EXIT_FAILED for an ACTION
 * run until its action answered; its `timers`; and `in_process`, 1 for a SERVE run that serves the process it runs
 * in alone, as a host started inside another application's process does, which never reads or writes the loader's
 * holders and ready records, so any number of such runs serve at once, each in its own process.
 */
typedef struct runtime_host_run {
    uint32_t mode;
    char *host_id;
    cJSON *started;
    cJSON *located;
    cJSON *libraries;
    cJSON *action;
    uint32_t phase;
    uint32_t exit_code;
    runtime_host_timers timers;
    uint32_t in_process;
} runtime_host_run;

/**
 * Starts the run `arguments` name in `environment`, an object of variable names to values: `<loader id>` serves,
 * `<loader id> in-process` serves in-process, `<loader id> action <plugin> <action> <input>` runs one action, the
 * input a JSON object. The layer is the
 * javascript-host at the version its boot hint names, beside every library the hint names; hosts.start plans the run,
 * a SERVE run as the loader's session start; the plan's libraries are the hinted ones with a binary but runtime
 * itself, which the program was started with, and its plugins are those cache.locate finds of the plan's. An ACTION
 * run's plugin must be one of them, and ui.action merges the action's arguments from its manifest. The scopes begin
 * last, over the loader's home and the places.
 *
 * @return the run, freed with runtime_host_run_end; NULL with `*refusal` set to {message, fix} for arguments that name
 * no run, a layer with no hinted version, a refused hosts.start, a located plugin with no entry, a plugin the plan does
 * not run, a manifest that cannot be read, an action ui.action refuses, or scopes that cannot begin, or with it NULL
 * when memory runs out. A run that is not answered leaves no run record of its own and no scopes begun, and a refused
 * SERVE run that is not in-process publishes its refusal to the loader's ready record once its places resolved.
 */
runtime_host_run *runtime_host_run_start(const char *const *arguments, size_t argument_count,
                                         const cJSON *environment, cJSON **refusal);

/** Ends the scopes, removes the run record the run's plan wrote and frees the run. NULL does nothing. */
void runtime_host_run_end(runtime_host_run *run);

#endif
