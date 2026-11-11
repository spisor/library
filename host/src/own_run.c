#include "own_run.h"

#include <stdlib.h>
#include <string.h>

#include "boot_stub.h"
#include "host_output.h"
#include "install_run.h"
#include "cache_place.h"
#include "own_executable.h"
#include "paths.h"
#include "processes.h"

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#endif

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

#if defined(_WIN32)

/**
 * The path the file system finally names for path, 8.3 names and links resolved, without the \\?\ prefix;
 * freed with free, NULL when it cannot be opened.
 */
static char *final_path(const char *path) {
    wchar_t *wide = spisor_wide_path(path);
    HANDLE handle = wide == NULL ? INVALID_HANDLE_VALUE
                                 : CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    wchar_t final[32768];
    DWORD length = GetFinalPathNameByHandleW(handle, final, 32768, VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (length == 0 || length >= 32768) {
        return NULL;
    }
    const wchar_t *start = final;
    if (wcsncmp(final, L"\\\\?\\UNC\\", 8) == 0) {
        final[6] = L'\\';
        start = final + 6;
    } else if (wcsncmp(final, L"\\\\?\\", 4) == 0) {
        start = final + 4;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, start, -1, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : malloc((size_t)bytes);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, start, -1, narrow, bytes, NULL, NULL);
    }
    return narrow;
}

#else

static char *final_path(const char *path) {
    return runtime_cache_resolve(path, NULL, 0);
}

#endif

static void report(const char *executable, const char *problem) {
    host_write(1, "spisor cannot keep the spisor-host version it runs from a sweep: ");
    host_write(1, executable);
    host_write(1, problem);
    host_write(1, "\nfix: run spisor-host from <cache>/bundles/spisor-host/<version>/, as spisor-boot does\n");
}

/** The version segment of the executable below cache/bundles/spisor-host, freed with free; NULL outside it. */
static char *own_version(const host_context *context) {
    char *executable = host_own_executable();
    const char *segments[2] = {"bundles", RUNTIME_ECOSYSTEM_HOST_ID};
    char *joined = runtime_cache_resolve(context->cache, segments, 2);
    char *folder = joined == NULL ? NULL : final_path(joined);
    char *resolved = executable == NULL ? NULL : final_path(executable);
    char *version = NULL;
    if (folder != NULL && resolved != NULL && runtime_cache_is_strictly_inside(folder, resolved)) {
        const char *start = resolved + strlen(folder) + 1;
        size_t length = 0;
        while (start[length] != 0 && !is_separator(start[length])) {
            length++;
        }
        version = start[length] == 0 ? NULL : malloc(length + 1);
        if (version != NULL) {
            memcpy(version, start, length);
            version[length] = 0;
        } else {
            report(executable, " lies in the bundles folder but in no version of it");
        }
    }
    free(resolved);
    free(folder);
    free(joined);
    free(executable);
    return version;
}

static cJSON *run_call(const own_run *run, const char *version) {
    cJSON *call = cJSON_CreateObject();
    int built = call != NULL && cJSON_AddStringToObject(call, "state", run->context->state) != NULL &&
                cJSON_AddStringToObject(call, "hostId", RUNTIME_ECOSYSTEM_HOST_ID) != NULL &&
                cJSON_AddNumberToObject(call, "checkMilliseconds", HOST_OWN_RUN_INTERVAL_MILLISECONDS) != NULL;
    cJSON *ran = built ? cJSON_AddObjectToObject(call, "run") : NULL;
    built = ran != NULL && (version == NULL || cJSON_AddStringToObject(ran, RUNTIME_ECOSYSTEM_HOST_ID, version) != NULL) &&
            (run->record == NULL || cJSON_AddStringToObject(call, "record", run->record) != NULL);
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static char *recorded(const own_run *run, const char *version) {
    cJSON *refusal = NULL;
    cJSON *answer = host_call(runtime_processes_record_run, run_call(run, version), &refusal);
    const char *record = host_text(answer, "record");
    char *copy = record == NULL ? NULL : malloc(strlen(record) + 1);
    if (copy != NULL) {
        strcpy(copy, record);
    }
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
    return copy;
}

void host_start_own_run(own_run *run, const host_context *context) {
    run->context = context;
    run->record = NULL;
    run->version = own_version(context);
    run->record = run->version == NULL ? NULL : recorded(run, run->version);
    if (run->version != NULL && run->record == NULL) {
        report(run->version, " could not be recorded as running");
    }
}

void host_stamp_own_run(own_run *run) {
    if (run->record != NULL) {
        free(recorded(run, run->version));
    }
}

void host_finish_own_run(own_run *run) {
    free(run->version);
    run->version = NULL;
    if (run->record == NULL) {
        return;
    }
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && (cJSON_AddStringToObject(call, "state", run->context->state) == NULL ||
                         cJSON_AddStringToObject(call, "hostId", RUNTIME_ECOSYSTEM_HOST_ID) == NULL ||
                         cJSON_AddStringToObject(call, "record", run->record) == NULL)) {
        cJSON_Delete(call);
        call = NULL;
    }
    cJSON *refusal = NULL;
    cJSON_Delete(host_call(runtime_processes_remove_runs, call, &refusal));
    cJSON_Delete(refusal);
    free(run->record);
    run->record = NULL;
}
