#ifndef RUNTIME_TEST_CHILD_PROCESS_H
#define RUNTIME_TEST_CHILD_PROCESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_times.h"
#include "process_alive.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

/** A process this test started, kept until it is reaped so its pid is never reused meanwhile. */
typedef struct child_process {
    long pid;
#if defined(_WIN32)
    HANDLE handle;
#endif
} child_process;

static inline void pause_milliseconds(long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

#if defined(_WIN32)

/** Starts executable with arguments, none holding a quote, suspended when asked so a job can take it first. */
static inline child_process start_child(const char *executable, const char *const *arguments, size_t count,
                                        int suspended, HANDLE *thread) {
    child_process child = {0, NULL};
    char line[8192];
    size_t used = (size_t)snprintf(line, sizeof line, "\"%s\"", executable);
    for (size_t index = 0; index < count && used < sizeof line; index++) {
        used += (size_t)snprintf(line + used, sizeof line - used, " \"%s\"", arguments[index]);
    }
    wchar_t wide[8192];
    if (MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, 8192) == 0) {
        return child;
    }
    STARTUPINFOW startup;
    PROCESS_INFORMATION information;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    DWORD flags = CREATE_NO_WINDOW | (suspended ? CREATE_SUSPENDED : 0);
    if (!CreateProcessW(NULL, wide, NULL, NULL, FALSE, flags, NULL, NULL, &startup, &information)) {
        fprintf(stderr, "could not start %s: %lu\n", executable, (unsigned long)GetLastError());
        return child;
    }
    if (thread != NULL) {
        *thread = information.hThread;
    } else {
        CloseHandle(information.hThread);
    }
    child.pid = (long)information.dwProcessId;
    child.handle = information.hProcess;
    return child;
}

/** Hard-kills and reaps child, keeping its handle so its pid is not reused before release_child. */
static inline void kill_child(child_process *child) {
    if (child->handle != NULL) {
        TerminateProcess(child->handle, 9);
        WaitForSingleObject(child->handle, 10000);
    }
}

static inline void release_child(child_process *child) {
    if (child->handle != NULL) {
        CloseHandle(child->handle);
        child->handle = NULL;
    }
}

/** Hard-kills a process that is no child of this one, such as a detached background host. */
static inline void kill_pid(long pid) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (process != NULL) {
        TerminateProcess(process, 9);
        WaitForSingleObject(process, 10000);
        CloseHandle(process);
    }
}

#else

static inline child_process start_child(const char *executable, const char *const *arguments, size_t count,
                                        int suspended, void *thread) {
    (void)suspended;
    (void)thread;
    child_process child = {0};
    char *values[64];
    values[0] = (char *)executable;
    for (size_t index = 0; index < count && index < 62; index++) {
        values[index + 1] = (char *)arguments[index];
    }
    values[count + 1 < 63 ? count + 1 : 63] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execv(executable, values);
        _exit(127);
    }
    child.pid = pid > 0 ? (long)pid : 0;
    return child;
}

static inline void kill_child(child_process *child) {
    if (child->pid > 0 && waitpid((pid_t)child->pid, NULL, WNOHANG) == 0) {
        kill((pid_t)child->pid, SIGKILL);
        waitpid((pid_t)child->pid, NULL, 0);
    }
}

static inline void release_child(child_process *child) {
    child->pid = 0;
}

static inline void kill_pid(long pid) {
    if (pid > 0) {
        kill((pid_t)pid, SIGKILL);
    }
}

#endif

/** Polls every 20 ms until condition holds or milliseconds pass; answers whether it held. */
static inline int wait_until(int (*condition)(const void *), const void *context, double milliseconds) {
    double started = runtime_now_milliseconds();
    while (!condition(context)) {
        if (runtime_now_milliseconds() - started > milliseconds) {
            return 0;
        }
        pause_milliseconds(20);
    }
    return 1;
}

static inline int is_gone(const void *pid) {
    return !runtime_process_is_alive(*(const double *)pid);
}

#endif
