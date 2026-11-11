#ifndef RUNTIME_PROCESSES_READY_H
#define RUNTIME_PROCESSES_READY_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * processes.ready: {state, hostId, pid, environment?, refused?, fix?} publishes that the session host
 * hostId, running as pid, is ready, by writing state/processes/<hostId>.ready.json whole. environment is
 * an object of strings the application is started with, empty by default; refused and fix publish that
 * the host cannot serve instead, and exclude environment. A host publishes on every start, so a launch
 * can tell a host that has nothing to publish from one that has not finished starting. Answers {}.
 *
 * @implNote Replaced at once, so a reader never sees half a record, and the record names its pid, so
 * a reader ignores one an earlier run left.
 */
RUNTIME_EXPORT int32_t runtime_processes_ready(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * Publishes, as processes.ready does, that the session host host_id running as this process cannot serve,
 * refused and fix being refusal's message and fix. 1 when the record was written.
 */
int runtime_publish_ready_refusal(const char *state, const char *host_id, const cJSON *refusal);

/**
 * processes.awaitReady: {state, hostId, pid, timeoutMilliseconds} polls every 50 milliseconds for the
 * record of hostId naming pid, ignoring one that names another, and answers {environment} from it; a
 * published refusal is refused as it was published, and a record that does not appear within
 * timeoutMilliseconds is refused naming the host.
 */
RUNTIME_EXPORT int32_t runtime_processes_await_ready(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
