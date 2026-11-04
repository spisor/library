#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"

#if defined(_WIN32)
#include <windows.h>

#include "command_line_windows.h"
#include "wide_path.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

static char *problem_of(const char *executable, const char *reason) {
    const char *parts[4] = {"could not run ", executable, ": ", reason};
    return runtime_cache_text(parts, 4);
}

#if defined(_WIN32)

static wchar_t *wide_text(const char *text) {
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = units <= 0 ? NULL : malloc((size_t)units * sizeof *wide);
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, units) != units) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static size_t line_size(const char *executable, const cJSON *arguments) {
    size_t size = strlen(executable) + 3;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; argument != NULL; argument = argument->next) {
        size += 2 * strlen(argument->valuestring) + 3;
    }
    return size;
}

static wchar_t *command_line(const char *executable, const cJSON *arguments) {
    wchar_t *program = wide_text(executable);
    wchar_t *line = program == NULL ? NULL : malloc(line_size(executable, arguments) * sizeof *line);
    size_t used = 0;
    if (line != NULL) {
        line[used++] = L'"';
        wcscpy(line + used, program);
        used += wcslen(program);
        line[used++] = L'"';
    }
    for (const cJSON *argument = arguments == NULL || line == NULL ? NULL : arguments->child; argument != NULL;
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

static HANDLE null_device(void) {
    SECURITY_ATTRIBUTES inherited = {sizeof inherited, NULL, TRUE};
    return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                       OPEN_EXISTING, 0, NULL);
}

static int wait_for(HANDLE process, int *exit_code) {
    DWORD code = 0;
    int waited = WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code);
    *exit_code = (int)code;
    return waited;
}

static int create_silenced(const wchar_t *application, wchar_t *line, HANDLE null, PROCESS_INFORMATION *information) {
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null;
    startup.hStdOutput = null;
    startup.hStdError = null;
    return CreateProcessW(application, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, information) != 0;
}

int runtime_run_to_exit(const char *executable, const cJSON *arguments, int *exit_code, char **problem) {
    *problem = NULL;
    wchar_t *application = spisor_wide_path(executable);
    wchar_t *line = command_line(executable, arguments);
    HANDLE null = null_device();
    PROCESS_INFORMATION information = {NULL, NULL, 0, 0};
    int started = application != NULL && line != NULL && null != INVALID_HANDLE_VALUE &&
                  create_silenced(application, line, null, &information);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    int waited = started && wait_for(information.hProcess, exit_code);
    if (started) {
        error = waited ? ERROR_SUCCESS : GetLastError();
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
    }
    if (null != INVALID_HANDLE_VALUE) {
        CloseHandle(null);
    }
    free(line);
    free(application);
    if (!waited) {
        char reason[64];
        snprintf(reason, sizeof reason, "Windows error %lu", (unsigned long)error);
        *problem = problem_of(executable, reason);
    }
    return waited;
}

#else

static size_t argument_count(const cJSON *arguments) {
    size_t count = 0;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; argument != NULL; argument = argument->next) {
        count++;
    }
    return count;
}

static int spawn_silenced(const char *executable, char **list, pid_t *child) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return ENOMEM;
    }
    int error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    error = error != 0 ? error : posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    error = error != 0 ? error : posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    error = error != 0 ? error : posix_spawn(child, executable, &actions, NULL, list, environ);
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

static int wait_for(pid_t child, int *exit_code) {
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return waited == child;
}

int runtime_run_to_exit(const char *executable, const cJSON *arguments, int *exit_code, char **problem) {
    *problem = NULL;
    size_t count = argument_count(arguments);
    char **list = calloc(count + 2, sizeof *list);
    if (list == NULL) {
        return 0;
    }
    list[0] = (char *)executable;
    size_t index = 1;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; argument != NULL; argument = argument->next) {
        list[index++] = argument->valuestring;
    }
    pid_t child = 0;
    int error = spawn_silenced(executable, list, &child);
    free(list);
    int waited = error == 0 && wait_for(child, exit_code);
    if (!waited) {
        *problem = problem_of(executable, strerror(error != 0 ? error : errno));
    }
    return waited;
}

#endif
