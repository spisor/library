#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>

#include "boot.h"
#include "boot_resolve.h"
#include "boot_stub.h"
#include "cache_refusal.h"
#include "json_service.h"
#include "paths.h"
#include "platform_key.h"
#include "spisor/file.h"
#include "spisor/path.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#if defined(_WIN32)
#define STUB_FILE "forebay-boot.exe"
#else
#define STUB_FILE "forebay-boot"
#endif

#define PLACEMENT_FIX "check that SPISOR_CACHE names a writable folder, then run install again"

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

/** The forebay-boot the hinted forebay-host build carries, or NULL when there is none. */
static char *carried_stub(const char *cache) {
    cJSON *refusal = NULL;
    cJSON *resolved = runtime_resolve_booted_host(cache, RUNTIME_BOOTED_HOST_ID, runtime_platform_key(), &refusal);
    cJSON_Delete(refusal);
    const char *segments[3] = {cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resolved, "directory")),
                               runtime_platform_key(), STUB_FILE};
    char *carried = segments[0] == NULL ? NULL : joined(segments, 3);
    cJSON_Delete(resolved);
    if (carried != NULL && !spisor_file_exists(carried)) {
        free(carried);
        carried = NULL;
    }
    return carried;
}

static int copy_runnable(const char *source, const char *destination) {
    const char *parent_segments[2] = {destination, ".."};
    char *parent = joined(parent_segments, 2);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(source, &length);
    int copied = parent != NULL && bytes != NULL && spisor_make_directories(parent) == 1 &&
                 spisor_replace_file(destination, bytes, length) == 1;
#if !defined(_WIN32)
    copied = copied && chmod(destination, 0755) == 0;
#endif
    free(bytes);
    free(parent);
    return copied;
}

static cJSON *placement(const char *stub, int placed) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && (cJSON_AddBoolToObject(answer, "placed", placed) == NULL || cJSON_AddStringToObject(answer, "path", stub) == NULL)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *place(const cJSON *argument, cJSON **refusal) {
    const char *cache = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "cache"));
    if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache)) {
        *refusal = runtime_refusal("the argument carries no usable cache", "send {\"cache\": \"<absolute directory>\"}");
        return NULL;
    }
    char *stub = runtime_boot_stub_path(cache);
    char *carried = stub == NULL || spisor_file_exists(stub) ? NULL : carried_stub(cache);
    cJSON *answer = NULL;
    if (carried != NULL && !copy_runnable(carried, stub)) {
        const char *parts[2] = {"forebay-boot could not be put at ", stub};
        *refusal = runtime_cache_refusal(parts, 2, PLACEMENT_FIX);
    } else if (stub != NULL) {
        answer = placement(stub, carried != NULL);
    }
    free(carried);
    free(stub);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_boot_place(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, place);
}
