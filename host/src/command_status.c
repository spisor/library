#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "hosts.h"
#include "paths.h"
#include "platform_key.h"
#include "spisor/path.h"

static cJSON *installed_of(const host_context *context, cJSON **refusal) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", context->cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return host_call(runtime_cache_installed, argument, refusal);
}

/** cache.check of one installed version's folder, which hashes every file it holds. */
static cJSON *check_of(const host_context *context, const char *root, const char *id, const char *version, int per_platform) {
    const char *segments[5] = {context->cache, root, id, version, runtime_platform_key()};
    char *directory = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, per_platform ? 5 : 4);
    cJSON *argument = directory == NULL ? NULL : cJSON_CreateObject();
    if (argument != NULL && cJSON_AddStringToObject(argument, "directory", directory) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    free(directory);
    cJSON *refusal = NULL;
    cJSON *check = host_call(runtime_cache_check, argument, &refusal);
    return check != NULL ? check : refusal;
}

/** Adds {id, version, detail, fix, file?} to damaged for each version under root whose files no longer have their hashes. */
static int add_damaged(const host_context *context, const cJSON *versions_of, const char *root, int per_platform, cJSON *damaged) {
    const cJSON *versions;
    cJSON_ArrayForEach(versions, versions_of) {
        const cJSON *version;
        cJSON_ArrayForEach(version, versions) {
            cJSON *check = check_of(context, root, versions->string, version->valuestring, per_platform);
            const char *kind = host_text(check, "kind");
            if (check == NULL || (kind != NULL && strcmp(kind, "intact") == 0)) {
                cJSON_Delete(check);
                if (check == NULL) {
                    return 0;
                }
                continue;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(check, "kind");
            cJSON_AddItemToObject(check, "version", cJSON_CreateString(version->valuestring));
            cJSON_AddItemToObject(check, "id", cJSON_CreateString(versions->string));
            cJSON_AddItemToArray(damaged, check);
        }
    }
    return 1;
}

static cJSON *damaged_of(const host_context *context, const cJSON *installed) {
    cJSON *damaged = cJSON_CreateArray();
    if (damaged == NULL || !add_damaged(context, cJSON_GetObjectItemCaseSensitive(installed, "bundles"), "bundles", 0, damaged) ||
        !add_damaged(context, cJSON_GetObjectItemCaseSensitive(installed, "libraries"), "libraries", 1, damaged)) {
        cJSON_Delete(damaged);
        return NULL;
    }
    return damaged;
}

static int refuse_damaged(const cJSON *damaged) {
    const cJSON *first = cJSON_GetArrayItem(damaged, 0);
    const char *message = host_text(first, "detail");
    const char *fix = host_text(first, "fix");
    const char *parts[1] = {message == NULL ? "an installed version is damaged" : message};
    return host_refuse(host_refusal_of(parts, 1, fix == NULL ? "install it again" : fix));
}

/**
 * {hosts, unreadable, installed, damaged}: what hosts.list answers, what the cache holds for this platform
 * and every installed version whose files no longer carry their hashes, refused when there is one.
 *
 * @implNote A start checks only sizes (runtime_cache_check_version_sizes), so status is where a same-size
 * corruption is found.
 */
static int print_status(const host_context *context) {
    cJSON *refusal = NULL;
    cJSON *status = host_call(runtime_hosts_list, host_places_argument(context), &refusal);
    cJSON *installed = status == NULL ? NULL : installed_of(context, &refusal);
    if (installed == NULL || !cJSON_AddItemToObject(status, "installed", installed)) {
        cJSON_Delete(installed);
        cJSON_Delete(status);
        return host_refuse(refusal);
    }
    cJSON *damaged = damaged_of(context, installed);
    if (damaged == NULL || !cJSON_AddItemToObject(status, "damaged", damaged)) {
        cJSON_Delete(damaged);
        cJSON_Delete(status);
        return host_refuse(NULL);
    }
    host_print_json(status);
    int code = cJSON_GetArraySize(damaged) == 0 ? HOST_EXIT_DONE : refuse_damaged(damaged);
    cJSON_Delete(status);
    return code;
}

int host_command_status(int count, char **arguments) {
    (void)arguments;
    if (count != 0) {
        return host_misuse("status takes no arguments");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? print_status(&context) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
