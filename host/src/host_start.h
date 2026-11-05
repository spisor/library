#ifndef HOST_START_H
#define HOST_START_H

#include "cJSON.h"
#include "host_context.h"

/** The catalog id of the generic host that runs a loader's own plugins on JavaScript. */
#define HOST_JAVASCRIPT_HOST_ID "javascript-host"

/**
 * How to start registered host id in its own layer, as processes.hold takes it: {executable, arguments,
 * environment}, from the start its registration records at the version it records. A native host's entry
 * is its executable; a JavaScript one's entry is run by the JS runtime <cache>/runtimes/javascript.json
 * names. Either is given the host id as its last argument. NULL with *refusal set for a host that is not
 * registered, records no start, or whose version is not installed intact.
 */
cJSON *host_start_of(const host_context *context, const char *id, cJSON **refusal);

/** Appends the count strings of extra to the arguments of start, a start host_start_of answered; 0 when memory runs out. */
int host_start_append_arguments(cJSON *start, const char *const *extra, size_t count);

/**
 * How to start loader_id's own plugins in a session host on javascript-host, the version its boot hint
 * names, with the loader id as its last argument; NULL with *refusal set when it is not installed.
 */
cJSON *host_session_start_of(const host_context *context, const char *loader_id, cJSON **refusal);

#endif
