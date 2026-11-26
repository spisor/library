#ifndef HOST_START_H
#define HOST_START_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "host_context.h"

/**
 * How to start registered host id in its own layer, as processes.hold takes it: {executable, arguments,
 * environment}, the start its registration records at the version it records, as runtime_entry_start answers it, given
 * the host id and then the count strings of extra. NULL with *refusal set for a host that is not registered, records
 * no start, or whose start runtime_entry_start refuses.
 */
cJSON *host_start_of(const host_context *context, const char *id, const char *const *extra, size_t count,
                     cJSON **refusal);

/**
 * The layers hosts' layer_start answers for loader_id in mode, a runtime_hosts_layer_mode, with plugin, action and
 * input for an ACTION and NULL for any other mode, and result, the path an ACTION's result is written to, NULL for
 * none: an array of {runtime, hostId, executable, arguments, environment}, in
 * the order to start them, the front door's first; NULL with *refusal set when it refuses, or with it NULL when memory
 * runs out.
 */
cJSON *host_layer_starts(const host_context *context, const char *loader_id, uint32_t mode, const char *plugin,
                         const char *action, const char *input, const char *result, cJSON **refusal);

#endif
