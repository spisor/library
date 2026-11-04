#if defined(__linux__)
#define _GNU_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_start.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#include "windows_process.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif

static char *problem_of(const char *executable, const char *reason) {
    const char *parts[4] = {"the background host could not be started from ", executable, ": ", reason};
    return runtime_cache_text(parts, 4);
}

static size_t text_count(const cJSON *list) {
    size_t count = 0;
    for (const cJSON *item = list == NULL ? NULL : list->child; item != NULL; item = item->next) {
        count++;
    }
    return count;
}

#if defined(_WIN32)

typedef struct windows_start {
    wchar_t *application;
    wchar_t *line;
    wchar_t *environment;
    wchar_t *directory;
    HANDLE null;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes;
} windows_start;

static void free_windows_start(windows_start *prepared) {
    runtime_windows_free_handle_list(prepared->attributes);
    if (prepared->null != INVALID_HANDLE_VALUE) {
        CloseHandle(prepared->null);
    }
    free(prepared->application);
    free(prepared->line);
    free(prepared->environment);
    free(prepared->directory);
}

static int prepare_windows_start(const runtime_detached_start *start, windows_start *prepared) {
    memset(prepared, 0, sizeof *prepared);
    prepared->null = runtime_windows_null_device();
    prepared->application = spisor_wide_path(start->executable);
    prepared->line = runtime_windows_command_line(start->executable, start->arguments);
    prepared->environment = start->environment == NULL ? NULL : runtime_windows_environment_block(start->environment);
    prepared->directory = spisor_wide_path(start->directory);
    prepared->attributes = prepared->null == INVALID_HANDLE_VALUE ? NULL : runtime_windows_handle_list(&prepared->null, 1);
    return prepared->application != NULL && prepared->line != NULL && prepared->directory != NULL &&
           prepared->attributes != NULL && (start->environment == NULL || prepared->environment != NULL);
}

static int create_process(windows_start *prepared, DWORD flags, PROCESS_INFORMATION *information) {
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = prepared->null;
    startup.StartupInfo.hStdOutput = prepared->null;
    startup.StartupInfo.hStdError = prepared->null;
    startup.lpAttributeList = prepared->attributes;
    return CreateProcessW(prepared->application, prepared->line, NULL, NULL, TRUE, flags, prepared->environment,
                          prepared->directory, &startup.StartupInfo, information) != 0;
}

static char *windows_problem(const char *executable, DWORD error) {
    char reason[64];
    snprintf(reason, sizeof reason, "Windows error %lu", (unsigned long)error);
    return problem_of(executable, reason);
}

int runtime_start_detached(const runtime_detached_start *start, double *pid, int *outside_job, char **problem) {
    *problem = NULL;
    windows_start prepared;
    if (!prepare_windows_start(start, &prepared)) {
        free_windows_start(&prepared);
        *problem = problem_of(start->executable, "its path, arguments or environment are not valid UTF-8 text");
        return 0;
    }
    DWORD flags = CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT |
                  EXTENDED_STARTUPINFO_PRESENT;
    PROCESS_INFORMATION information;
    *outside_job = create_process(&prepared, flags | CREATE_BREAKAWAY_FROM_JOB, &information);
    DWORD error = *outside_job ? ERROR_SUCCESS : GetLastError();
    int started = *outside_job || (error == ERROR_ACCESS_DENIED && create_process(&prepared, flags, &information));
    if (!started) {
        *problem = windows_problem(start->executable, error == ERROR_ACCESS_DENIED ? GetLastError() : error);
    }
    free_windows_start(&prepared);
    if (!started) {
        return 0;
    }
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    *pid = (double)information.dwProcessId;
    return 1;
}

#else

#define LAST_SIGNAL 64
#define HANDBACK_MILLISECONDS 5000

static void free_list(char **list, size_t count) {
    for (size_t index = 0; list != NULL && index < count; index++) {
        free(list[index]);
    }
    free(list);
}

static char **argument_list(const char *executable, const cJSON *arguments) {
    size_t count = text_count(arguments);
    char **list = calloc(count + 2, sizeof *list);
    const char *parts[1] = {executable};
    int built = list != NULL && (list[0] = runtime_cache_text(parts, 1)) != NULL;
    size_t index = 1;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; built && argument != NULL;
         argument = argument->next) {
        const char *value[1] = {argument->valuestring};
        built = (list[index++] = runtime_cache_text(value, 1)) != NULL;
    }
    if (!built) {
        free_list(list, count + 1);
        return NULL;
    }
    return list;
}

static char **environment_list(const cJSON *environment) {
    size_t count = text_count(environment);
    char **list = calloc(count + 1, sizeof *list);
    int built = list != NULL;
    size_t index = 0;
    for (const cJSON *variable = built ? environment->child : NULL; built && variable != NULL; variable = variable->next) {
        const char *parts[3] = {variable->string, "=", variable->valuestring};
        built = (list[index++] = runtime_cache_text(parts, 3)) != NULL;
    }
    if (!built) {
        free_list(list, count);
        return NULL;
    }
    return list;
}

/**
 * @implNote pipe2 creates both ends close-on-exec at once where it exists, so no other thread's fork and
 * exec between pipe and fcntl can carry a write end away; elsewhere that gap remains, and the reads that
 * wait on these pipes are bounded instead.
 */
static int close_on_exec_pipe(int ends[2]) {
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

static void write_all(int descriptor, const void *bytes, size_t length) {
    const char *cursor = bytes;
    while (length > 0) {
        ssize_t written = write(descriptor, cursor, length);
        if (written <= 0 && errno != EINTR) {
            return;
        }
        if (written > 0) {
            cursor += written;
            length -= (size_t)written;
        }
    }
}

static double now_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

/** Whether descriptor has something to read, or reached its end, before the deadline. */
static int readable_before(int descriptor, double deadline) {
    struct pollfd waiting = {descriptor, POLLIN, 0};
    for (;;) {
        double left = deadline - now_milliseconds();
        if (left <= 0) {
            return 0;
        }
        int ready = poll(&waiting, 1, (int)left + 1);
        if (ready > 0) {
            return 1;
        }
        if (ready == 0 || errno != EINTR) {
            return 0;
        }
    }
}

/** Reads up to length bytes, stopping at the end, an error, or HANDBACK_MILLISECONDS without data. */
static size_t read_all(int descriptor, void *bytes, size_t length) {
    char *cursor = bytes;
    size_t got = 0;
    double deadline = now_milliseconds() + HANDBACK_MILLISECONDS;
    while (got < length && readable_before(descriptor, deadline)) {
        ssize_t read_now = read(descriptor, cursor + got, length - got);
        if (read_now == 0 || (read_now < 0 && errno != EINTR)) {
            break;
        }
        if (read_now > 0) {
            got += (size_t)read_now;
        }
    }
    return got;
}

/** In the started host itself, between fork and exec: only async-signal-safe calls. */
static void become_host(const char *directory, char **arguments, char **environment, int failure) {
    struct sigaction default_action;
    memset(&default_action, 0, sizeof default_action);
    default_action.sa_handler = SIG_DFL;
    for (int signal_number = 1; signal_number <= LAST_SIGNAL; signal_number++) {
        sigaction(signal_number, &default_action, NULL);
    }
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    int null = open("/dev/null", O_RDWR);
    if (chdir(directory) == 0 && null >= 0 && dup2(null, 0) >= 0 && dup2(null, 1) >= 0 && dup2(null, 2) >= 0) {
        if (null > 2) {
            close(null);
        }
        execve(arguments[0], arguments, environment);
    }
    int error = errno;
    write_all(failure, &error, sizeof error);
    _exit(127);
}

/** The first child: a new session, then the host as its child, whose pid it hands back before it exits. */
static void start_in_new_session(const char *directory, char **arguments, char **environment, int pid_end,
                                 int failure) {
    setsid();
    pid_t host = fork();
    if (host == 0) {
        become_host(directory, arguments, environment, failure);
    }
    if (host > 0) {
        write_all(pid_end, &host, sizeof host);
    }
    _exit(host > 0 ? 0 : 1);
}

/**
 * @implNote The failure pipe reaches its end when the host execs; a write end some other process carried
 * away would hold that end back, so a wait past HANDBACK_MILLISECONDS counts as started rather than
 * keeping the holders lock for ever. A failed exec writes at once, well inside it.
 */
static int await_host(pid_t first, int pid_end, int failure, double *pid, int *error) {
    pid_t host = 0;
    int got_pid = read_all(pid_end, &host, sizeof host) == sizeof host;
    *error = 0;
    int failed = read_all(failure, error, sizeof *error) == sizeof *error;
    while (waitpid(first, NULL, 0) < 0 && errno == EINTR) {
    }
    if (got_pid && !failed) {
        *pid = (double)host;
    }
    return got_pid && !failed;
}

static int spawn_detached(const char *directory, char **arguments, char **environment, double *pid, int *error) {
    int pid_pipe[2];
    int failure_pipe[2];
    if (!close_on_exec_pipe(pid_pipe)) {
        *error = errno;
        return 0;
    }
    if (!close_on_exec_pipe(failure_pipe)) {
        *error = errno;
        close(pid_pipe[0]);
        close(pid_pipe[1]);
        return 0;
    }
    pid_t first = fork();
    if (first == 0) {
        start_in_new_session(directory, arguments, environment, pid_pipe[1], failure_pipe[1]);
    }
    close(pid_pipe[1]);
    close(failure_pipe[1]);
    int started = first > 0 && await_host(first, pid_pipe[0], failure_pipe[0], pid, error);
    if (first < 0) {
        *error = errno;
    }
    close(pid_pipe[0]);
    close(failure_pipe[0]);
    return started;
}

int runtime_start_detached(const runtime_detached_start *start, double *pid, int *outside_job, char **problem) {
    *problem = NULL;
    *outside_job = 1;
    char **arguments = argument_list(start->executable, start->arguments);
    char **environment = start->environment == NULL ? NULL : environment_list(start->environment);
    int error = ENOMEM;
    int started = arguments != NULL && (start->environment == NULL || environment != NULL) &&
                  spawn_detached(start->directory, arguments, environment == NULL ? environ : environment, pid, &error);
    if (!started) {
        *problem = problem_of(start->executable, error == 0 ? "it did not start" : strerror(error));
    }
    free_list(arguments, text_count(start->arguments) + 1);
    free_list(environment, text_count(start->environment));
    return started;
}

#endif
