#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "catalog_time.h"
#include "definition_support.h"
#include "file_times.h"
#include "log.h"
#include "log_writer.h"
#include "plugin_scope.h"
#include "spisor/definition.h"
#include "spisor/json_value.h"
#include "standard_refusal.h"

/** One entry waiting for the writer, in one block with its `line`, holding a reference to the scope it came through. */
typedef struct scope_log_entry {
    runtime_log_queued queued;
    runtime_plugin_scope *scope;
    uint32_t level;
    char time[RUNTIME_CATALOG_TIME_SIZE];
    char *line;
} scope_log_entry;

static runtime_log_request request_of(const scope_log_entry *entry) {
    const runtime_plugin_scope *scope = entry->scope;
    runtime_log_request request;
    memset(&request, 0, sizeof request);
    request.home = scope->host.home;
    request.portable = scope->host.portable;
    request.plugin = scope->manifest.id;
    request.start = scope->host.start;
    request.time = entry->time;
    request.message = entry->line;
    request.names = scope->host.names;
    request.config = scope->manifest.config;
    request.level = entry->level;
    request.no_color = scope->host.no_color != 0;
    return request;
}

/**
 * Writes one entry on the writer's thread and frees it. Nothing is left to tell of a line runtime's log refused: it
 * answers a console line instead for an error, which is printed here.
 */
static void deliver(runtime_log_queued *queued) {
    scope_log_entry *entry = (scope_log_entry *)queued;
    runtime_plugin_scope *scope = entry->scope;
    runtime_log_request request = request_of(entry);
    runtime_log_console console = {NULL, NULL};
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    if (runtime_write_log(&request, &console, &refusal) == SPISOR_OK && console.line != NULL) {
        fprintf(stderr, "%s\n", console.line);
    }
    runtime_release_storage(console.storage);
    spisor_refusal_release(&refusal);
    spisor_memory_release(entry);
    runtime_plugin_scope_release(scope);
}

static spisor_status refuse_call(spisor_refusal *refusal, const char *problem, const char *fix) {
    char detail[160];
    snprintf(detail, sizeof detail, "log's write was given %s", problem);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

/** The refusal of a call services.h does not allow, or OK. */
static spisor_status check_call(uint32_t level, const char *message, const weivel_standard_value *fields,
                                spisor_refusal *refusal) {
    if (level < WEIVEL_STANDARD_LOG_TRACE || level > WEIVEL_STANDARD_LOG_ERROR) {
        return refuse_call(refusal, "a level it does not know", "pass a weivel_standard_log_level, 1 to 5");
    }
    if (message == NULL) {
        return refuse_call(refusal, "no message", "pass the entry's message as text");
    }
    if (fields != NULL && fields->kind != WEIVEL_STANDARD_VALUE_RECORD) {
        return refuse_call(refusal, "fields that are not a record", "pass the fields as a RECORD, or NULL for none");
    }
    return SPISOR_OK;
}

/**
 * The fields as the compact JSON the line carries after its message, NULL for none.
 *
 * @implNote weivel_standard_value and spisor_value are one layout, which restatement_test holds, so the standard's
 * tree is read as runtime's own.
 */
static spisor_status fields_text(const weivel_standard_value *fields, char **text, spisor_refusal *refusal) {
    cJSON *json = NULL;
    spisor_status status;
    *text = NULL;
    if (fields == NULL || fields->count == 0) {
        return SPISOR_OK;
    }
    status = spisor_json_of_value((const spisor_value *)fields, &json);
    if (status == SPISOR_MALFORMED) {
        return refuse_call(refusal, "fields that are not a well-formed value tree",
                           "pass the fields as weivel/value.h defines a RECORD");
    }
    if (status == SPISOR_OK) {
        *text = cJSON_PrintUnformatted(json);
        status = *text == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    }
    cJSON_Delete(json);
    return status;
}

static scope_log_entry *new_entry(runtime_plugin_scope *scope, uint32_t level, const char *message,
                                  const char *fields) {
    size_t message_length = strlen(message);
    size_t fields_length = fields == NULL ? 0 : strlen(fields) + 1;
    scope_log_entry *entry = spisor_memory_allocate(sizeof *entry + message_length + fields_length + 1);
    if (entry == NULL) {
        return NULL;
    }
    memset(entry, 0, sizeof *entry);
    entry->queued.deliver = deliver;
    entry->scope = scope;
    entry->level = level - WEIVEL_STANDARD_LOG_DEBUG;
    entry->line = (char *)(entry + 1);
    memcpy(entry->line, message, message_length);
    if (fields != NULL) {
        entry->line[message_length] = ' ';
        memcpy(entry->line + message_length + 1, fields, fields_length - 1);
    }
    entry->line[message_length + fields_length] = '\0';
    return entry;
}

static spisor_status refuse_clock(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "the clock reads outside the years 0 to 9999",
                            "set the system clock, then write again");
}

static spisor_status refuse_hand_off(runtime_log_hand_off handed, spisor_refusal *refusal) {
    if (handed == RUNTIME_LOG_FULL) {
        return spisor_refuse_as(refusal, SPISOR_REFUSED,
                                "the log's writer already holds every entry it can, so this one was not kept",
                                "write fewer entries at once; the writer takes more as it writes them");
    }
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "no host is started, so the log's writer is not running",
                            "write while the host started through weivel.host runs");
}

/** Hands a stamped entry to the writer, which then holds it and a reference to the scope. */
static spisor_status hand_off(scope_log_entry *entry, spisor_refusal *refusal) {
    runtime_log_hand_off handed;
    if (!runtime_catalog_format_time(runtime_now_milliseconds(), entry->time)) {
        spisor_memory_release(entry);
        return refuse_clock(refusal);
    }
    runtime_plugin_scope_hold(entry->scope);
    handed = runtime_log_writer_hand(&entry->queued);
    if (handed == RUNTIME_LOG_TAKEN) {
        return SPISOR_OK;
    }
    runtime_plugin_scope_release(entry->scope);
    spisor_memory_release(entry);
    return refuse_hand_off(handed, refusal);
}

static spisor_status write_kept(runtime_plugin_scope *scope, uint32_t level, const char *message,
                                const weivel_standard_value *fields, spisor_refusal *refusal) {
    char *fields_line;
    scope_log_entry *entry;
    spisor_status status = fields_text(fields, &fields_line, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    entry = new_entry(scope, level, message, fields_line);
    cJSON_free(fields_line);
    return entry == NULL ? SPISOR_OUT_OF_MEMORY : hand_off(entry, refusal);
}

/** @implNote runtime's log keeps no trace level, so a TRACE entry is dropped, as services.h allows. */
static weivel_status write_entry(void *context, uint32_t level, const char *message,
                                 const weivel_standard_value *fields, weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    status = check_call(level, message, fields, &filled);
    if (status == SPISOR_OK && level != WEIVEL_STANDARD_LOG_TRACE) {
        status = write_kept(context, level, message, fields, &filled);
    }
    return runtime_answer_service(status, &filled, refusal);
}

void runtime_scope_log_fill(runtime_plugin_scope *scope) {
    weivel_standard_log *log = &scope->log;
    log->size = (uint32_t)sizeof *log;
    log->context = scope;
    log->write = write_entry;
    log->release_refusal = runtime_release_service_refusal;
}
