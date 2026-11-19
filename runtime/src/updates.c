#include "updates.h"
#include "allocation.h"

#include <stdlib.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
#include "json_service.h"
#include "host_references.h"
#include "platform_key.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "paths.h"
#include "registry.h"
#include "updates_target.h"

#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"
#define UNREGISTERED_FIX "install the host again to register it, or stop the copy of it that outlived its removal"
#define UNREADABLE_REGISTRATION_FIX "install the host again so its registration is rewritten"

spisor_lock *runtime_updates_lock(const runtime_updates_target *target, cJSON **refusal) {
    spisor_lock *guard;
    if (runtime_hosts_lock(target->state, &guard) == SPISOR_REFUSED) {
        const char *parts[1] = {"the registry lock under the state folder could not be taken"};
        *refusal = runtime_cache_refusal(parts, 1, WRITABLE_STATE_FIX);
    }
    return guard;
}

static cJSON *refuse_unregistered(const runtime_updates_target *target, runtime_registry_answer loaded) {
    if (loaded == RUNTIME_REGISTRY_ABSENT) {
        const char *parts[3] = {"the host ", target->host_id,
                                " is not registered, so nothing is recorded or installed for it"};
        return runtime_cache_refusal(parts, 3, UNREGISTERED_FIX);
    }
    const char *parts[3] = {"the registration of the host ", target->host_id,
                            " could not be read, so nothing is recorded or installed for it"};
    return runtime_cache_refusal(parts, 3, UNREADABLE_REGISTRATION_FIX);
}

spisor_lock *runtime_updates_lock_registered(const runtime_updates_target *target, cJSON **refusal) {
    spisor_lock *guard = runtime_updates_lock(target, refusal);
    cJSON *registration = NULL;
    runtime_registry_answer loaded =
        guard == NULL ? RUNTIME_REGISTRY_OK : runtime_registry_load(target->state, target->host_id, &registration);
    cJSON_Delete(registration);
    if (loaded == RUNTIME_REGISTRY_OUT_OF_MEMORY) {
        spisor_unlock(guard);
        return NULL;
    }
    if (loaded != RUNTIME_REGISTRY_OK) {
        *refusal = refuse_unregistered(target, loaded);
        spisor_unlock(guard);
        return NULL;
    }
    return guard;
}

cJSON *runtime_updates_unrecorded(char *problem) {
    const char *parts[1] = {problem};
    cJSON *refusal = problem == NULL ? NULL : runtime_cache_refusal(parts, 1, WRITABLE_STATE_FIX);
    spisor_memory_release(problem);
    return refusal;
}

int runtime_updates_refuse_platformless(const runtime_updates_target *target, cJSON **refusal) {
    if (target->platform != NULL) {
        return 0;
    }
    *refusal = runtime_refusal("this build of the runtime has no platform key, so no version can be resolved for it",
                               "run a build of the runtime made for this operating system and architecture");
    return 1;
}
