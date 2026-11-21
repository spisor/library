#ifndef RUNTIME_SCOPE_HOST_H
#define RUNTIME_SCOPE_HOST_H

#include <stdint.h>

#include "cJSON.h"
#include "catalog_time.h"
#include "spisor/status.h"

/**
 * What every plugin's scope reads of the host it opens in, as the host's start learns it: the application's `home`,
 * absolute; its `names` declaration, a record, or NULL; the `portable` place, absolute, or NULL for a host with no
 * portable layer; the `cache` place and the `secrets_file`, both absolute, and `force_file_store` and `no_color`,
 * each 1 or 0, as places' resolve and hosts' start answer them. Each scope keeps its own copy, so a scope outlives the
 * host's end.
 */
typedef struct runtime_scope_host {
    const char *home;
    const cJSON *names;
    const char *portable;
    const char *cache;
    const char *secrets_file;
    uint32_t force_file_store;
    uint32_t no_color;
} runtime_scope_host;

/**
 * A copy of a runtime_scope_host its holder owns, freed with runtime_scope_host_free, with the `start` of the host's
 * session, the toISOString time its begin read, which names the session's log files.
 */
typedef struct runtime_owned_scope_host {
    char *home;
    cJSON *names;
    char *portable;
    char *cache;
    char *secrets_file;
    uint32_t force_file_store;
    uint32_t no_color;
    char start[RUNTIME_CATALOG_TIME_SIZE];
} runtime_owned_scope_host;

/**
 * Starts the host every scope opened from now on opens in, replacing any started before, and starts the log's writer
 * when it is not running (log_writer.h). Safe from any thread.
 *
 * @return OK; MALFORMED for a home, portable place, cache place or secrets file that is not absolute, or names that
 * are not a record, with nothing replaced; REFUSED when the clock reads outside the years 0 to 9999 or the log's
 * writer cannot start, with nothing replaced; OUT_OF_MEMORY, with nothing replaced.
 */
spisor_status runtime_scopes_begin(const runtime_scope_host *host);

/**
 * Ends the host, so no scope opens until the next begin, and stops the log's writer once it has written every entry
 * handed to it; the scopes already open keep their copies, and their log refuses until the next begin.
 */
void runtime_scopes_end(void);

/**
 * A copy of the started host into `*copy`.
 *
 * @return OK; NONE when no host is started; OUT_OF_MEMORY. `*copy` holds nothing unless the answer is OK.
 */
spisor_status runtime_scopes_host(runtime_owned_scope_host *copy);

/** Frees what a copy holds and leaves it empty; an empty copy is left as it is. */
void runtime_scope_host_free(runtime_owned_scope_host *copy);

#endif
