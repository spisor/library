#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "host_child.h"
#include "host_context.h"
#include "host_output.h"
#include "paths.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>

#include "host_arguments.h"
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#define NO_HOST_FIX "pass the host id first: forebay-boot <host-id> <arguments>"

#if defined(_WIN32)

static char *own_executable(void) {
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(NULL, path, 32768);
    int bytes = length == 0 || length >= 32768 ? 0 : WideCharToMultiByte(CP_UTF8, 0, path, (int)length, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : malloc((size_t)bytes + 1);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, path, (int)length, narrow, bytes, NULL, NULL);
        narrow[bytes] = 0;
    }
    return narrow;
}

#elif defined(__APPLE__)

static char *own_executable(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *path = malloc(size + 1);
    if (path != NULL && _NSGetExecutablePath(path, &size) != 0) {
        free(path);
        path = NULL;
    }
    return path;
}

#else

static char *own_executable(void) {
    size_t capacity = 4096;
    char *path = malloc(capacity);
    ssize_t length = path == NULL ? -1 : readlink("/proc/self/exe", path, capacity - 1);
    if (length < 0) {
        free(path);
        return NULL;
    }
    path[length] = 0;
    return path;
}

#endif

/**
 * The cache this stub boots from: it lives at <cache>/boot/forebay-boot, so the cache is two folders up.
 *
 * @implNote Read from where the stub lies rather than from the environment, so a command shim run under
 * another SPISOR_CACHE still boots the version its own install recorded.
 */
static char *own_cache(void) {
    char *executable = own_executable();
    const char *segments[3] = {executable, "..", ".."};
    char *cache = executable == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
    free(executable);
    return cache;
}

static cJSON *resolve_call(const char *cache, const char *host_id, int count, char **arguments) {
    cJSON *call = cJSON_CreateObject();
    cJSON *passed = call == NULL ? NULL : cJSON_AddArrayToObject(call, "arguments");
    int built = passed != NULL && cJSON_AddStringToObject(call, "cache", cache) != NULL &&
                cJSON_AddStringToObject(call, "hostId", host_id) != NULL;
    for (int index = 0; built && index < count; index++) {
        built = cJSON_AddItemToArray(passed, cJSON_CreateString(arguments[index]));
    }
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Runs the version of host argument 0 its boot hint names, with the rest of the arguments passed through. */
static int boot(int count, char **arguments) {
    if (count < 1 || arguments[0][0] == 0) {
        const char *parts[1] = {"no host id was given"};
        return host_refuse(host_refusal_of(parts, 1, NO_HOST_FIX));
    }
    char *cache = own_cache();
    cJSON *refusal = NULL;
    cJSON *resolved = cache == NULL ? NULL
                                    : host_call(runtime_boot_resolve, resolve_call(cache, arguments[0], count - 1, arguments + 1),
                                                &refusal);
    free(cache);
    if (resolved != NULL && !host_make_runnable(host_text(resolved, "executable"))) {
        const char *parts[2] = {host_text(resolved, "executable"), " cannot be made runnable"};
        refusal = host_refusal_of(parts, 2, "check that the cache SPISOR_CACHE names is writable, then run the command again");
    } else if (resolved != NULL) {
        refusal = host_replace_process(host_text(resolved, "executable"), cJSON_GetObjectItemCaseSensitive(resolved, "arguments"));
    }
    cJSON_Delete(resolved);
    return host_refuse(refusal);
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = host_utf8_arguments(count, wide);
    int code = arguments == NULL ? HOST_EXIT_MISUSED : boot(count - 1, arguments + 1);
    host_free_arguments(arguments, count);
    return code;
}

#else

int main(int count, char **arguments) {
    return boot(count - 1, arguments + 1);
}

#endif
