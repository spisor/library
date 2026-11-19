#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "allocation.h"
#include "host_child.h"
#include "host_service.h"
#include "host_output.h"
#include "spisor/json_allocation.h"
#include "own_executable.h"
#include "paths.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include "host_arguments.h"
#endif

#define NO_HOST_FIX "pass the host id first: spisor-boot <host-id> <arguments>"

/**
 * The cache this stub boots from: it lives at <cache>/boot/spisor-boot, so the cache is two folders up.
 *
 * @implNote Read from where the stub lies rather than from the environment, so a command shim run under
 * another SPISOR_CACHE still boots the version its own install recorded.
 */
static char *own_cache(void) {
    char *executable = host_own_executable();
    const char *segments[3] = {executable, "..", ".."};
    char *cache = executable == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
    spisor_memory_release(executable);
    return cache;
}

/** The arguments to pass through as an array of strings; NULL when memory runs out. */
static cJSON *passed_arguments(int count, char **arguments) {
    cJSON *passed = cJSON_CreateArray();
    for (int index = 0; passed != NULL && index < count; index++) {
        cJSON *argument = cJSON_CreateString(arguments[index]);
        if (argument == NULL || !cJSON_AddItemToArray(passed, argument)) {
            cJSON_Delete(argument);
            cJSON_Delete(passed);
            passed = NULL;
        }
    }
    return passed;
}

/** Runs the version of host argument 0 its boot hint names, with the rest of the arguments passed through. */
static int boot(int count, char **arguments) {
    if (count < 1 || arguments[0][0] == 0) {
        const char *parts[1] = {"no host id was given"};
        return host_refuse(host_refusal_of(parts, 1, NO_HOST_FIX));
    }
    char *cache = own_cache();
    cJSON *refusal = NULL;
    cJSON *resolved = cache == NULL ? NULL : runtime_boot_resolve_host(cache, arguments[0], &refusal);
    cJSON *passed = resolved == NULL ? NULL : passed_arguments(count - 1, arguments + 1);
    spisor_memory_release(cache);
    if (passed != NULL && !host_make_runnable(host_text(resolved, "executable"))) {
        const char *parts[2] = {host_text(resolved, "executable"), " cannot be made runnable"};
        refusal = host_refusal_of(parts, 2,
                                  "check that the cache SPISOR_CACHE names is writable, then run the command again");
    } else if (passed != NULL) {
        refusal = host_replace_process(host_text(resolved, "executable"), passed, NULL);
    }
    cJSON_Delete(passed);
    cJSON_Delete(resolved);
    return host_refuse(refusal);
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    spisor_json_use_library_allocator();
    char **arguments = host_utf8_arguments(count, wide);
    int code = arguments == NULL ? HOST_EXIT_MISUSED : boot(count - 1, arguments + 1);
    host_free_arguments(arguments, count);
    return code;
}

#else

int main(int count, char **arguments) {
    spisor_json_use_library_allocator();
    return boot(count - 1, arguments + 1);
}

#endif
