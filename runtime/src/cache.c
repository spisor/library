#include "cache.h"

#include "cache_damaged.h"
#include "cache_install.h"
#include "cache_installed.h"
#include "cache_locate.h"
#include "cache_marker.h"
#include "cache_sweep.h"
#include "catalog_answer.h"
#include "json_service.h"
#include "hosts_lock.h"
#include "platform_key.h"

static const char *nonempty(const char *text) {
    return text != NULL && text[0] != 0 ? text : NULL;
}

/** The platform a call names, this machine's key for NULL. */
static const char *platform_or_own(const char *platform) {
    return platform == NULL ? runtime_platform_key() : platform;
}

#define CACHE_SHAPE "the cache folder's path"

runtime_cache_installation *runtime_cache_install_storing(const runtime_cache_install_request *request,
                                                          cJSON **refusal) {
    const char *cache = nonempty(request->cache);
    const char *id = nonempty(request->id);
    const cJSON *item = request->item;
    const char *version = nonempty(request->version);
    const char *platform = platform_or_own(request->platform);
    *refusal = NULL;
    if (cache == NULL) {
        runtime_catalog_refuse_member("cache", CACHE_SHAPE, refusal);
        return NULL;
    }
    if (id == NULL) {
        runtime_catalog_refuse_member("id", "the catalog item's id", refusal);
        return NULL;
    }
    if (!cJSON_IsObject(item)) {
        runtime_catalog_refuse_member("item", "the catalog item as catalog.read answered it", refusal);
        return NULL;
    }
    if (version == NULL) {
        runtime_catalog_refuse_member("version", "one of the versions the item lists", refusal);
        return NULL;
    }
    return runtime_cache_store_version(cache, id, item, version, platform, refusal);
}

cJSON *runtime_cache_installed(const char *cache, const char *platform, cJSON **refusal) {
    const char *own_platform = platform_or_own(platform);
    cJSON *answer;
    *refusal = NULL;
    if (nonempty(cache) == NULL) {
        return runtime_catalog_refuse_member("cache", CACHE_SHAPE, refusal);
    }
    answer = cJSON_CreateObject();
    if (!runtime_catalog_attach(answer, "bundles", runtime_cache_installed_bundles(cache)) ||
        !runtime_catalog_attach(answer, "libraries", runtime_cache_installed_libraries(cache, own_platform))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_cache_check(const char *directory, cJSON **refusal) {
    *refusal = NULL;
    if (nonempty(directory) == NULL) {
        return runtime_catalog_refuse_member("directory", "an installed version's folder", refusal);
    }
    return runtime_cache_check_version(directory);
}

/** {name: item}, which takes item, NULL when memory runs out. */
static cJSON *answer_of(const char *name, cJSON *item) {
    cJSON *answer = item == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    if (!runtime_catalog_attach(answer, name, item)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_cache_damaged(const char *cache, const char *platform, cJSON **refusal) {
    *refusal = NULL;
    if (nonempty(cache) == NULL) {
        return runtime_catalog_refuse_member("cache", CACHE_SHAPE, refusal);
    }
    return answer_of("damaged", runtime_cache_damaged_versions(cache, platform_or_own(platform)));
}

cJSON *runtime_cache_locate(const char *cache, const cJSON *plugins, cJSON **refusal) {
    *refusal = NULL;
    if (nonempty(cache) == NULL) {
        return runtime_catalog_refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (!cJSON_IsObject(plugins)) {
        return runtime_catalog_refuse_member("plugins", "an object of plugin ids to the version each runs", refusal);
    }
    return runtime_cache_locate_plugins(cache, plugins);
}

cJSON *runtime_cache_sweep(const char *cache, const char *state, cJSON **refusal) {
    *refusal = NULL;
    if (nonempty(cache) == NULL) {
        return runtime_catalog_refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (nonempty(state) == NULL) {
        return runtime_catalog_refuse_member("state", "the state folder's path", refusal);
    }
    spisor_lock *guard;
    spisor_status locked = runtime_hosts_lock(state, &guard);
    if (locked == SPISOR_OUT_OF_MEMORY) {
        return NULL;
    }
    if (locked != SPISOR_OK) {
        *refusal = runtime_refusal("the registry lock under the state folder could not be taken",
                                   "check that SPISOR_STATE names a writable folder, then try again");
        return NULL;
    }
    cJSON *answer = answer_of("removed", runtime_cache_sweep_versions(cache, state));
    spisor_unlock(guard);
    return answer;
}
