#if defined(__linux__)
#define _GNU_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"

#define POLL_MILLISECONDS 10
#define RUN_OUTPUT_GRACE_MILLISECONDS 250

#if defined(_WIN32)
#include <windows.h>

#include "command_line_windows.h"
#include "wide_path.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif

/** What a run collected of its standard output. */
typedef struct run_output {
    char bytes[RUNTIME_RUN_OUTPUT_LIMIT + 1];
    size_t length;
} run_output;

static char *problem_of(const char *executable, const char *reason) {
    const char *parts[4] = {"could not run ", executable, ": ", reason};
    return runtime_cache_text(parts, 4);
}

static void keep(run_output *collected, const char *bytes, size_t length) {
    size_t room = RUNTIME_RUN_OUTPUT_LIMIT - collected->length;
    size_t kept = length < room ? length : room;
    memcpy(collected->bytes + collected->length, bytes, kept);
    collected->length += kept;
}

static int answer_output(const run_output *collected, char **output) {
    if (output == NULL) {
        return 1;
    }
    *output = malloc(collected->length + 1);
    if (*output == NULL) {
        return 0;
    }
    memcpy(*output, collected->bytes, collected->length);
    (*output)[collected->length] = 0;
    return 1;
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

/** The handles a run hands its child: the null device, and the write end of the output pipe when one is taken. */
typedef struct windows_run {
    HANDLE null;
    HANDLE output_read;
    HANDLE output_write;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes;
    HANDLE inherited[2];
} windows_run;

static HANDLE null_device(void) {
    SECURITY_ATTRIBUTES inherited = {sizeof inherited, NULL, TRUE};
    return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                       OPEN_EXISTING, 0, NULL);
}

static int open_output_pipe(windows_run *run) {
    SECURITY_ATTRIBUTES inherited = {sizeof inherited, NULL, TRUE};
    if (!CreatePipe(&run->output_read, &run->output_write, &inherited, 0)) {
        run->output_read = NULL;
        run->output_write = NULL;
        return 0;
    }
    return SetHandleInformation(run->output_read, HANDLE_FLAG_INHERIT, 0) != 0;
}

static LPPROC_THREAD_ATTRIBUTE_LIST handle_list(windows_run *run, DWORD count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(size);
    if (attributes == NULL || !InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        free(attributes);
        return NULL;
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, run->inherited,
                                   count * sizeof run->inherited[0], NULL, NULL)) {
        DeleteProcThreadAttributeList(attributes);
        free(attributes);
        return NULL;
    }
    return attributes;
}

static int prepare(windows_run *run, int capture) {
    memset(run, 0, sizeof *run);
    run->null = null_device();
    if (run->null == INVALID_HANDLE_VALUE || (capture && !open_output_pipe(run))) {
        return 0;
    }
    run->inherited[0] = run->null;
    run->inherited[1] = run->output_write;
    run->attributes = handle_list(run, capture ? 2 : 1);
    return run->attributes != NULL;
}

static void close_handle(HANDLE *handle) {
    if (*handle != NULL && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
    }
    *handle = NULL;
}

static void release(windows_run *run) {
    if (run->attributes != NULL) {
        DeleteProcThreadAttributeList(run->attributes);
        free(run->attributes);
    }
    close_handle(&run->null);
    close_handle(&run->output_read);
    close_handle(&run->output_write);
}

static int create(const wchar_t *application, wchar_t *line, windows_run *run, PROCESS_INFORMATION *information) {
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = run->null;
    startup.StartupInfo.hStdOutput = run->output_write != NULL ? run->output_write : run->null;
    startup.StartupInfo.hStdError = run->null;
    startup.lpAttributeList = run->attributes;
    return CreateProcessW(application, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL,
                          NULL, &startup.StartupInfo, information) != 0;
}

/** Reads whatever the pipe holds now, keeping what fits; 0 once its write end is closed everywhere. */
static int drain(HANDLE pipe, run_output *collected) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
        return 0;
    }
    char buffer[512];
    while (available > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, available < sizeof buffer ? available : sizeof buffer, &read, NULL) || read == 0) {
            return 0;
        }
        keep(collected, buffer, read);
        available -= read;
    }
    return 1;
}

/**
 * 1 with the exit code once process exits within timeout, its output drained meanwhile; 0 after
 * terminating one that did not.
 *
 * @implNote The pipe is drained while the child runs, so a child writing more than the pipe holds never
 * blocks, and for at most RUN_OUTPUT_GRACE_MILLISECONDS after it exits, so a grandchild that inherited
 * the write end, as a node shim's node does, never holds the run open.
 */
static int await_exit(windows_run *run, HANDLE process, unsigned timeout_milliseconds, int *exit_code,
                      run_output *collected) {
    ULONGLONG deadline = GetTickCount64() + timeout_milliseconds;
    ULONGLONG exited_at = 0;
    int open = run->output_read != NULL;
    for (;;) {
        open = open && drain(run->output_read, collected);
        int exited = WaitForSingleObject(process, POLL_MILLISECONDS) == WAIT_OBJECT_0;
        ULONGLONG now = GetTickCount64();
        exited_at = exited && exited_at == 0 ? now : exited_at;
        if (exited && (!open || now - exited_at >= RUN_OUTPUT_GRACE_MILLISECONDS || now >= deadline)) {
            break;
        }
        if (!exited && now >= deadline) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
            return 0;
        }
    }
    DWORD code = 0;
    int read = GetExitCodeProcess(process, &code) != 0;
    *exit_code = (int)code;
    return read;
}

int runtime_run_to_exit(const char *executable, const cJSON *arguments, unsigned timeout_milliseconds, int *exit_code,
                        char **output, char **problem) {
    *problem = NULL;
    wchar_t *application = spisor_wide_path(executable);
    wchar_t *line = command_line(executable, arguments);
    windows_run run;
    memset(&run, 0, sizeof run);
    PROCESS_INFORMATION information = {NULL, NULL, 0, 0};
    run_output collected = {{0}, 0};
    int started = application != NULL && line != NULL && prepare(&run, output != NULL) &&
                  create(application, line, &run, &information);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    int exited = 0;
    if (started) {
        close_handle(&run.output_write);
        exited = await_exit(&run, information.hProcess, timeout_milliseconds, exit_code, &collected);
        CloseHandle(information.hThread);
        CloseHandle(information.hProcess);
    }
    release(&run);
    free(line);
    free(application);
    if (!started) {
        char reason[64];
        snprintf(reason, sizeof reason, "Windows error %lu", (unsigned long)error);
        *problem = problem_of(executable, reason);
    } else if (!exited) {
        *problem = problem_of(executable, "it did not exit in time and was stopped");
    }
    return exited && answer_output(&collected, output);
}

#else

static double now_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static void pause_briefly(void) {
    struct timespec pause = {0, 10 * 1000000L};
    nanosleep(&pause, NULL);
}

static size_t argument_count(const cJSON *arguments) {
    size_t count = 0;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; argument != NULL; argument = argument->next) {
        count++;
    }
    return count;
}

static int spawn_silenced(const char *executable, char **list, const int *output_pipe, pid_t *child) {
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return ENOMEM;
    }
    int error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (output_pipe == NULL) {
        error = error != 0 ? error : posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    } else {
        error = error != 0 ? error : posix_spawn_file_actions_adddup2(&actions, output_pipe[1], 1);
        error = error != 0 ? error : posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
        if (output_pipe[1] != 1) {
            error = error != 0 ? error : posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
        }
    }
    error = error != 0 ? error : posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    error = error != 0 ? error : posix_spawn(child, executable, &actions, NULL, list, environ);
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

/** Reads whatever descriptor holds within one poll interval, keeping what fits; 0 once it reached its end. */
static int drain(int descriptor, run_output *collected) {
    struct pollfd waiting = {descriptor, POLLIN, 0};
    char buffer[512];
    int ready = poll(&waiting, 1, POLL_MILLISECONDS);
    if (ready <= 0) {
        return ready == 0 || errno == EINTR;
    }
    ssize_t read_now = read(descriptor, buffer, sizeof buffer);
    if (read_now > 0) {
        keep(collected, buffer, (size_t)read_now);
    }
    return read_now > 0 || (read_now < 0 && errno == EINTR);
}

static void stop(pid_t child) {
    int status = 0;
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

static int exit_code_of(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/**
 * 1 with the exit code once child exits by the deadline, its output drained meanwhile; 0 after killing
 * one that did not.
 *
 * @implNote The output is drained for at most RUN_OUTPUT_GRACE_MILLISECONDS after the child exits, so a
 * grandchild that inherited the write end, as a node shim's node does, never holds the run open.
 */
static int await_exit(pid_t child, int descriptor, double deadline, int *exit_code, run_output *collected) {
    int open = descriptor >= 0;
    double exited_at = 0;
    for (;;) {
        if (open) {
            open = drain(descriptor, collected);
        } else if (exited_at == 0) {
            pause_briefly();
        }
        int status = 0;
        pid_t waited = exited_at == 0 ? waitpid(child, &status, WNOHANG) : child;
        double now = now_milliseconds();
        if (waited == child && exited_at == 0) {
            *exit_code = exit_code_of(status);
            exited_at = now;
        }
        if (exited_at != 0 && (!open || now - exited_at >= RUN_OUTPUT_GRACE_MILLISECONDS || now >= deadline)) {
            return 1;
        }
        if (exited_at == 0 && ((waited < 0 && errno != EINTR) || now > deadline)) {
            stop(child);
            return 0;
        }
    }
}

static char **argument_list(const char *executable, const cJSON *arguments) {
    char **list = calloc(argument_count(arguments) + 2, sizeof *list);
    size_t index = 0;
    if (list != NULL) {
        list[index++] = (char *)executable;
    }
    for (const cJSON *argument = list == NULL || arguments == NULL ? NULL : arguments->child; argument != NULL;
         argument = argument->next) {
        list[index++] = argument->valuestring;
    }
    return list;
}

/**
 * @implNote Both ends close on exec, at once through pipe2 where it exists, so no process another thread
 * starts meanwhile carries either away; the child gets the write end only as its standard output.
 */
static int open_output_pipe(int ends[2]) {
#if defined(__linux__)
    return pipe2(ends, O_CLOEXEC) == 0;
#else
    if (pipe(ends) != 0) {
        return 0;
    }
    if (fcntl(ends[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(ends[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(ends[0]);
        close(ends[1]);
        return 0;
    }
    return 1;
#endif
}

int runtime_run_to_exit(const char *executable, const cJSON *arguments, unsigned timeout_milliseconds, int *exit_code,
                        char **output, char **problem) {
    *problem = NULL;
    double deadline = now_milliseconds() + timeout_milliseconds;
    int ends[2] = {-1, -1};
    char **list = argument_list(executable, arguments);
    if (list == NULL || (output != NULL && !open_output_pipe(ends))) {
        free(list);
        return 0;
    }
    pid_t child = 0;
    int error = spawn_silenced(executable, list, output == NULL ? NULL : ends, &child);
    free(list);
    run_output collected = {{0}, 0};
    if (ends[1] >= 0) {
        close(ends[1]);
    }
    int exited = error == 0 && await_exit(child, ends[0], deadline, exit_code, &collected);
    if (ends[0] >= 0) {
        close(ends[0]);
    }
    if (error != 0) {
        *problem = problem_of(executable, strerror(error));
    } else if (!exited) {
        *problem = problem_of(executable, "it did not exit in time and was stopped");
    }
    return exited && answer_output(&collected, output);
}

#endif
