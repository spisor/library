#include "cache_install.h"
#include "allocation.h"

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
    cJSON *sizes;
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
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(placement->reference, "size");
    return add_name(outcome == RUNTIME_CACHE_CACHED ? record->cached : record->fetched, placement->key) &&
           cJSON_AddStringToObject(record->hashes, placement->key, sha256->valuestring) != NULL &&
           cJSON_AddNumberToObject(record->sizes, placement->key, size->valuedouble) != NULL;
}

/**
 * @implNote Written only after every file is stored and verified, so a refusal midway leaves no runnable
 * version. The version directory is made here only for a version of no files: storing a file made it
 * already, so a directory gone by now was swept away under the install, and remaking it would write a
 * marker naming files that are no longer there.
 */
static int mark_complete(const runtime_cache_plan *plan, const char *id, const char *version,
                         const install_record *record, cJSON **refusal) {
    int has_directory = plan->count > 0 || spisor_make_directories(plan->directory);
    spisor_status status =
        has_directory ? runtime_cache_write_marker(plan->directory, record->hashes, record->sizes) : SPISOR_REFUSED;
    if (status != SPISOR_REFUSED) {
        return status == SPISOR_OK;
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

struct runtime_cache_installation {
    runtime_cache_plan plan;
    const char *id;
    const char *version;
    install_record record;
    cJSON *answer;
};

void runtime_cache_free_installation(runtime_cache_installation *installation) {
    if (installation == NULL) {
        return;
    }
    runtime_cache_free_plan(&installation->plan);
    cJSON_Delete(installation->record.fetched);
    cJSON_Delete(installation->record.cached);
    cJSON_Delete(installation->record.hashes);
    cJSON_Delete(installation->record.sizes);
    cJSON_Delete(installation->answer);
    spisor_memory_release(installation);
}

static int store_planned(runtime_cache_installation *installation, cJSON **refusal) {
    install_record *record = &installation->record;
    *record = (install_record){cJSON_CreateArray(), cJSON_CreateArray(), cJSON_CreateObject(), cJSON_CreateObject()};
    int stored = record->fetched != NULL && record->cached != NULL && record->hashes != NULL && record->sizes != NULL;
    for (size_t index = 0; stored && index < installation->plan.count; index++) {
        stored = store_one(&installation->plan.placements[index], installation->id, installation->version, record,
                           refusal);
    }
    installation->answer =
        stored ? answer_of(&installation->plan, installation->id, installation->version, record) : NULL;
    return installation->answer != NULL;
}

runtime_cache_installation *runtime_cache_store_version(const char *cache, const char *id, const cJSON *item,
                                                        const char *version, const char *platform, cJSON **refusal) {
    runtime_cache_installation *installation = spisor_memory_allocate_zeroed(1, sizeof *installation);
    if (installation == NULL) {
        return NULL;
    }
    installation->id = id;
    installation->version = version;
    if (!runtime_cache_plan_install(cache, id, item, version, platform, &installation->plan, refusal)) {
        spisor_memory_release(installation);
        return NULL;
    }
    if (!store_planned(installation, refusal)) {
        runtime_cache_free_installation(installation);
        return NULL;
    }
    return installation;
}

const cJSON *runtime_cache_installation_answer(const runtime_cache_installation *installation) {
    return installation->answer;
}

int runtime_cache_mark_installation(const runtime_cache_installation *installation, cJSON **refusal) {
    return mark_complete(&installation->plan, installation->id, installation->version, &installation->record, refusal);
}

cJSON *runtime_cache_finish_installation(runtime_cache_installation *installation, cJSON **refusal) {
    cJSON *answer = NULL;
    if (installation != NULL && runtime_cache_mark_installation(installation, refusal)) {
        answer = installation->answer;
        installation->answer = NULL;
    }
    runtime_cache_free_installation(installation);
    return answer;
}

cJSON *runtime_cache_install_version(const char *cache, const char *id, const cJSON *item, const char *version,
                                     const char *platform, cJSON **refusal) {
    return runtime_cache_finish_installation(runtime_cache_store_version(cache, id, item, version, platform, refusal),
                                             refusal);
}
