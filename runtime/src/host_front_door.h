#ifndef RUNTIME_HOST_FRONT_DOOR_H
#define RUNTIME_HOST_FRONT_DOOR_H

#include "cJSON.h"
#include "host_run.h"

/**
 * The environment a SERVE run of `run` hands its loader's application through the ready record. For the layer holding
 * the front door of a loader whose standard declares `application.proxy`, it is the proxy's `baseUrlVariable` set to
 * `http://127.0.0.1:<port>` and its placeholder's `variable` set to its `value`, the port being the one the front door,
 * the standard's first required plugin, declares it listens on: the `port` access of its manifest with mode `listen`,
 * whose target is a port number or `setting:<key>`, a key read from the plugin's merged settings as its scope's config
 * answers them. Any other layer, and a loader whose standard declares no proxy, hands an empty object.
 *
 * @implNote The declaration is read rather than the plugin asked: a front door listens before its activation answers,
 * on the port its declaration names, so once `started` is told it activated the declared port is the bound one. A
 * port the system picks (`any`) cannot be read this way, so it is refused like a missing declaration.
 *
 * @return a new object; NULL with *refusal set to {message, fix} when the front door is not among the run's plugins,
 * declares no fixed port it listens on, or names a setting holding no port; or with it NULL when memory runs out.
 */
cJSON *runtime_front_door_environment(const runtime_host_run *run, cJSON **refusal);

#endif
