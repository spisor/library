#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "host_child.h"
#include "host_service.h"
#include "host_output.h"
#include "wide_path.h"
#include "windows_command.h"
#include "windows_process.h"

#define START_FIX "install the application again, or run spisor-host install --catalog <path or url>"
#define DEFAULT_COMMAND_SHELL L"cmd.exe"

/** The command shell environment names under ComSpec, matched ignoring case, else this process's own, else cmd.exe. */
static wchar_t *command_shell_of(const cJSON *environment) {
    for (const cJSON *entry = environment == NULL ? NULL : environment->child; entry != NULL; entry = entry->next) {
        if (_stricmp(entry->string, "ComSpec") == 0 && cJSON_IsString(entry)) {
            return runtime_windows_text(entry->valuestring);
        }
    }
    DWORD units = GetEnvironmentVariableW(L"ComSpec", NULL, 0);
    wchar_t *shell = malloc((units == 0 ? wcslen(DEFAULT_COMMAND_SHELL) + 1 : units) * sizeof *shell);
    if (shell != NULL && (units == 0 || GetEnvironmentVariableW(L"ComSpec", shell, units) != units - 1)) {
        wcscpy(shell, DEFAULT_COMMAND_SHELL);
    }
    return shell;
}

/** "shell" /d /s /c "line", the line reaching cmd.exe verbatim. */
static wchar_t *shell_line(const char *batch, const cJSON *arguments, const cJSON *environment) {
    char *narrow_line = host_command_shell_line(batch, arguments);
    wchar_t *line = narrow_line == NULL ? NULL : runtime_windows_text(narrow_line);
    wchar_t *shell = line == NULL ? NULL : command_shell_of(environment);
    size_t length = shell == NULL ? 0 : wcslen(shell) + wcslen(line) + 16;
    wchar_t *full = shell == NULL ? NULL : malloc(length * sizeof *full);
    if (full != NULL) {
        swprintf(full, length, L"\"%ls\" /d /s /c %ls", shell, line);
    }
    free(shell);
    free(line);
    free(narrow_line);
    return full;
}

/** The standard handles this process has, made inheritable, each once; how many. */
static DWORD standard_handles(HANDLE handles[3]) {
    const DWORD names[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    DWORD count = 0;
    for (int index = 0; index < 3; index++) {
        HANDLE handle = GetStdHandle(names[index]);
        int known = 0;
        for (DWORD seen = 0; seen < count; seen++) {
            known = known || handles[seen] == handle;
        }
        if (handle != NULL && handle != INVALID_HANDLE_VALUE && !known &&
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            handles[count++] = handle;
        }
    }
    return count;
}

/** Starts the child with the console and standard streams of this process and only those, and flags; 1 on success. */
static int create(const wchar_t *application, wchar_t *line, wchar_t *block, DWORD extra_flags, PROCESS_INFORMATION *information) {
    HANDLE handles[3];
    DWORD count = standard_handles(handles);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = runtime_windows_handle_list(handles, count);
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.lpAttributeList = attributes;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | (block != NULL ? CREATE_UNICODE_ENVIRONMENT : 0) | extra_flags;
    int created = attributes != NULL &&
                  CreateProcessW(application, line, NULL, NULL, count > 0, flags, block, NULL, &startup.StartupInfo, information);
    runtime_windows_free_handle_list(attributes);
    return created;
}

/** Handles Ctrl+C and Ctrl+Break, which the console delivers to the child as well, so this process waits for it. */
static BOOL WINAPI outlive_stop(DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

static cJSON *start_refusal(const char *executable, DWORD error) {
    char reason[64];
    snprintf(reason, sizeof reason, "Windows error %lu", (unsigned long)error);
    const char *parts[3] = {executable, " could not be started: ", reason};
    return host_refusal_of(parts, 3, START_FIX);
}

/** Waits for process to exit, ticking watch meanwhile; 1 with its exit code, 0 with *error when the wait failed. */
static int wait_for(HANDLE process, const host_child_watch *watch, int *exit_code, DWORD *error) {
    DWORD interval = watch != NULL && watch->tick != NULL ? watch->interval_milliseconds : INFINITE;
    DWORD waited = WAIT_TIMEOUT;
    while ((waited = WaitForSingleObject(process, interval)) == WAIT_TIMEOUT) {
        watch->tick(watch->context);
    }
    DWORD code = STILL_ACTIVE;
    if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE) {
        *error = waited == WAIT_OBJECT_0 ? GetLastError() : waited;
        return 0;
    }
    *exit_code = (int)code;
    return 1;
}

static cJSON *wait_refusal(const char *executable, DWORD error) {
    char reason[64];
    snprintf(reason, sizeof reason, "Windows error %lu", (unsigned long)error);
    const char *parts[3] = {"spisor could not wait for ", executable, " to exit: "};
    const char *fix_parts[2] = {reason, "; the application may still be running, so check it before starting it again"};
    char fix[256];
    snprintf(fix, sizeof fix, "%s%s", fix_parts[0], fix_parts[1]);
    return host_refusal_of(parts, 3, fix);
}

static int run_child(const char *executable, const cJSON *arguments, const cJSON *environment, const host_child_watch *watch,
                     DWORD extra_flags, int *exit_code, cJSON **refusal) {
    *refusal = NULL;
    int through_shell = host_needs_command_shell(executable);
    wchar_t *application = through_shell ? NULL : spisor_wide_path(executable);
    wchar_t *line = through_shell ? shell_line(executable, arguments, environment)
                                  : runtime_windows_command_line(executable, arguments);
    wchar_t *block = environment == NULL ? NULL : runtime_windows_environment_block(environment);
    PROCESS_INFORMATION information = {NULL, NULL, 0, 0};
    int prepared = line != NULL && (through_shell || application != NULL) && (environment == NULL || block != NULL);
    SetConsoleCtrlHandler(outlive_stop, TRUE);
    int created = prepared && create(application, line, block, extra_flags, &information);
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    free(block);
    free(line);
    free(application);
    if (created) {
        CloseHandle(information.hThread);
        if (watch != NULL && watch->started != NULL) {
            watch->started((double)information.dwProcessId, watch->context);
        }
        DWORD wait_error = ERROR_SUCCESS;
        if (!wait_for(information.hProcess, watch, exit_code, &wait_error)) {
            *refusal = wait_refusal(executable, wait_error);
            created = 0;
        }
        CloseHandle(information.hProcess);
    } else if (prepared) {
        *refusal = start_refusal(executable, error);
    }
    SetConsoleCtrlHandler(outlive_stop, FALSE);
    return created;
}

int host_run_child(const char *executable, const cJSON *arguments, const cJSON *environment, const host_child_watch *watch,
                   int *exit_code, cJSON **refusal) {
    return run_child(executable, arguments, environment, watch, 0, exit_code, refusal);
}

/**
 * CREATE_NO_WINDOW when this process has no console at all, else nothing, so the child shares the console
 * it has, a windowless one included.
 *
 * @implNote Without it Windows gives the console child of a console-less process a console window of its
 * own, which flashes open on a SessionStart run by an application that spawns its hooks detached. The test
 * is GetConsoleProcessList, not GetConsoleWindow: the latter is NULL in the windowless console a windowsHide
 * spawn makes too, where a console of the child's own cost about 20 ms per hook on Windows Debug.
 */
static DWORD replacing_flags(void) {
    DWORD attached[1];
    return GetConsoleProcessList(attached, 1) == 0 ? CREATE_NO_WINDOW : 0;
}

cJSON *host_replace_process(const char *executable, const cJSON *arguments, const host_child_watch *watch) {
    cJSON *refusal = NULL;
    int code = 1;
    if (run_child(executable, arguments, NULL, watch, replacing_flags(), &code, &refusal)) {
        exit(code);
    }
    return refusal;
}
