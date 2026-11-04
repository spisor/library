#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "boot_resolve.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "updates_request.h"
#include "version_map.h"

static cJSON *no_rollback(const char *item) {
    const char *parts[3] = {"no version of ", item, " is kept for rollback, since no start switched it"};
    const char *fix_parts[3] = {"pin the version to run with plugins.", item, ".pin in the settings"};
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 3, fix);
    free(fix);
    return refusal;
}

static cJSON *not_intact(const char *item, const char *version) {
    const char *parts[4] = {item, "@", version, " is not installed intact, so it cannot be rolled back to"};
    return runtime_cache_refusal(parts, 4, RUNTIME_REINSTALL_FIX);
}

/**
 * Swaps item's run and rollback versions in references, and records the version it moves away from in
 * rolled_back_from, so no check schedules it again while it is still the catalog's latest.
 */
static int swap(runtime_host_references *references, const char *item, const char *version) {
    const char *current = runtime_map_version(references->run, item);
    char *switched_from = NULL;
    if (current != NULL) {
        const char *parts[1] = {current};
        switched_from = runtime_cache_text(parts, 1);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(references->rollback, item);
    if (switched_from != NULL && references->rolled_back_from == NULL) {
        references->rolled_back_from = cJSON_CreateObject();
    }
    int swapped = (current == NULL || switched_from != NULL) && runtime_map_set(references->run, item, version) &&
                  (switched_from == NULL ||
                   (runtime_map_set(references->rollback, item, switched_from) &&
                    references->rolled_back_from != NULL &&
                    runtime_map_set(references->rolled_back_from, item, switched_from)));
    free(switched_from);
    return swapped;
}

static cJSON *rolled(const runtime_host_references *references) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && runtime_catalog_attach(answer, "run", cJSON_Duplicate(references->run, 1)) &&
                runtime_catalog_attach(answer, "next", cJSON_Duplicate(references->next, 1)) &&
                runtime_catalog_attach(answer, "rollback", cJSON_Duplicate(references->rollback, 1));
    if (!built) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *roll_back(const runtime_updates_request *request, const char *item, cJSON **refusal) {
    runtime_host_references recorded = runtime_read_host_references(request->state, request->host_id);
    const char *kept = recorded.rollback == NULL ? NULL : runtime_map_version(recorded.rollback, item);
    char *version = NULL;
    if (kept != NULL) {
        const char *parts[1] = {kept};
        version = runtime_cache_text(parts, 1);
    }
    cJSON *answer = NULL;
    char *problem = NULL;
    if (recorded.run != NULL && kept == NULL) {
        *refusal = no_rollback(item);
    } else if (version != NULL && !runtime_bundle_is_intact(request->cache, item, version)) {
        *refusal = not_intact(item, version);
    } else if (version != NULL && swap(&recorded, item, version)) {
        if (runtime_record_host_references(request->state, request->host_id, &recorded, &problem) &&
            runtime_hint_host_version(request->cache, item, version)) {
            answer = rolled(&recorded);
        } else {
            const char *parts[3] = {"the boot hint of ", item, " could not be written under the cache"};
            *refusal = runtime_updates_unrecorded(problem != NULL ? problem : runtime_cache_text(parts, 3));
        }
    }
    free(version);
    runtime_free_host_references(&recorded);
    return answer;
}

cJSON *runtime_updates_rollback_for(const runtime_updates_request *request, cJSON **refusal) {
    const char *item = runtime_catalog_text_member(request->argument, "item");
    if (item == NULL || item[0] == 0) {
        const char *parts[1] = {"the argument carries no item"};
        *refusal = runtime_cache_refusal(parts, 1, "pass item, the id of the bundle to roll back");
        return NULL;
    }
    runtime_hosts_guard *guard = runtime_updates_lock(request, refusal);
    cJSON *answer = guard == NULL ? NULL : roll_back(request, item, refusal);
    runtime_hosts_unlock(guard);
    return answer;
}
