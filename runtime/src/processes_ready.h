#ifndef RUNTIME_PROCESSES_READY_H
#define RUNTIME_PROCESSES_READY_H

#include "cJSON.h"

/**
 * What a session host publishes: that host_id, running as pid, is ready with environment, an object of strings the
 * application is started with (NULL for none), or, with refused and fix, that it cannot serve, environment then NULL.
 */
typedef struct runtime_processes_publication {
    const char *state;
    const char *host_id;
    double pid;
    const cJSON *environment;
    const char *refused;
    const char *fix;
} runtime_processes_publication;

/**
 * processes' ready: publishes the publication by writing state/processes/<host_id>.ready.json whole. A host publishes
 * on every start, so a launch can tell a host that has nothing to publish from one that has not finished starting.
 * 1 when written; 0 with *refusal set when it could not be, or NULL when memory ran out.
 *
 * @implNote Replaced at once, so a reader never sees half a record, and the record names its pid, so
 * a reader ignores one an earlier run left.
 */
int runtime_processes_ready(const runtime_processes_publication *publication, cJSON **refusal);

/**
 * Publishes, as processes.ready does, that the session host host_id running as this process cannot serve,
 * refused and fix being refusal's message and fix. 1 when the record was written.
 */
int runtime_publish_ready_refusal(const char *state, const char *host_id, const cJSON *refusal);

/**
 * processes' awaitReady: polls every 50 milliseconds for the record of host_id naming pid, ignoring one that names
 * another, and answers {environment} from it; a published refusal is refused as it was published, and a record that
 * does not appear within timeout milliseconds is refused naming the host.
 */
cJSON *runtime_processes_await_ready(const char *state, const char *host_id, double pid, double timeout,
                                     cJSON **refusal);

#endif
