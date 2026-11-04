#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "command_line_windows.h"
#include "host_child.h"
#include "host_context.h"
#include "host_output.h"
#include "wide_path.h"
#include "windows_command.h"

#define START_FIX "install the application again, or run forebay-host install --catalog <path or url>"
#define DEFAULT_COMMAND_SHELL "cmd.exe"

static wchar_t *wide_text(const char *text) {
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = units <= 0 ? NULL : malloc((size_t)units * sizeof *wide);
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, units) != units) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static size_t line_capacity(const wchar_t *program, const cJSON *arguments) {
    size_t capacity = wcslen(program) + 3;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; argument != NULL; argument = argument->next) {
        capacity += 2 * strlen(argument->valuestring) + 3;
    }
    return capacity + 1;
}

/** "program" and each argument quoted as CommandLineToArgvW reads it back. */
static wchar_t *direct_line(const char *executable, const cJSON *arguments) {
    wchar_t *program = wide_text(executable);
    wchar_t *line = program == NULL ? NULL : malloc(line_capacity(program, arguments) * sizeof *line);
    size_t used = 0;
    if (line != NULL) {
        line[used++] = L'"';
        wcscpy(line + used, program);
        used += wcslen(program);
        line[used++] = L'"';
    }
    for (const cJSON *argument = line == NULL || arguments == NULL ? NULL : arguments->child; argument != NULL;
         argument = argument->next) {
        wchar_t *wide = wide_text(argument->valuestring);
        if (wide == NULL) {
            free(line);
            line = NULL;
            break;
        }
        used = runtime_append_quoted_argument(line, used, wide);
        free(wide);
    }
    if (line != NULL) {
        line[used] = L'\0';
    }
    free(program);
    return line;
}

static const char *command_shell_of(const cJSON *environment) {
    for (const cJSON *entry = environment == NULL ? NULL : environment->child; entry != NULL; entry = entry->next) {
        if (_stricmp(entry->string, "ComSpec") == 0 && cJSON_IsString(entry)) {
            return entry->valuestring;
        }
    }
    const char *own = getenv("ComSpec");
    return own != NULL ? own : DEFAULT_COMMAND_SHELL;
}

/** "shell" /d /s /c "line", the line reaching cmd.exe verbatim. */
static wchar_t *shell_line(const char *batch, const cJSON *arguments, const cJSON *environment) {
    char *line = host_command_shell_line(batch, arguments);
    const char *shell = command_shell_of(environment);
    size_t length = line == NULL ? 0 : strlen(shell) + strlen(line) + 16;
    char *full = line == NULL ? NULL : malloc(length);
    if (full != NULL) {
        strcpy(full, "\"");
        strcat(full, shell);
        strcat(full, "\" /d /s /c ");
        strcat(full, line);
    }
    wchar_t *wide = full == NULL ? NULL : wide_text(full);
    free(full);
    free(line);
    return wide;
}

static int compare_entries(const void *left, const void *right) {
    return _wcsicmp(*(const wchar_t *const *)left, *(const wchar_t *const *)right);
}

/** The NAME=VALUE block CreateProcessW takes, sorted by name ignoring case as Windows keeps it. */
static wchar_t *environment_block(const cJSON *environment) {
    int count = cJSON_GetArraySize(environment);
    wchar_t **entries = calloc((size_t)count + 1, sizeof *entries);
    size_t total = 1;
    int used = 0;
    for (const cJSON *entry = entries == NULL ? NULL : environment->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsString(entry)) {
            continue;
        }
        char *line = malloc(strlen(entry->string) + strlen(entry->valuestring) + 2);
        if (line != NULL) {
            strcpy(line, entry->string);
            strcat(line, "=");
            strcat(line, entry->valuestring);
            entries[used] = wide_text(line);
        }
        free(line);
        if (entries[used] == NULL) {
            total = 0;
            break;
        }
        total += wcslen(entries[used++]) + 1;
    }
    wchar_t *block = entries == NULL || total == 0 ? NULL : malloc(total * sizeof *block);
    if (block != NULL) {
        qsort(entries, (size_t)used, sizeof *entries, compare_entries);
        size_t at = 0;
        for (int index = 0; index < used; index++) {
            wcscpy(block + at, entries[index]);
            at += wcslen(entries[index]) + 1;
        }
        block[at] = L'\0';
    }
    for (int index = 0; entries != NULL && index < count; index++) {
        free(entries[index]);
    }
    free(entries);
    return block;
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

static LPPROC_THREAD_ATTRIBUTE_LIST handle_list(HANDLE *handles, DWORD count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(size);
    if (attributes == NULL || !InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        free(attributes);
        return NULL;
    }
    if (count > 0 && !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                                count * sizeof handles[0], NULL, NULL)) {
        DeleteProcThreadAttributeList(attributes);
        free(attributes);
        return NULL;
    }
    return attributes;
}

/** Starts the child with the console and standard streams of this process and only those; 1 on success. */
static int create(const wchar_t *application, wchar_t *line, wchar_t *block, PROCESS_INFORMATION *information) {
    HANDLE handles[3];
    DWORD count = standard_handles(handles);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = handle_list(handles, count);
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.lpAttributeList = attributes;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | (block != NULL ? CREATE_UNICODE_ENVIRONMENT : 0);
    int created = attributes != NULL &&
                  CreateProcessW(application, line, NULL, NULL, count > 0, flags, block, NULL, &startup.StartupInfo, information);
    if (attributes != NULL) {
        DeleteProcThreadAttributeList(attributes);
        free(attributes);
    }
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

int host_run_child(const char *executable, const cJSON *arguments, const cJSON *environment, host_child_started started,
                   void *context, int *exit_code, cJSON **refusal) {
    *refusal = NULL;
    int through_shell = host_needs_command_shell(executable);
    wchar_t *application = through_shell ? NULL : spisor_wide_path(executable);
    wchar_t *line = through_shell ? shell_line(executable, arguments, environment) : direct_line(executable, arguments);
    wchar_t *block = environment == NULL ? NULL : environment_block(environment);
    PROCESS_INFORMATION information = {NULL, NULL, 0, 0};
    int prepared = line != NULL && (through_shell || application != NULL) && (environment == NULL || block != NULL);
    SetConsoleCtrlHandler(outlive_stop, TRUE);
    int created = prepared && create(application, line, block, &information);
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    free(block);
    free(line);
    free(application);
    if (created) {
        CloseHandle(information.hThread);
        if (started != NULL) {
            started((double)information.dwProcessId, context);
        }
        DWORD code = 1;
        WaitForSingleObject(information.hProcess, INFINITE);
        GetExitCodeProcess(information.hProcess, &code);
        CloseHandle(information.hProcess);
        *exit_code = (int)code;
    } else if (prepared) {
        *refusal = start_refusal(executable, error);
    }
    SetConsoleCtrlHandler(outlive_stop, FALSE);
    return created;
}

cJSON *host_replace_process(const char *executable, const cJSON *arguments) {
    cJSON *refusal = NULL;
    int code = 1;
    if (host_run_child(executable, arguments, NULL, NULL, NULL, &code, &refusal)) {
        exit(code);
    }
    return refusal;
}
