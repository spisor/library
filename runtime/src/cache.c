#include "cache.h"

#include <stdlib.h>

#include "cache_damaged.h"
#include "cache_install.h"
#include "cache_installed.h"
#include "cache_locate.h"
#include "cache_marker.h"
#include "cache_sweep.h"
#include "catalog_answer.h"
#include "hosts_lock.h"
#include "platform_key.h"
#include "spisor/json_kind.h"

typedef int32_t (*cache_service)(const cJSON *argument, uint8_t **out, size_t *out_len);

static int32_t serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len, cache_service service) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = service(call.argument, out, out_len);
    spisor_discard_call(&call);
    return status;
}

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

static int32_t answer_or_refuse(cJSON *answer, cJSON *refusal, uint8_t **out, size_t *out_len) {
    if (answer != NULL || refusal == NULL) {
        return runtime_catalog_answer(answer, out, out_len);
    }
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(refusal, "fix");
    int32_t status = spisor_fail(message->valuestring, fix->valuestring, out, out_len);
    cJSON_Delete(refusal);
    return status;
}

static int32_t install(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *cache = nonempty_text(argument, "cache");
    const char *id = nonempty_text(argument, "id");
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "item");
    const char *version = nonempty_text(argument, "version");
    const char *platform = platform_member(argument);
    if (cache == NULL) {
        return runtime_catalog_refuse_argument("cache", "the cache folder's path", out, out_len);
    }
    if (id == NULL) {
        return runtime_catalog_refuse_argument("id", "the catalog item's id", out, out_len);
    }
    if (!cJSON_IsObject(item)) {
        return runtime_catalog_refuse_argument("item", "the catalog item as catalog.read answered it", out, out_len);
    }
    if (version == NULL) {
        return runtime_catalog_refuse_argument("version", "one of the versions the item lists", out, out_len);
    }
    if (platform == NULL) {
        return runtime_catalog_refuse_argument("platform", "a platform key such as linux-x64, or leave it out", out,
                                               out_len);
    }
    cJSON *refusal = NULL;
    cJSON *answer = runtime_cache_install_version(cache, id, item, version, platform, &refusal);
    return answer_or_refuse(answer, refusal, out, out_len);
}

static cJSON *installed_answer(const char *cache, const char *platform) {
    cJSON *answer = cJSON_CreateObject();
    if (!runtime_catalog_attach(answer, "bundles", runtime_cache_installed_bundles(cache)) ||
        !runtime_catalog_attach(answer, "libraries", runtime_cache_installed_libraries(cache, platform))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int32_t installed(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *cache = nonempty_text(argument, "cache");
    const char *platform = platform_member(argument);
    if (cache == NULL) {
        return runtime_catalog_refuse_argument("cache", "the cache folder's path", out, out_len);
    }
    if (platform == NULL) {
        return runtime_catalog_refuse_argument("platform", "a platform key such as linux-x64, or leave it out", out,
                                               out_len);
    }
    return runtime_catalog_answer(installed_answer(cache, platform), out, out_len);
}

static int32_t check(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *directory = nonempty_text(argument, "directory");
    if (directory == NULL) {
        return runtime_catalog_refuse_argument("directory", "an installed version's folder", out, out_len);
    }
    return runtime_catalog_answer(runtime_cache_check_version(directory), out, out_len);
}

static cJSON *damaged_answer(const char *cache, const char *platform) {
    cJSON *damaged = runtime_cache_damaged_versions(cache, platform);
    cJSON *answer = damaged == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL || !runtime_catalog_attach(answer, "damaged", damaged)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int32_t damaged(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *cache = nonempty_text(argument, "cache");
    const char *platform = platform_member(argument);
    if (cache == NULL) {
        return runtime_catalog_refuse_argument("cache", "the cache folder's path", out, out_len);
    }
    if (platform == NULL) {
        return runtime_catalog_refuse_argument("platform", "a platform key such as linux-x64, or leave it out", out,
                                               out_len);
    }
    return runtime_catalog_answer(damaged_answer(cache, platform), out, out_len);
}

static int32_t locate(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *cache = nonempty_text(argument, "cache");
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(argument, "plugins");
    if (cache == NULL) {
        return runtime_catalog_refuse_argument("cache", "the cache folder's path", out, out_len);
    }
    if (!cJSON_IsObject(plugins)) {
        return runtime_catalog_refuse_argument("plugins", "an object of plugin ids to the version each runs", out,
                                               out_len);
    }
    return runtime_catalog_answer(runtime_cache_locate_plugins(cache, plugins), out, out_len);
}

static int32_t platform(const cJSON *argument, uint8_t **out, size_t *out_len) {
    (void)argument;
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddStringToObject(answer, "platform", runtime_platform_key()) == NULL) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return runtime_catalog_answer(answer, out, out_len);
}

static cJSON *swept(const char *cache, const char *state) {
    cJSON *removed = runtime_cache_sweep_versions(cache, state);
    cJSON *answer = removed == NULL ? NULL : cJSON_CreateObject();
    if (removed != NULL && !runtime_catalog_attach(answer, "removed", removed)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int32_t sweep(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *cache = nonempty_text(argument, "cache");
    const char *state = nonempty_text(argument, "state");
    if (cache == NULL) {
        return runtime_catalog_refuse_argument("cache", "the cache folder's path", out, out_len);
    }
    if (state == NULL) {
        return runtime_catalog_refuse_argument("state", "the state folder's path", out, out_len);
    }
    runtime_hosts_guard *guard = runtime_hosts_lock(state);
    if (guard == NULL) {
        return spisor_fail("the registry lock under the state folder could not be taken",
                           "check that SPISOR_STATE names a writable folder, then try again", out, out_len);
    }
    cJSON *answer = swept(cache, state);
    runtime_hosts_unlock(guard);
    return runtime_catalog_answer(answer, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_cache_install(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, install);
}

RUNTIME_EXPORT int32_t runtime_cache_installed(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, installed);
}

RUNTIME_EXPORT int32_t runtime_cache_check(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, check);
}

RUNTIME_EXPORT int32_t runtime_cache_damaged(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, damaged);
}

RUNTIME_EXPORT int32_t runtime_cache_locate(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, locate);
}

RUNTIME_EXPORT int32_t runtime_cache_platform(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, platform);
}

RUNTIME_EXPORT int32_t runtime_cache_sweep(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, sweep);
}
