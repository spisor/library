#ifndef RUNTIME_SCOPE_HOST_H
#define RUNTIME_SCOPE_HOST_H

#include "cJSON.h"
#include "spisor/status.h"

/**
 * What every plugin's scope reads of the host it opens in, as the host's start learns it: the application's `home`,
 * absolute; its `names` declaration, a record, or NULL; and the `portable` place, absolute, or NULL for a host with no
 * portable layer. Each scope keeps its own copy, so a scope outlives the host's end.
 */
typedef struct runtime_scope_host {
    const char *home;
    const cJSON *names;
    const char *portable;
} runtime_scope_host;

/** A copy of a runtime_scope_host its holder owns, freed with runtime_scope_host_free. */
typedef struct runtime_owned_scope_host {
    char *home;
    cJSON *names;
    char *portable;
} runtime_owned_scope_host;

/**
 * Starts the host every scope opened from now on opens in, replacing any started before. Safe from any thread.
 *
 * @return OK; MALFORMED for a home or portable place that is not absolute, or names that are not a record, with
 * nothing replaced; OUT_OF_MEMORY, with nothing replaced.
 */
spisor_status runtime_scopes_begin(const runtime_scope_host *host);

/** Ends the host, so no scope opens until the next begin; the scopes already open keep their copies. */
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
