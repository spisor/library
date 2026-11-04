#include "cache_install.h"

#include <stdlib.h>

#include "cache_marker.h"
#include "cache_plan.h"
#include "cache_refusal.h"
#include "cache_store.h"
#include "spisor/file.h"

#define UNWRITABLE_FIX "free the cached file or point SPISOR_CACHE at a writable directory, then try again"

typedef struct install_record {
    cJSON *fetched;
    cJSON *cached;
    cJSON *hashes;
} install_record;

static int add_name(cJSON *names, const char *key) {
    cJSON *name = cJSON_CreateString(key);
    return name != NULL && cJSON_AddItemToArray(names, name);
}

/** The store's refusal with its message prefixed by the file and the item, so a caller can say which failed. */
static cJSON *file_refusal(const char *key, const char *id, const char *version, cJSON *stored) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(stored, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(stored, "fix");
    const char *parts[8] = {"file ", key, " of ", id, "@", version, ": ",
                            cJSON_IsString(message) ? message->valuestring : "it could not be stored"};
    cJSON *refusal = runtime_cache_refusal(parts, 8, cJSON_IsString(fix) ? fix->valuestring : UNWRITABLE_FIX);
    cJSON_Delete(stored);
    return refusal;
}

static int store_one(const runtime_cache_placement *placement, const char *id, const char *version,
                     install_record *record, cJSON **refusal) {
    cJSON *stored = NULL;
    runtime_cache_stored outcome = runtime_cache_store_file(placement->reference, placement->destination, &stored);
    if (outcome == RUNTIME_CACHE_REFUSED) {
        *refusal = file_refusal(placement->key, id, version, stored);
        return 0;
    }
    if (outcome == RUNTIME_CACHE_OUT_OF_MEMORY) {
        return 0;
    }
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(placement->reference, "sha256");
    return add_name(outcome == RUNTIME_CACHE_CACHED ? record->cached : record->fetched, placement->key) &&
           cJSON_AddStringToObject(record->hashes, placement->key, sha256->valuestring) != NULL;
}

/** @implNote Written only after every file is stored and verified, so a refusal midway leaves no runnable version. */
static int mark_complete(const runtime_cache_plan *plan, const char *id, const char *version,
                         const install_record *record, cJSON **refusal) {
    if (spisor_make_directories(plan->directory) && runtime_cache_write_marker(plan->directory, record->hashes)) {
        return 1;
    }
    const char *parts[7] = {"file " RUNTIME_INSTALL_MARKER " of ", id, "@", version, ": the completion marker in ",
                            plan->directory, " could not be written"};
    *refusal = runtime_cache_refusal(parts, 7, UNWRITABLE_FIX);
    return 0;
}

static cJSON *answer_of(const runtime_cache_plan *plan, const char *id, const char *version, install_record *record) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && cJSON_AddStringToObject(answer, "id", id) != NULL &&
                cJSON_AddStringToObject(answer, "version", version) != NULL &&
                cJSON_AddStringToObject(answer, "directory", plan->directory) != NULL &&
                cJSON_AddItemToObject(answer, "fetched", record->fetched);
    record->fetched = built ? NULL : record->fetched;
    built = built && cJSON_AddItemToObject(answer, "cached", record->cached);
    record->cached = built ? NULL : record->cached;
    if (!built) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *install_planned(const runtime_cache_plan *plan, const char *id, const char *version, cJSON **refusal) {
    install_record record = {cJSON_CreateArray(), cJSON_CreateArray(), cJSON_CreateObject()};
    int stored = record.fetched != NULL && record.cached != NULL && record.hashes != NULL;
    for (size_t index = 0; stored && index < plan->count; index++) {
        stored = store_one(&plan->placements[index], id, version, &record, refusal);
    }
    cJSON *answer = stored && mark_complete(plan, id, version, &record, refusal)
                        ? answer_of(plan, id, version, &record)
                        : NULL;
    cJSON_Delete(record.fetched);
    cJSON_Delete(record.cached);
    cJSON_Delete(record.hashes);
    return answer;
}

cJSON *runtime_cache_install_version(const char *cache, const char *id, const cJSON *item, const char *version,
                                     const char *platform, cJSON **refusal) {
    runtime_cache_plan plan;
    if (!runtime_cache_plan_install(cache, id, item, version, platform, &plan, refusal)) {
        return NULL;
    }
    cJSON *answer = install_planned(&plan, id, version, refusal);
    runtime_cache_free_plan(&plan);
    return answer;
}
