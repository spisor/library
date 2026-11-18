#ifndef RUNTIME_UPDATES_TARGET_H
#define RUNTIME_UPDATES_TARGET_H

#include "cJSON.h"
#include "hosts_lock.h"

/** Fills a consumer's answer from an answer about to be committed; 0 when memory runs out, which stops the commit. */
typedef int (*runtime_updates_precommit)(const cJSON *answer, void *context);

/** The host an updates call acts for: the places it acts in, its plain id and the platform key, NULL for none. */
typedef struct runtime_updates_target {
    const char *state;
    const char *cache;
    const char *host_id;
    const char *platform;
} runtime_updates_target;

/** 1 with *refusal set when target has no platform key, as on a build of the runtime made for no platform; else 0. */
int runtime_updates_refuse_platformless(const runtime_updates_target *target, cJSON **refusal);

/** The registry lock under target's state, or NULL with *refusal saying it could not be taken. */
spisor_lock *runtime_updates_lock(const runtime_updates_target *target, cJSON **refusal);

/**
 * The registry lock, taken only while target's host is registered: NULL with *refusal set when it is
 * not, or when its registration cannot be read, and with *refusal NULL when memory runs out.
 *
 * @implNote Every update write takes this, so a host still running after hosts.remove deleted its state
 * under the same lock cannot record references, run records or hints the sweep would keep for ever.
 */
spisor_lock *runtime_updates_lock_registered(const runtime_updates_target *target, cJSON **refusal);

/** The refusal of a references file that could not be written, taking problem, which it frees. */
cJSON *runtime_updates_unrecorded(char *problem);

#endif
