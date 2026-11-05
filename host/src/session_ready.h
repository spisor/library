#ifndef HOST_SESSION_READY_H
#define HOST_SESSION_READY_H

#include "cJSON.h"
#include "host_context.h"

/**
 * The environment the session host of host_id, running as pid, publishes once it is ready, an object of
 * strings and empty when it has nothing to publish; NULL with *refusal set when it publishes a refusal or
 * is not ready in time. A record an earlier run of the host left, naming another pid, is waited past.
 */
cJSON *host_await_session_environment(const host_context *context, const char *host_id, double pid, cJSON **refusal);

/**
 * A copy of base with every variable of published set over it, the published value winning; NULL when
 * memory runs out.
 *
 * @implNote On Windows a name matches ignoring case, since the environment does, so Path published over
 * PATH replaces it instead of standing beside it.
 */
cJSON *host_overlay_environment(const cJSON *base, const cJSON *published);

#endif
