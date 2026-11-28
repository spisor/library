#ifndef RUNTIME_PROCESSES_CHILD_START_H
#define RUNTIME_PROCESSES_CHILD_START_H

#include "cJSON.h"

/**
 * One declared action to run as a child: `loader_id`'s `plugin` running `action` with `input`, a JSON object's text,
 * in `environment`, an object of variable names to values, whose places are `cache` and `state`.
 */
typedef struct runtime_child_action_request {
    const char *cache;
    const char *state;
    const cJSON *environment;
    const char *loader_id;
    const char *plugin;
    const char *action;
    const char *input;
} runtime_child_action_request;

/**
 * processes.run_action: {exitCode, result?, failure?}. The start is the ACTION layer hosts.layer_start answers, given a
 * result path runtime_fresh_result_path draws under the state's results folder; the child runs as
 * runtime_run_reporting_errors runs one, owning its standard handles, its environment the request's and, on Windows,
 * CREATE_NO_WINDOW. Once it exits the result file is read when it exited 0 and deleted either way; `result` is the
 * value it holds, absent when the run wrote none, and `failure`, present exactly when the exit code is not 0, the end
 * of the child's standard error as runtime_run_reporting_errors answers it, trimmed, or a sentence naming the exit
 * code, unsigned and on Windows from 0x80000000 also in hex, when that is empty. NULL with *refusal set when
 * the environment's places are not `cache` and `state`, no free result path can be drawn, the start
 * is refused as hosts.layer_start refuses it, the child cannot be started, its start's environment holds a value that
 * is no text, or its result cannot be read as JSON; NULL
 * with it NULL when memory runs out, the result file removed whenever the child ran as runtime_take_result removes it.
 */
cJSON *runtime_processes_run_action(const runtime_child_action_request *request, cJSON **refusal);

#endif
