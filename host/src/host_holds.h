#ifndef HOST_HOLDS_H
#define HOST_HOLDS_H

#include "cJSON.h"
#include "host_context.h"

/**
 * Makes holder a holder of each of starts, an object {host id: start}, starting any that is not running;
 * 1, or 0 with *refusal set after releasing the ones already held, so a refusal leaves nothing held.
 */
int host_hold_all(const host_context *context, const cJSON *starts, double holder, cJSON **refusal);

/** Drops holder from each of starts' hosts; never signals one, which stops itself once it sees no holder. */
void host_release_all(const host_context *context, const cJSON *starts, double holder);

#endif
