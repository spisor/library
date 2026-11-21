#ifndef RUNTIME_HOST_RUN_H
#define RUNTIME_HOST_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * One run of a loader's plugins as `weivel.host`'s start decided it, held until its stop: the `mode`, a
 * weivel_standard_host_mode; the loader's `host_id`; `started`, what hosts.start answered, whose plan names the run
 * record; `located`, cache.locate's answer over that plan's plugins, its problems the versions it could not run;
 * `libraries`, the plan's libraries as [{binary, declaration}]; and for an ACTION run `action`, {plugin, name,
 * arguments}, the plugin's manifest path, the action's id and the arguments ui.action merged, else NULL.
 */
typedef struct runtime_host_run {
    uint32_t mode;
    char *host_id;
    cJSON *started;
    cJSON *located;
    cJSON *libraries;
    cJSON *action;
} runtime_host_run;

/**
 * Starts the run `arguments` name in `environment`, an object of variable names to values: `<loader id>` serves,
 * `<loader id> action <plugin> <action> <input>` runs one action, the input a JSON object. The layer is the
 * javascript-host at the version its boot hint names, beside every library the hint names; hosts.start plans the run,
 * a SERVE run as the loader's session start; the plan's libraries are the hinted ones with a binary but runtime
 * itself, which the program was started with, and its plugins are those cache.locate finds of the plan's. An ACTION
 * run's plugin must be one of them, and ui.action merges the action's arguments from its manifest. The scopes begin
 * last, over the loader's home and the places.
 *
 * @return the run, freed with runtime_host_run_end; NULL with `*refusal` set to {message, fix} for arguments that name
 * no run, a layer with no hinted version, a refused hosts.start, a plugin the plan does not run, a manifest that cannot
 * be read, an action ui.action refuses, or scopes that cannot begin, or with it NULL when memory runs out. A run that
 * is not answered leaves no run record of its own and no scopes begun, and a refused SERVE run publishes its refusal
 * to the loader's ready record once its places resolved.
 */
runtime_host_run *runtime_host_run_start(const char *const *arguments, size_t argument_count,
                                         const cJSON *environment, cJSON **refusal);

/** Ends the scopes, removes the run record the run's plan wrote and frees the run. NULL does nothing. */
void runtime_host_run_end(runtime_host_run *run);

#endif
