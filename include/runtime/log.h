#ifndef RUNTIME_PUBLIC_LOG_H
#define RUNTIME_PUBLIC_LOG_H

/**
 * @file
 * runtime's `log` service: appends a plugin's log line to the day's file under the home's logs folder and answers the
 * console line due, if any. A size-prefixed struct is read only up to its `size`. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the provider's until the
 * release for its kind.
 */

#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A line's level. Append-only: a published value keeps its number. */
typedef enum runtime_log_level {
    RUNTIME_LOG_DEBUG = 0,
    RUNTIME_LOG_INFO = 1,
    RUNTIME_LOG_WARN = 2,
    RUNTIME_LOG_ERROR = 3
} runtime_log_level;

/**
 * One line. `home`, `names`, `portable`, `plugin` and `config` are as runtime/config.h's runtime_config_plugin has
 * them, and name the file after the plugin's config name. `start` (the session's start, naming the file) and `time`
 * are 24 character toISOString times; `level` is a runtime_log_level; `no_color` 1 leaves the console line plain.
 * First shape ends at `no_color`.
 */
typedef struct runtime_log_entry {
    uint32_t size;
    const char *home;
    const spisor_value *names;
    const char *portable;
    const char *plugin;
    const spisor_value *config;
    const char *start;
    const char *time;
    uint32_t level;
    const char *message;
    uint32_t no_color;
} runtime_log_entry;

/** The console line due, NULL when none is. First shape ends at `storage`. */
typedef struct runtime_log_answer {
    uint32_t size;
    const char *console;
    void *storage;
} runtime_log_answer;

/** The log service. First shape ends at `release_refusal`. */
typedef struct runtime_log_definition {
    uint32_t size;
    void *context;
    /**
     * Appends the line unless the merged config sets `logging` false, and answers a console line for an error, or
     * for any line when the ecosystem settings set `logConsole`; it is coloured unless `logColor` is false.
     * Unreadable settings leave console lines off and colour on.
     *
     * @param answer the consumer's empty answer. Required.
     * @param refusal the consumer's empty refusal, or NULL for none.
     * @return OK; MALFORMED for an entry without its texts, a level it does not know, a time that is not a
     * toISOString time, a home or portable place that is not absolute, `names` or `config` that is not a record, or a
     * config name or plugin id that is not one portable path segment; REFUSED when the line could not be appended and
     * no console line is due, so an error is never lost to its own unwritable log; OUT_OF_MEMORY.
     */
    spisor_status (*write)(void *context, const runtime_log_entry *entry, runtime_log_answer *answer,
                           spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_log_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_log_definition;

/** The static runtime_log_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_log_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
