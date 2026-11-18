#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>

#include "boot.h"
#include "allocation.h"
#include "boot_resolve.h"
#include "boot_stub.h"
#include "cache_refusal.h"
#include "install_run.h"
#include "json_service.h"
#include "paths.h"
#include "platform_key.h"
#include "file_reading.h"
#include "spisor/file.h"
#include "spisor/path.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define PLACEMENT_FIX "check that SPISOR_CACHE names a writable folder, then run install again"

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

/**
 * The spisor-boot the hinted spisor-host build carries, or NULL when there is none, and NULL with *out_of_memory set
 * to 1 when memory runs out.
 */
static char *carried_stub(const char *cache, int *out_of_memory) {
    cJSON *refusal = NULL;
    cJSON *resolved = runtime_resolve_booted_host(cache, RUNTIME_ECOSYSTEM_HOST_ID, runtime_platform_key(), &refusal);
    const char *segments[3] = {cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resolved, "directory")),
                               runtime_platform_key(), RUNTIME_BOOT_STUB_NAME};
    char *carried = segments[0] == NULL ? NULL : joined(segments, 3);
    *out_of_memory = (resolved == NULL && refusal == NULL) || (segments[0] != NULL && carried == NULL);
    cJSON_Delete(refusal);
    cJSON_Delete(resolved);
    spisor_file_presence presence = carried == NULL ? SPISOR_FILE_MISSING : spisor_probe_file(carried);
    *out_of_memory = *out_of_memory || presence == SPISOR_FILE_OUT_OF_MEMORY;
    if (carried != NULL && presence != SPISOR_FILE_PRESENT) {
        spisor_memory_release(carried);
        carried = NULL;
    }
    return carried;
}

/** Copies source to destination, runnable: SPISOR_REFUSED when it cannot, SPISOR_OUT_OF_MEMORY when memory ran out. */
static spisor_status copy_runnable(const char *source, const char *destination) {
    const char *parent_segments[2] = {destination, ".."};
    char *parent = joined(parent_segments, 2);
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_OUT_OF_MEMORY;
    uint8_t *bytes = parent == NULL ? NULL : spisor_read_file_reporting(source, &length, &outcome);
    spisor_status copied = outcome == SPISOR_READ_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY
                           : bytes == NULL                     ? SPISOR_REFUSED
                                                               : spisor_make_directories(parent);
    if (copied == SPISOR_OK) {
        copied = spisor_replace_file(destination, bytes, length);
    }
#if !defined(_WIN32)
    copied = copied == SPISOR_OK && chmod(destination, 0755) != 0 ? SPISOR_REFUSED : copied;
#endif
    spisor_memory_release(bytes);
    spisor_memory_release(parent);
    return copied;
}

static cJSON *placement(const char *stub, int placed) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && (cJSON_AddBoolToObject(answer, "placed", placed) == NULL ||
                           cJSON_AddStringToObject(answer, "path", stub) == NULL)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_boot_place_operation(const cJSON *argument, cJSON **refusal) {
    const char *cache = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "cache"));
    if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache)) {
        *refusal = runtime_refusal("the argument carries no usable cache", "send {\"cache\": \"<absolute directory>\"}");
        return NULL;
    }
    int out_of_memory = 0;
    char *stub = runtime_boot_stub_path(cache);
    char *carried = stub == NULL || spisor_probe_file(stub) == SPISOR_FILE_PRESENT
                        ? NULL
                        : carried_stub(cache, &out_of_memory);
    cJSON *answer = stub == NULL || out_of_memory ? NULL : placement(stub, carried != NULL);
    spisor_status copied = answer != NULL && carried != NULL ? copy_runnable(carried, stub) : SPISOR_OK;
    if (copied != SPISOR_OK) {
        const char *parts[2] = {"spisor-boot could not be put at ", stub};
        cJSON_Delete(answer);
        answer = NULL;
        *refusal = copied == SPISOR_OUT_OF_MEMORY ? NULL : runtime_cache_refusal(parts, 2, PLACEMENT_FIX);
    }
    spisor_memory_release(carried);
    spisor_memory_release(stub);
    return answer;
}
