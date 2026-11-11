#include "boot.h"

#include <stdlib.h>

#include "boot_hint.h"
#include "boot_resolve.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "hosts_lock.h"
#include "platform_key.h"
#include "spisor/json_kind.h"

#define HOST_ID_FIRST_FIX "pass the host id first: spisor-boot <host-id> <arguments>"

typedef int32_t (*boot_service)(const cJSON *argument, const char *cache, const char *host_id, uint8_t **out,
                                size_t *out_len);

static const char *nonempty_text(const cJSON *argument, const char *member) {
    const char *text = runtime_catalog_text_member(argument, member);
    return text != NULL && text[0] != 0 ? text : NULL;
}

static int32_t refuse_problem(char *problem, const char *fix, uint8_t **out, size_t *out_len) {
    if (problem == NULL) {
        return runtime_catalog_answer(NULL, out, out_len);
    }
    int32_t status = spisor_fail(problem, fix, out, out_len);
    free(problem);
    return status;
}

static int32_t refuse_answer(cJSON *refusal, uint8_t **out, size_t *out_len) {
    if (refusal == NULL) {
        return runtime_catalog_answer(NULL, out, out_len);
    }
    int32_t status = spisor_fail(cJSON_GetObjectItemCaseSensitive(refusal, "message")->valuestring,
                                 cJSON_GetObjectItemCaseSensitive(refusal, "fix")->valuestring, out, out_len);
    cJSON_Delete(refusal);
    return status;
}

static int32_t serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len, boot_service service) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const char *cache = nonempty_text(call.argument, "cache");
    const char *host_id = nonempty_text(call.argument, "hostId");
    if (cache == NULL) {
        status = runtime_catalog_refuse_argument("cache", "the cache folder's path", out, out_len);
    } else if (host_id == NULL) {
        status = spisor_fail("no host id was given", HOST_ID_FIRST_FIX, out, out_len);
    } else if (!runtime_is_plain_host_id(host_id)) {
        char *problem = NULL;
        free(runtime_boot_hint_path(cache, host_id, &problem));
        status = refuse_problem(problem, RUNTIME_PLAIN_HOST_ID_FIX, out, out_len);
    } else {
        status = service(call.argument, cache, host_id, out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}

static int32_t read_hint(const cJSON *argument, const char *cache, const char *host_id, uint8_t **out,
                         size_t *out_len) {
    (void)argument;
    cJSON *answer = cJSON_CreateObject();
    cJSON *hint = runtime_read_boot_hint(cache, host_id);
    if (answer != NULL && hint != NULL && !cJSON_AddItemToObject(answer, "hint", hint)) {
        cJSON_Delete(answer);
        answer = NULL;
    } else if (answer == NULL) {
        cJSON_Delete(hint);
    }
    return runtime_catalog_answer(answer, out, out_len);
}

static const char *platform_member(const cJSON *argument) {
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(argument, "platform");
    if (platform == NULL) {
        return runtime_platform_key();
    }
    return cJSON_IsString(platform) && runtime_is_platform_key(platform->valuestring) ? platform->valuestring : NULL;
}

static int is_version_map(const cJSON *value) {
    if (!cJSON_IsObject(value)) {
        return 0;
    }
    for (const cJSON *entry = value->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsString(entry) || entry->valuestring[0] == 0) {
            return 0;
        }
    }
    return 1;
}

static int32_t write_resolved_hint(const cJSON *argument, const char *cache, const char *host_id,
                                   const cJSON *libraries, uint8_t **out, size_t *out_len) {
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(argument, "host");
    const char *state = nonempty_text(argument, "state");
    spisor_lock *guard = state == NULL ? NULL : runtime_hosts_lock(state);
    if (state != NULL && guard == NULL) {
        return spisor_fail("the registry lock under the state folder could not be taken",
                           "check that SPISOR_STATE names a writable folder, then try again", out, out_len);
    }
    char *problem = NULL;
    int written = runtime_write_boot_hint(cache, host_id, libraries, host == NULL ? NULL : host->valuestring, &problem);
    int32_t status = written ? read_hint(argument, cache, host_id, out, out_len)
                             : refuse_problem(problem, "check that SPISOR_CACHE names a writable folder, then try again",
                                              out, out_len);
    spisor_unlock(guard);
    return status;
}

static int32_t write_hint(const cJSON *argument, const char *cache, const char *host_id, uint8_t **out,
                          size_t *out_len) {
    const cJSON *versions = cJSON_GetObjectItemCaseSensitive(argument, "libraries");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(argument, "host");
    const char *platform = platform_member(argument);
    if (!is_version_map(versions)) {
        return runtime_catalog_refuse_argument("libraries", "an object of library id to version", out, out_len);
    }
    if (host != NULL && (!cJSON_IsString(host) || host->valuestring[0] == 0)) {
        return runtime_catalog_refuse_argument("host", "the host bundle version, or leave it out", out, out_len);
    }
    if (platform == NULL) {
        return runtime_catalog_refuse_argument("platform", "a platform key such as linux-x64, or leave it out", out,
                                               out_len);
    }
    char *problem = NULL;
    cJSON *libraries = runtime_boot_hint_libraries(cache, versions, platform, &problem);
    if (libraries == NULL) {
        return refuse_problem(problem, RUNTIME_REINSTALL_FIX, out, out_len);
    }
    int32_t status = write_resolved_hint(argument, cache, host_id, libraries, out, out_len);
    cJSON_Delete(libraries);
    return status;
}

static int is_string_array(const cJSON *value) {
    if (!cJSON_IsArray(value)) {
        return 0;
    }
    for (const cJSON *entry = value->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsString(entry)) {
            return 0;
        }
    }
    return 1;
}

static int32_t resolve(const cJSON *argument, const char *cache, const char *host_id, uint8_t **out,
                       size_t *out_len) {
    const char *platform = platform_member(argument);
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(argument, "arguments");
    if (platform == NULL) {
        return runtime_catalog_refuse_argument("platform", "a platform key such as linux-x64, or leave it out", out,
                                               out_len);
    }
    if (arguments != NULL && !is_string_array(arguments)) {
        return runtime_catalog_refuse_argument("arguments", "an array of strings, or leave it out", out, out_len);
    }
    cJSON *refusal = NULL;
    cJSON *answer = runtime_resolve_booted_host(cache, host_id, platform, &refusal);
    if (answer == NULL) {
        return refuse_answer(refusal, out, out_len);
    }
    cJSON *passed = arguments == NULL ? cJSON_CreateArray() : cJSON_Duplicate(arguments, 1);
    if (!runtime_catalog_attach(answer, "arguments", passed)) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return runtime_catalog_answer(answer, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_boot_read(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, read_hint);
}

RUNTIME_EXPORT int32_t runtime_boot_write(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, write_hint);
}

RUNTIME_EXPORT int32_t runtime_boot_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, resolve);
}
