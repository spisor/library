#include "boot.h"
#include "allocation.h"

#include <stdlib.h>

#include "boot_hint.h"
#include "boot_resolve.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "hosts_lock.h"
#include "json_service.h"
#include "platform_key.h"
#include "registry.h"

#define HOST_ID_FIRST_FIX "pass the host id first: spisor-boot <host-id> <arguments>"
#define WRITABLE_CACHE_FIX "check that SPISOR_CACHE names a writable folder, then try again"

/** A boot hint write held between its preparation and its commit: the registry lock, and what it records. */
struct runtime_boot_hint_write {
    spisor_lock *guard;
    const char *cache;
    const char *host_id;
    cJSON *libraries;
    char *host;
    cJSON *answer;
};

static const char *nonempty_text(const cJSON *argument, const char *member) {
    const char *text = runtime_catalog_text_member(argument, member);
    return text != NULL && text[0] != 0 ? text : NULL;
}

/** Sets *refusal to problem with fix, taking problem; *refusal stays NULL for a NULL problem, memory running out. */
static cJSON *refuse_problem(char *problem, const char *fix, cJSON **refusal) {
    *refusal = problem == NULL ? NULL : runtime_refusal(problem, fix);
    spisor_memory_release(problem);
    return NULL;
}

/** Whether cache and host_id are usable, *refusal set as every boot operation refuses them when they are not. */
static int is_checked_host(const char *cache, const char *host_id, cJSON **refusal) {
    if (cache == NULL || cache[0] == 0) {
        runtime_catalog_refuse_member("cache", "the cache folder's path", refusal);
        return 0;
    }
    if (host_id == NULL || host_id[0] == 0) {
        *refusal = runtime_refusal("no host id was given", HOST_ID_FIRST_FIX);
        return 0;
    }
    if (!runtime_is_plain_host_id(host_id)) {
        char *problem = NULL;
        spisor_memory_release(runtime_boot_hint_path(cache, host_id, &problem));
        refuse_problem(problem, RUNTIME_PLAIN_HOST_ID_FIX, refusal);
        return 0;
    }
    return 1;
}

/** The argument's cache and host id, or NULL with *refusal set as every boot operation refuses them. */
static const char *checked_host(const cJSON *argument, const char **cache, cJSON **refusal) {
    const char *host_id = nonempty_text(argument, "hostId");
    *cache = nonempty_text(argument, "cache");
    return is_checked_host(*cache, host_id, refusal) ? host_id : NULL;
}

cJSON *runtime_boot_resolve_host(const char *cache, const char *host_id, cJSON **refusal) {
    *refusal = NULL;
    return is_checked_host(cache, host_id, refusal)
               ? runtime_resolve_booted_host(cache, host_id, runtime_platform_key(), refusal)
               : NULL;
}

static cJSON *hint_answer(cJSON *hint) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || (hint != NULL && !cJSON_AddItemToObject(answer, "hint", hint))) {
        cJSON_Delete(answer);
        cJSON_Delete(hint);
        return NULL;
    }
    return answer;
}

cJSON *runtime_boot_read_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = NULL;
    const char *host_id = checked_host(argument, &cache, refusal);
    int out_of_memory = 0;
    cJSON *hint = host_id == NULL ? NULL : runtime_read_boot_hint(cache, host_id, &out_of_memory);
    return host_id == NULL || out_of_memory ? NULL : hint_answer(hint);
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

static cJSON *refuse_write_argument(const cJSON *argument, cJSON **refusal) {
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(argument, "host");
    if (!is_version_map(cJSON_GetObjectItemCaseSensitive(argument, "libraries"))) {
        return runtime_catalog_refuse_member("libraries", "an object of library id to version", refusal);
    }
    if (host != NULL && (!cJSON_IsString(host) || host->valuestring[0] == 0)) {
        return runtime_catalog_refuse_member("host", "the host bundle version, or leave it out", refusal);
    }
    return runtime_catalog_refuse_member("platform", "a platform key such as linux-x64, or leave it out", refusal);
}

/** The host version the write records: the one given, else the one the hint names now; *out_of_memory as it reads. */
static char *recorded_host(const char *cache, const char *host_id, const cJSON *given, int *out_of_memory) {
    cJSON *hint = given == NULL ? runtime_read_boot_hint(cache, host_id, out_of_memory) : NULL;
    const cJSON *host = given != NULL ? given : cJSON_GetObjectItemCaseSensitive(hint, "host");
    const char *parts[1] = {cJSON_GetStringValue(host)};
    char *copy = parts[0] == NULL ? NULL : runtime_cache_text(parts, 1);
    *out_of_memory = *out_of_memory || (parts[0] != NULL && copy == NULL);
    cJSON_Delete(hint);
    return copy;
}

/** {hint: {libraries, host?}}, the hint the write records, built before it is written; NULL out of memory. */
static cJSON *written_answer(const cJSON *libraries, const char *host) {
    cJSON *hint = cJSON_CreateObject();
    int built = hint != NULL && runtime_catalog_attach(hint, "libraries", cJSON_Duplicate(libraries, 1)) &&
                (host == NULL || cJSON_AddStringToObject(hint, "host", host) != NULL);
    if (!built) {
        cJSON_Delete(hint);
        return NULL;
    }
    return hint_answer(hint);
}

void runtime_boot_free_write(runtime_boot_hint_write *write) {
    if (write != NULL) {
        spisor_unlock(write->guard);
        cJSON_Delete(write->libraries);
        spisor_memory_release(write->host);
        cJSON_Delete(write->answer);
        spisor_memory_release(write);
    }
}

/** Takes the registry lock under state, when given, and reads the host version the write keeps. */
static runtime_boot_hint_write *held_write(const cJSON *argument, runtime_boot_hint_write *write, cJSON **refusal) {
    const char *state = nonempty_text(argument, "state");
    int out_of_memory = 0;
    spisor_status locked = state == NULL ? SPISOR_OK : runtime_hosts_lock(state, &write->guard);
    if (locked == SPISOR_OUT_OF_MEMORY) {
        runtime_boot_free_write(write);
        return NULL;
    }
    if (locked != SPISOR_OK) {
        *refusal = runtime_refusal("the registry lock under the state folder could not be taken",
                                   "check that SPISOR_STATE names a writable folder, then try again");
        runtime_boot_free_write(write);
        return NULL;
    }
    write->host = recorded_host(write->cache, write->host_id, cJSON_GetObjectItemCaseSensitive(argument, "host"),
                                &out_of_memory);
    write->answer = out_of_memory ? NULL : written_answer(write->libraries, write->host);
    if (write->answer == NULL) {
        runtime_boot_free_write(write);
        return NULL;
    }
    return write;
}

runtime_boot_hint_write *runtime_boot_prepare_write(const cJSON *argument, cJSON **refusal) {
    const char *cache = NULL;
    const char *host_id = checked_host(argument, &cache, refusal);
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(argument, "host");
    const char *platform = platform_member(argument);
    if (host_id == NULL) {
        return NULL;
    }
    if (!is_version_map(cJSON_GetObjectItemCaseSensitive(argument, "libraries")) || platform == NULL ||
        (host != NULL && (!cJSON_IsString(host) || host->valuestring[0] == 0))) {
        refuse_write_argument(argument, refusal);
        return NULL;
    }
    char *problem = NULL;
    runtime_boot_hint_write *write = spisor_memory_allocate_zeroed(1, sizeof *write);
    cJSON *libraries = write == NULL ? NULL
                                     : runtime_boot_hint_libraries(
                                           cache, cJSON_GetObjectItemCaseSensitive(argument, "libraries"), platform,
                                           &problem);
    if (libraries == NULL) {
        spisor_memory_release(write);
        refuse_problem(problem, RUNTIME_REINSTALL_FIX, refusal);
        return NULL;
    }
    write->cache = cache;
    write->host_id = host_id;
    write->libraries = libraries;
    return held_write(argument, write, refusal);
}

const cJSON *runtime_boot_write_answer(const runtime_boot_hint_write *write) {
    return write->answer;
}

int runtime_boot_commit_write(runtime_boot_hint_write *write, cJSON **refusal) {
    char *problem = NULL;
    if (runtime_write_boot_hint(write->cache, write->host_id, write->libraries, write->host, &problem)) {
        return 1;
    }
    refuse_problem(problem, WRITABLE_CACHE_FIX, refusal);
    return 0;
}

cJSON *runtime_boot_write_operation(const cJSON *argument, cJSON **refusal) {
    runtime_boot_hint_write *write = runtime_boot_prepare_write(argument, refusal);
    cJSON *answer = NULL;
    if (write != NULL && runtime_boot_commit_write(write, refusal)) {
        answer = write->answer;
        write->answer = NULL;
    }
    runtime_boot_free_write(write);
    return answer;
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

cJSON *runtime_boot_resolve_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = NULL;
    const char *host_id = checked_host(argument, &cache, refusal);
    const char *platform = platform_member(argument);
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(argument, "arguments");
    if (host_id == NULL) {
        return NULL;
    }
    if (platform == NULL) {
        return runtime_catalog_refuse_member("platform", "a platform key such as linux-x64, or leave it out", refusal);
    }
    if (arguments != NULL && !is_string_array(arguments)) {
        return runtime_catalog_refuse_member("arguments", "an array of strings, or leave it out", refusal);
    }
    cJSON *answer = runtime_resolve_booted_host(cache, host_id, platform, refusal);
    cJSON *passed = answer == NULL ? NULL : arguments == NULL ? cJSON_CreateArray() : cJSON_Duplicate(arguments, 1);
    if (answer != NULL && !runtime_catalog_attach(answer, "arguments", passed)) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}
