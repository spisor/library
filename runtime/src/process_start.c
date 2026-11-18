#if defined(__linux__)
#define _GNU_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_start.h"
#include "allocation.h"

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

/** How long a cancelled host is waited for once terminated, which a suspended process never nears. */
#define CANCEL_WAIT_MILLISECONDS 10000

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
    spisor_memory_release(prepared->application);
    spisor_memory_release(prepared->line);
    spisor_memory_release(prepared->environment);
    spisor_memory_release(prepared->directory);
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

static int are_convertible_texts(const cJSON *texts) {
    const cJSON *text;
    cJSON_ArrayForEach(text, texts) {
        if (!cJSON_IsString(text) || !spisor_is_convertible_path(text->valuestring) ||
            (text->string != NULL && !spisor_is_convertible_path(text->string))) {
            return 0;
        }
    }
    return 1;
}

/** Whether every text the start hands Windows is valid UTF-8, read without allocating. */
static int is_convertible_start(const runtime_detached_start *start) {
    return spisor_is_convertible_path(start->executable) && spisor_is_convertible_path(start->directory) &&
           are_convertible_texts(start->arguments) && are_convertible_texts(start->environment);
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

int runtime_start_detached(const runtime_detached_start *start, runtime_detached_host *host, char **problem) {
    *problem = NULL;
    memset(host, 0, sizeof *host);
    host->executable = start->executable;
    if (!is_convertible_start(start)) {
        *problem = problem_of(start->executable, "its path, arguments or environment are not valid UTF-8 text");
        return 0;
    }
    windows_start prepared;
    if (!prepare_windows_start(start, &prepared)) {
        int unopened = prepared.null == INVALID_HANDLE_VALUE;
        free_windows_start(&prepared);
        *problem = unopened ? problem_of(start->executable, "the null device could not be opened") : NULL;
        return 0;
    }
    DWORD flags = CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT |
                  EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    PROCESS_INFORMATION information;
    host->outside_job = create_process(&prepared, flags | CREATE_BREAKAWAY_FROM_JOB, &information);
    DWORD error = host->outside_job ? ERROR_SUCCESS : GetLastError();
    int started = host->outside_job || (error == ERROR_ACCESS_DENIED && create_process(&prepared, flags, &information));
    if (!started) {
        *problem = windows_problem(start->executable, error == ERROR_ACCESS_DENIED ? GetLastError() : error);
    }
    free_windows_start(&prepared);
    if (!started) {
        return 0;
    }
    host->process = information.hProcess;
    host->thread = information.hThread;
    host->pid = (double)information.dwProcessId;
    return 1;
}

static void close_host(runtime_detached_host *host) {
    CloseHandle(host->thread);
    CloseHandle(host->process);
    host->thread = NULL;
    host->process = NULL;
}

int runtime_release_detached(runtime_detached_host *host, char **problem) {
    *problem = NULL;
    int resumed = ResumeThread(host->thread) != (DWORD)-1;
    if (!resumed) {
        *problem = windows_problem(host->executable, GetLastError());
        TerminateProcess(host->process, 1);
    }
    close_host(host);
    return resumed;
}

void runtime_cancel_detached(runtime_detached_host *host) {
    TerminateProcess(host->process, 1);
    WaitForSingleObject(host->process, CANCEL_WAIT_MILLISECONDS);
    close_host(host);
}

#else

#define LAST_SIGNAL 64
#define HANDBACK_MILLISECONDS 5000
/** How long a held host waits to be told to run, far past any record write, before it ends unrun. */
#define GO_MILLISECONDS 60000
#define GO 'g'

static void free_list(char **list, size_t count) {
    for (size_t index = 0; list != NULL && index < count; index++) {
        spisor_memory_release(list[index]);
    }
    spisor_memory_release(list);
}

static char **argument_list(const char *executable, const cJSON *arguments) {
    size_t count = text_count(arguments);
    char **list = spisor_memory_allocate_zeroed(count + 2, sizeof *list);
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
    char **list = spisor_memory_allocate_zeroed(count + 1, sizeof *list);
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

/**
 * Whether the caller said go before it closed the pipe, in the host between fork and exec, so only async-signal-safe
 * calls; a caller gone or silent past GO_MILLISECONDS counts as no.
 */
static int is_told_to_go(int go) {
    struct pollfd waiting = {go, POLLIN, 0};
    char said = 0;
    int ready;
    do {
        ready = poll(&waiting, 1, GO_MILLISECONDS);
    } while (ready < 0 && errno == EINTR);
    ssize_t read_now;
    do {
        read_now = ready > 0 ? read(go, &said, 1) : 0;
    } while (read_now < 0 && errno == EINTR);
    return read_now == 1 && said == GO;
}

/** In the started host itself, between fork and exec: only async-signal-safe calls. */
static void become_host(const char *directory, char **arguments, char **environment, int go, int failure) {
    if (!is_told_to_go(go)) {
        _exit(0);
    }
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
static void start_in_new_session(const char *directory, char **arguments, char **environment, int pid_end, int go,
                                 int failure) {
    setsid();
    pid_t host = fork();
    if (host == 0) {
        become_host(directory, arguments, environment, go, failure);
    }
    if (host > 0) {
        write_all(pid_end, &host, sizeof host);
    }
    _exit(host > 0 ? 0 : 1);
}

/** The three pipes a start hands the host: its pid back, go forward, and why its exec failed back. */
typedef struct start_pipes {
    int pid[2];
    int go[2];
    int failure[2];
} start_pipes;

static void close_end(int *end) {
    if (*end >= 0) {
        close(*end);
        *end = -1;
    }
}

static void close_pipes(start_pipes *pipes) {
    for (int end = 0; end < 2; end++) {
        close_end(&pipes->pid[end]);
        close_end(&pipes->go[end]);
        close_end(&pipes->failure[end]);
    }
}

static int open_pipes(start_pipes *pipes) {
    start_pipes closed = {{-1, -1}, {-1, -1}, {-1, -1}};
    *pipes = closed;
    if (!close_on_exec_pipe(pipes->pid) || !close_on_exec_pipe(pipes->go) || !close_on_exec_pipe(pipes->failure)) {
        int error = errno;
        close_pipes(pipes);
        errno = error;
        return 0;
    }
    return 1;
}

/** Reads the host's pid the first child hands back, then reaps that child. */
static int await_pid(pid_t first, int pid_end, double *pid) {
    pid_t host = 0;
    int got_pid = read_all(pid_end, &host, sizeof host) == sizeof host;
    while (waitpid(first, NULL, 0) < 0 && errno == EINTR) {
    }
    if (got_pid) {
        *pid = (double)host;
    }
    return got_pid;
}

static int spawn_detached(const char *directory, char **arguments, char **environment, runtime_detached_host *host,
                          int *error) {
    start_pipes pipes;
    if (!open_pipes(&pipes)) {
        *error = errno;
        return 0;
    }
    pid_t first = fork();
    if (first == 0) {
        close(pipes.pid[0]);
        close(pipes.go[1]);
        close(pipes.failure[0]);
        start_in_new_session(directory, arguments, environment, pipes.pid[1], pipes.go[0], pipes.failure[1]);
    }
    *error = first < 0 ? errno : 0;
    close_end(&pipes.pid[1]);
    close_end(&pipes.go[0]);
    close_end(&pipes.failure[1]);
    int started = first > 0 && await_pid(first, pipes.pid[0], &host->pid);
    if (started) {
        host->go = pipes.go[1];
        host->failure = pipes.failure[0];
        pipes.go[1] = -1;
        pipes.failure[0] = -1;
    }
    close_pipes(&pipes);
    return started;
}

int runtime_start_detached(const runtime_detached_start *start, runtime_detached_host *host, char **problem) {
    *problem = NULL;
    host->pid = 0;
    host->outside_job = 1;
    host->executable = start->executable;
    host->process = NULL;
    host->thread = NULL;
    host->go = -1;
    host->failure = -1;
    char **arguments = argument_list(start->executable, start->arguments);
    char **environment = start->environment == NULL ? NULL : environment_list(start->environment);
    int error = ENOMEM;
    int started = arguments != NULL && (start->environment == NULL || environment != NULL) &&
                  spawn_detached(start->directory, arguments, environment == NULL ? environ : environment, host,
                                 &error);
    if (!started) {
        *problem = problem_of(start->executable, error == 0 ? "it did not start" : strerror(error));
    }
    free_list(arguments, text_count(start->arguments) + 1);
    free_list(environment, text_count(start->environment));
    return started;
}

/**
 * @implNote The failure pipe reaches its end when the host execs; a write end some other process carried away would
 * hold that end back, so a wait past HANDBACK_MILLISECONDS counts as started rather than keeping the holders lock for
 * ever. A failed exec writes at once, well inside it.
 */
int runtime_release_detached(runtime_detached_host *host, char **problem) {
    const char go = GO;
    int error = 0;
    *problem = NULL;
    write_all(host->go, &go, 1);
    close_end(&host->go);
    int failed = read_all(host->failure, &error, sizeof error) == sizeof error;
    close_end(&host->failure);
    if (failed) {
        *problem = problem_of(host->executable, strerror(error));
    }
    return !failed;
}

void runtime_cancel_detached(runtime_detached_host *host) {
    close_end(&host->go);
    close_end(&host->failure);
}

#endif
