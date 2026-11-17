#ifndef RUNTIME_TEST_PROCESSES_CALL_H
#define RUNTIME_TEST_PROCESSES_CALL_H

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog_call.h"
#include "check.h"
#include "runtime/processes.h"
#include "spisor/status.h"

/**
 * @file
 * Calls the processes definition with a request a test writes as JSON, in the shape processes' operations take inside
 * runtime, and answers what the definition answered as that operation's JSON, so a test reads an answer the way
 * runtime's own callers do. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is
 * the test's data, never the boundary.
 */

typedef enum processes_operation {
    PROCESSES_HOLD,
    PROCESSES_RELEASE,
    PROCESSES_PRUNE,
    PROCESSES_READY,
    PROCESSES_AWAIT_READY,
    PROCESSES_RECORD_RUN,
    PROCESSES_LIVE_RUNS,
    PROCESSES_REMOVE_RUNS
} processes_operation;

/** The texts, pairs and variables a JSON request holds, borrowed from it; freed by processes_request_free. */
typedef struct processes_request {
    const char *state;
    const char *host_id;
    const char **arguments;
    size_t argument_count;
    runtime_variable *environment;
    size_t environment_count;
    runtime_item_version *run;
    size_t run_count;
} processes_request;

static inline const char *processes_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline double processes_number(const cJSON *object, const char *name) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(member) ? member->valuedouble : 0;
}

static inline void processes_request_read(const cJSON *request, processes_request *read) {
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(request, "start");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(start, "arguments");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(start == NULL ? request : start, "environment");
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(request, "run");
    const cJSON *entry;
    memset(read, 0, sizeof *read);
    read->state = processes_text(request, "state");
    read->host_id = processes_text(request, "hostId");
    read->arguments = calloc((size_t)cJSON_GetArraySize(arguments) + 1, sizeof *read->arguments);
    read->environment = calloc((size_t)cJSON_GetArraySize(environment) + 1, sizeof *read->environment);
    read->run = calloc((size_t)cJSON_GetArraySize(run) + 1, sizeof *read->run);
    CHECK(read->arguments != NULL && read->environment != NULL && read->run != NULL);
    cJSON_ArrayForEach(entry, arguments) {
        read->arguments[read->argument_count++] = cJSON_GetStringValue(entry);
    }
    cJSON_ArrayForEach(entry, environment) {
        read->environment[read->environment_count].name = entry->string;
        read->environment[read->environment_count++].value = cJSON_GetStringValue(entry);
    }
    cJSON_ArrayForEach(entry, run) {
        read->run[read->run_count].id = entry->string;
        read->run[read->run_count++].version = cJSON_GetStringValue(entry);
    }
}

static inline void processes_request_free(processes_request *read) {
    free((void *)read->arguments);
    free(read->environment);
    free(read->run);
}

static inline void processes_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        CHECK(cJSON_AddStringToObject(object, name, text) != NULL);
    }
}

static inline spisor_status processes_hold_call(const runtime_processes_definition *processes, const cJSON *request,
                                                const processes_request *read, cJSON **tree,
                                                spisor_refusal *refusal) {
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(request, "start");
    runtime_processes_hold_argument argument = SPISOR_INIT(runtime_processes_hold_argument);
    runtime_processes_hold_answer answer = SPISOR_INIT(runtime_processes_hold_answer);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.holder = (int64_t)processes_number(request, "holder");
    argument.executable = processes_text(start, "executable");
    argument.arguments = read->arguments;
    argument.argument_count = read->argument_count;
    argument.environment = cJSON_HasObjectItem(start, "environment") ? read->environment : NULL;
    argument.environment_count = read->environment_count;
    argument.directory = processes_text(start, "directory");
    status = processes->hold(processes->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddNumberToObject(*tree, "pid", (double)answer.pid);
        cJSON_AddBoolToObject(*tree, "started", answer.started != 0);
        if (answer.started) {
            cJSON_AddBoolToObject(*tree, "outsideJob", answer.outside_job != 0);
        }
    }
    processes->release_hold_answer(processes->context, &answer);
    return status;
}

static inline spisor_status processes_release_call(const runtime_processes_definition *processes,
                                                   const cJSON *request, const processes_request *read, cJSON **tree,
                                                   spisor_refusal *refusal) {
    runtime_processes_holder_argument argument = SPISOR_INIT(runtime_processes_holder_argument);
    runtime_processes_release_answer answer = SPISOR_INIT(runtime_processes_release_answer);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.holder = (int64_t)processes_number(request, "holder");
    status = processes->release(processes->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *holders = cJSON_CreateArray();
        for (size_t index = 0; index < answer.holder_count; index++) {
            CHECK(cJSON_AddItemToArray(holders, cJSON_CreateNumber((double)answer.holders[index])));
        }
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "holders", holders);
    }
    processes->release_release_answer(processes->context, &answer);
    return status;
}

static inline spisor_status processes_prune_call(const runtime_processes_definition *processes, const cJSON *request,
                                                 const processes_request *read, cJSON **tree,
                                                 spisor_refusal *refusal) {
    runtime_processes_prune_argument argument = SPISOR_INIT(runtime_processes_prune_argument);
    runtime_processes_prune_answer answer = SPISOR_INIT(runtime_processes_prune_answer);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.self = (int64_t)processes_number(request, "self");
    argument.now = processes_number(request, "now");
    argument.check_milliseconds = processes_number(request, "checkMilliseconds");
    status = processes->prune(processes->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddBoolToObject(*tree, "exit", answer.exit != 0);
    }
    processes->release_prune_answer(processes->context, &answer);
    return status;
}

static inline spisor_status processes_ready_call(const runtime_processes_definition *processes, const cJSON *request,
                                                 const processes_request *read, cJSON **tree,
                                                 spisor_refusal *refusal) {
    runtime_processes_ready_argument argument = SPISOR_INIT(runtime_processes_ready_argument);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.pid = (int64_t)processes_number(request, "pid");
    argument.environment = cJSON_HasObjectItem(request, "environment") ? read->environment : NULL;
    argument.environment_count = read->environment_count;
    argument.refused = processes_text(request, "refused");
    argument.fix = processes_text(request, "fix");
    status = processes->ready(processes->context, &argument, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
    }
    return status;
}

static inline spisor_status processes_await_call(const runtime_processes_definition *processes, const cJSON *request,
                                                 const processes_request *read, cJSON **tree,
                                                 spisor_refusal *refusal) {
    runtime_processes_await_argument argument = SPISOR_INIT(runtime_processes_await_argument);
    runtime_processes_await_answer answer = SPISOR_INIT(runtime_processes_await_answer);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.pid = (int64_t)processes_number(request, "pid");
    argument.timeout_milliseconds = (int64_t)processes_number(request, "timeoutMilliseconds");
    status = processes->await_ready(processes->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *environment;
        *tree = cJSON_CreateObject();
        environment = cJSON_AddObjectToObject(*tree, "environment");
        for (size_t index = 0; index < answer.environment_count; index++) {
            processes_add_text(environment, answer.environment[index].name, answer.environment[index].value);
        }
    }
    processes->release_await_answer(processes->context, &answer);
    return status;
}

static inline spisor_status processes_record_call(const runtime_processes_definition *processes,
                                                  const cJSON *request, const processes_request *read, cJSON **tree,
                                                  spisor_refusal *refusal) {
    runtime_processes_record_argument argument = SPISOR_INIT(runtime_processes_record_argument);
    runtime_processes_record_answer answer = SPISOR_INIT(runtime_processes_record_answer);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.run = read->run;
    argument.run_count = read->run_count;
    argument.check_milliseconds = processes_number(request, "checkMilliseconds");
    argument.record = processes_text(request, "record");
    status = processes->record_run(processes->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        processes_add_text(*tree, "record", answer.record);
    }
    processes->release_record_answer(processes->context, &answer);
    return status;
}

static inline spisor_status processes_live_call(const runtime_processes_definition *processes, const cJSON *request,
                                                const processes_request *read, cJSON **tree,
                                                spisor_refusal *refusal) {
    runtime_processes_live_argument argument = SPISOR_INIT(runtime_processes_live_argument);
    runtime_processes_live_answer answer = SPISOR_INIT(runtime_processes_live_answer);
    spisor_status status;
    argument.state = read->state;
    argument.now = processes_number(request, "now");
    status = processes->live_runs(processes->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *runs = cJSON_CreateArray();
        for (size_t index = 0; index < answer.run_count; index++) {
            cJSON *run = cJSON_CreateObject();
            const runtime_processes_run *each_run = &answer.runs[index];
            for (size_t each = 0; each < each_run->version_count; each++) {
                processes_add_text(run, each_run->versions[each].id, each_run->versions[each].version);
            }
            CHECK(cJSON_AddItemToArray(runs, run));
        }
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "runs", runs);
    }
    processes->release_live_answer(processes->context, &answer);
    return status;
}

static inline spisor_status processes_remove_call(const runtime_processes_definition *processes,
                                                  const cJSON *request, const processes_request *read, cJSON **tree,
                                                  spisor_refusal *refusal) {
    runtime_processes_remove_argument argument = SPISOR_INIT(runtime_processes_remove_argument);
    spisor_status status;
    argument.state = read->state;
    argument.host_id = read->host_id;
    argument.record = processes_text(request, "record");
    status = processes->remove_runs(processes->context, &argument, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
    }
    return status;
}

typedef spisor_status (*processes_caller)(const runtime_processes_definition *processes, const cJSON *request,
                                          const processes_request *read, cJSON **tree, spisor_refusal *refusal);

/** Calls `operation` with `request`, which it frees, answering the definition's status and the answer as JSON. */
static inline catalog_answer processes_call(processes_operation operation, cJSON *request) {
    static const processes_caller callers[] = {processes_hold_call,  processes_release_call, processes_prune_call,
                                               processes_ready_call, processes_await_call,   processes_record_call,
                                               processes_live_call,  processes_remove_call};
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_answer result = {0, NULL};
    processes_request read;
    spisor_status status;
    processes_request_read(request, &read);
    status = callers[operation](processes, request, &read, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cJSON_CreateObject();
        processes_add_text(result.tree, "message", refusal.detail);
        processes_add_text(result.tree, "fix", refusal.fix);
    }
    processes->release_refusal(processes->context, &refusal);
    processes_request_free(&read);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
