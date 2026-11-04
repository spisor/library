#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "boot_placement.h"

#include <stdlib.h>

#include "boot.h"
#include "boot_stub.h"
#include "host_output.h"
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
static char *carried_stub(const host_context *context) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && (cJSON_AddStringToObject(argument, "cache", context->cache) == NULL ||
                             cJSON_AddStringToObject(argument, "hostId", RUNTIME_BOOTED_HOST_ID) == NULL)) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    cJSON *refusal = NULL;
    cJSON *resolved = host_call(runtime_boot_resolve, argument, &refusal);
    cJSON_Delete(refusal);
    const char *segments[3] = {host_text(resolved, "directory"), runtime_platform_key(), STUB_FILE};
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

cJSON *host_place_boot_stub(const host_context *context) {
    char *stub = runtime_boot_stub_path(context->cache);
    char *carried = stub == NULL || spisor_file_exists(stub) ? NULL : carried_stub(context);
    cJSON *refusal = NULL;
    if (carried != NULL && !copy_runnable(carried, stub)) {
        const char *parts[2] = {"forebay-boot could not be put at ", stub};
        refusal = host_refusal_of(parts, 2, PLACEMENT_FIX);
    }
    free(carried);
    free(stub);
    return refusal;
}
