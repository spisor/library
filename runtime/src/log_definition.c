#include "runtime/log.h"

#include <stdlib.h>
#include <string.h>

#include "config_conversion.h"
#include "definition_support.h"
#include "log.h"

#define ENTRY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_log_entry, no_color)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_log_answer, storage)

static spisor_status fill_answer(const char *console, runtime_log_answer *answer) {
    const char *packed;
    if (!runtime_pack_texts(&console, &packed, 1, &answer->storage)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->console = packed;
    return SPISOR_OK;
}

static void fill_request(const runtime_log_entry *entry, const cJSON *names, const cJSON *config,
                         runtime_log_request *request) {
    request->home = entry->home;
    request->portable = entry->portable;
    request->plugin = entry->plugin;
    request->start = entry->start;
    request->time = entry->time;
    request->message = entry->message;
    request->names = names;
    request->config = config;
    request->level = entry->level;
    request->no_color = entry->no_color != 0;
}

static spisor_status write_line(void *context, const runtime_log_entry *entry, runtime_log_answer *answer,
                                spisor_refusal *refusal) {
    runtime_log_request request;
    cJSON *names = NULL;
    cJSON *config = NULL;
    char *console = NULL;
    spisor_status status;
    (void)context;
    if (entry == NULL || !runtime_has_shape(entry->size, ENTRY_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "log's write");
    }
    status = runtime_record_json(entry->names, "names", &names, refusal);
    if (status == SPISOR_OK) {
        status = runtime_record_json(entry->config, "config", &config, refusal);
    }
    if (status == SPISOR_OK) {
        fill_request(entry, names, config, &request);
        status = runtime_write_log(&request, &console, refusal);
    }
    if (status == SPISOR_OK) {
        status = fill_answer(console, answer);
    }
    free(console);
    cJSON_Delete(config);
    cJSON_Delete(names);
    return status;
}

static void release_answer(void *context, runtime_log_answer *answer) {
    (void)context;
    if (answer == NULL || !runtime_has_shape(answer->size, ANSWER_SHAPE)) {
        return;
    }
    runtime_release_storage(answer->storage);
    RUNTIME_CLEAR_ANSWER(answer, ANSWER_SHAPE);
}

static const runtime_log_definition definition = {
    sizeof(runtime_log_definition), NULL, write_line, release_answer, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_log_definition_of(void) {
    return &definition;
}
