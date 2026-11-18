#include "own_run.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "boot_stub.h"
#include "host_output.h"
#include "install_run.h"
#include "cache_place.h"
#include "own_executable.h"
#include "paths.h"
#include "runtime/processes.h"

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
 * freed, NULL when it cannot be opened.
 */
static char *final_path(const char *path) {
    wchar_t *wide = spisor_wide_path(path);
    HANDLE handle = wide == NULL ? INVALID_HANDLE_VALUE
                                 : CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    spisor_memory_release(wide);
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
    char *narrow = bytes <= 0 ? NULL : spisor_memory_allocate((size_t)bytes);
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

/** The version segment of the executable below cache/bundles/spisor-host, freed; NULL outside it. */
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
        version = start[length] == 0 ? NULL : spisor_memory_allocate(length + 1);
        if (version != NULL) {
            memcpy(version, start, length);
            version[length] = 0;
        } else {
            report(executable, " lies in the bundles folder but in no version of it");
        }
    }
    spisor_memory_release(resolved);
    spisor_memory_release(folder);
    spisor_memory_release(joined);
    spisor_memory_release(executable);
    return version;
}

static char *recorded(const own_run *run, const char *version) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_item_version ran = {RUNTIME_ECOSYSTEM_HOST_ID, version};
    runtime_processes_record_argument argument = SPISOR_INIT(runtime_processes_record_argument);
    runtime_processes_record_answer answer = SPISOR_INIT(runtime_processes_record_answer);
    argument.state = run->context->state;
    argument.host_id = RUNTIME_ECOSYSTEM_HOST_ID;
    argument.run = version == NULL ? NULL : &ran;
    argument.run_count = version == NULL ? 0 : 1;
    argument.check_milliseconds = HOST_OWN_RUN_INTERVAL_MILLISECONDS;
    argument.record = run->record;
    char *copy = NULL;
    if (processes->record_run(processes->context, &argument, &answer, NULL) == SPISOR_OK) {
        copy = spisor_memory_allocate(strlen(answer.record) + 1);
    }
    if (copy != NULL) {
        strcpy(copy, answer.record);
    }
    processes->release_record_answer(processes->context, &answer);
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
        spisor_memory_release(recorded(run, run->version));
    }
}

void host_finish_own_run(own_run *run) {
    spisor_memory_release(run->version);
    run->version = NULL;
    if (run->record == NULL) {
        return;
    }
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_remove_argument argument = SPISOR_INIT(runtime_processes_remove_argument);
    argument.state = run->context->state;
    argument.host_id = RUNTIME_ECOSYSTEM_HOST_ID;
    argument.record = run->record;
    processes->remove_runs(processes->context, &argument, NULL);
    spisor_memory_release(run->record);
    run->record = NULL;
}
