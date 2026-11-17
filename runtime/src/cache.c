#include "cache.h"

#include <stdio.h>

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

static const char *nonempty_text(const cJSON *argument, const char *member) {
    const char *text = runtime_catalog_text_member(argument, member);
    return text != NULL && text[0] != 0 ? text : NULL;
}

/** The platform member, this machine's key when it is absent, or NULL when it is there but not text. */
static const char *platform_member(const cJSON *argument) {
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(argument, "platform");
    if (platform == NULL) {
        return runtime_platform_key();
    }
    return cJSON_IsString(platform) ? platform->valuestring : NULL;
}

/** The refusal of an argument that carries no usable member, saying what shape it takes; NULL out of memory. */
static cJSON *refuse_member(const char *member, const char *shape, cJSON **refusal) {
    char message[256];
    char fix[512];
    snprintf(message, sizeof message, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as %s", member, shape);
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

#define CACHE_SHAPE "the cache folder's path"
#define PLATFORM_SHAPE "a platform key such as linux-x64, or leave it out"

cJSON *runtime_cache_install_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = nonempty_text(argument, "cache");
    const char *id = nonempty_text(argument, "id");
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "item");
    const char *version = nonempty_text(argument, "version");
    const char *platform = platform_member(argument);
    if (cache == NULL) {
        return refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (id == NULL) {
        return refuse_member("id", "the catalog item's id", refusal);
    }
    if (!cJSON_IsObject(item)) {
        return refuse_member("item", "the catalog item as catalog.read answered it", refusal);
    }
    if (version == NULL) {
        return refuse_member("version", "one of the versions the item lists", refusal);
    }
    if (platform == NULL) {
        return refuse_member("platform", PLATFORM_SHAPE, refusal);
    }
    return runtime_cache_install_version(cache, id, item, version, platform, refusal);
}

cJSON *runtime_cache_installed_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = nonempty_text(argument, "cache");
    const char *platform = platform_member(argument);
    cJSON *answer;
    if (cache == NULL) {
        return refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (platform == NULL) {
        return refuse_member("platform", PLATFORM_SHAPE, refusal);
    }
    answer = cJSON_CreateObject();
    if (!runtime_catalog_attach(answer, "bundles", runtime_cache_installed_bundles(cache)) ||
        !runtime_catalog_attach(answer, "libraries", runtime_cache_installed_libraries(cache, platform))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_cache_check_operation(const cJSON *argument, cJSON **refusal) {
    const char *directory = nonempty_text(argument, "directory");
    if (directory == NULL) {
        return refuse_member("directory", "an installed version's folder", refusal);
    }
    return runtime_cache_check_version(directory);
}

/** {name: item}, which takes item, NULL when memory runs out. */
static cJSON *answer_of(const char *name, cJSON *item) {
    cJSON *answer = item == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL || !runtime_catalog_attach(answer, name, item)) {
        cJSON_Delete(answer);
        cJSON_Delete(item);
        return NULL;
    }
    return answer;
}

cJSON *runtime_cache_damaged_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = nonempty_text(argument, "cache");
    const char *platform = platform_member(argument);
    if (cache == NULL) {
        return refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (platform == NULL) {
        return refuse_member("platform", PLATFORM_SHAPE, refusal);
    }
    return answer_of("damaged", runtime_cache_damaged_versions(cache, platform));
}

cJSON *runtime_cache_locate_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = nonempty_text(argument, "cache");
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(argument, "plugins");
    if (cache == NULL) {
        return refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (!cJSON_IsObject(plugins)) {
        return refuse_member("plugins", "an object of plugin ids to the version each runs", refusal);
    }
    return runtime_cache_locate_plugins(cache, plugins);
}

cJSON *runtime_cache_sweep_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = nonempty_text(argument, "cache");
    const char *state = nonempty_text(argument, "state");
    if (cache == NULL) {
        return refuse_member("cache", CACHE_SHAPE, refusal);
    }
    if (state == NULL) {
        return refuse_member("state", "the state folder's path", refusal);
    }
    spisor_lock *guard = runtime_hosts_lock(state);
    if (guard == NULL) {
        *refusal = runtime_refusal("the registry lock under the state folder could not be taken",
                                   "check that SPISOR_STATE names a writable folder, then try again");
        return NULL;
    }
    cJSON *answer = answer_of("removed", runtime_cache_sweep_versions(cache, state));
    spisor_unlock(guard);
    return answer;
}
