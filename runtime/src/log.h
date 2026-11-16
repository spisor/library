#ifndef RUNTIME_LOG_H
#define RUNTIME_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "runtime/log.h"
#include "spisor/status.h"

/**
 * The ANSI colour code of a log name: the TypeScript palette indexed by its 31-multiplier hash.
 *
 * @implNote The hash runs over the name's UTF-16 code units, as charCodeAt does, so a character
 * outside the BMP counts as two surrogate units. name is valid UTF-8.
 */
uint32_t runtime_name_colour(const char *name);

/** One log line as runtime's C code holds it: `names` and `config` NULL for none, `level` a runtime_log_level. */
typedef struct runtime_log_request {
    const char *home;
    const char *portable;
    const char *plugin;
    const char *start;
    const char *time;
    const char *message;
    const cJSON *names;
    const cJSON *config;
    uint32_t level;
    int no_color;
} runtime_log_request;

/** The console line a write answers, NULL when none is due, in a block freed with runtime_release_storage. */
typedef struct runtime_log_console {
    const char *line;
    void *storage;
} runtime_log_console;

/**
 * Appends the line and answers the console line due, with the rules, statuses and refusals include/runtime/log.h
 * documents. Only an OK answer fills `console`.
 *
 * @implNote A failed append answers a refusal only when no console line is due, so an error is never lost to its own
 * unwritable log, as the TypeScript printed before it wrote.
 */
spisor_status runtime_write_log(const runtime_log_request *entry, runtime_log_console *console,
                                spisor_refusal *refusal);

#endif
