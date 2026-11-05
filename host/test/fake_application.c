#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>

#include "host_arguments.h"
#else
#include <time.h>
#include <unistd.h>
#endif

/** The variable naming the folder the fake application reports to and is released through. */
#define SIGNALS_VARIABLE "FAKE_APPLICATION_SIGNALS"
#define LONGEST_LIFE_MILLISECONDS 60000

#if defined(_WIN32)

static long parent_id(void) {
    DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof entry);
    entry.dwSize = sizeof entry;
    long parent = 0;
    for (BOOL more = snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry); more && parent == 0;
         more = Process32NextW(snapshot, &entry)) {
        parent = entry.th32ProcessID == self ? (long)entry.th32ParentProcessID : 0;
    }
    if (snapshot != INVALID_HANDLE_VALUE) {
        CloseHandle(snapshot);
    }
    return parent;
}

static long own_id(void) {
    return (long)GetCurrentProcessId();
}

static void pause_briefly(void) {
    Sleep(50);
}

static char *environment_value(const char *name) {
    wchar_t wide_name[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 64);
    wchar_t value[32768];
    DWORD length = GetEnvironmentVariableW(wide_name, value, 32768);
    if (length == 0 || length >= 32768) {
        return NULL;
    }
    char *narrow = malloc(4 * (size_t)length + 1);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, value, -1, narrow, 4 * (int)length + 1, NULL, NULL);
    }
    return narrow;
}

static FILE *open_for_writing(const char *path) {
    wchar_t wide[4096];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 4096);
    return _wfopen(wide, L"wb");
}

static int exists(const char *path) {
    wchar_t wide[4096];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 4096);
    return GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

#else

static long parent_id(void) {
    return (long)getppid();
}

static long own_id(void) {
    return (long)getpid();
}

static void pause_briefly(void) {
    struct timespec pause = {0, 50 * 1000000L};
    nanosleep(&pause, NULL);
}

static char *environment_value(const char *name) {
    const char *value = getenv(name);
    char *copy = value == NULL ? NULL : malloc(strlen(value) + 1);
    if (copy != NULL) {
        strcpy(copy, value);
    }
    return copy;
}

static FILE *open_for_writing(const char *path) {
    return fopen(path, "wb");
}

static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
    }
    return file != NULL;
}

#endif

static char *signal_file(const char *signals, const char *tag, const char *ending) {
    char *path = malloc(strlen(signals) + strlen(tag) + strlen(ending) + 2);
    if (path != NULL) {
        sprintf(path, "%s/%s%s", signals, tag, ending);
    }
    return path;
}

/**
 * Records {pid, parent, argv} and, on Windows, consoleWindow and the pids attached to its console as
 * consoleProcesses, as <tag>.started, written whole under a temporary name first.
 */
static void record_start(const char *signals, const char *tag, int count, char **arguments) {
    cJSON *started = cJSON_CreateObject();
    cJSON_AddNumberToObject(started, "pid", (double)own_id());
    cJSON_AddNumberToObject(started, "parent", (double)parent_id());
#if defined(_WIN32)
    cJSON_AddBoolToObject(started, "consoleWindow", GetConsoleWindow() != NULL);
    DWORD attached[64];
    DWORD attached_count = GetConsoleProcessList(attached, 64);
    cJSON *console_processes = cJSON_AddArrayToObject(started, "consoleProcesses");
    for (DWORD index = 0; index < attached_count && index < 64; index++) {
        cJSON_AddItemToArray(console_processes, cJSON_CreateNumber((double)attached[index]));
    }
#endif
    cJSON *argv = cJSON_AddArrayToObject(started, "argv");
    for (int index = 1; index < count; index++) {
        cJSON_AddItemToArray(argv, cJSON_CreateString(arguments[index]));
    }
    char *text = cJSON_PrintUnformatted(started);
    char *partial = signal_file(signals, tag, ".partial");
    char *path = signal_file(signals, tag, ".started");
    FILE *file = open_for_writing(partial);
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
        remove(path);
        rename(partial, path);
    }
    free(path);
    free(partial);
    cJSON_free(text);
    cJSON_Delete(started);
}

/**
 * fake-application <tag> <exit code> [arguments...]: records its pid, parent and arguments as <tag>.started
 * in the signals folder, waits for <tag>.release and exits with the code; with no tag it exits at once.
 */
static int run(int count, char **arguments) {
    const char *tag = count > 1 ? arguments[1] : "";
    int code = count > 2 ? atoi(arguments[2]) : 0;
    char *signals = environment_value(SIGNALS_VARIABLE);
    if (tag[0] == 0 || signals == NULL) {
        free(signals);
        return code;
    }
    record_start(signals, tag, count, arguments);
    char *release = signal_file(signals, tag, ".release");
    for (int waited = 0; release != NULL && !exists(release) && waited < LONGEST_LIFE_MILLISECONDS; waited += 50) {
        pause_briefly();
    }
    free(release);
    free(signals);
    return code;
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = host_utf8_arguments(count, wide);
    int code = arguments == NULL ? 2 : run(count, arguments);
    host_free_arguments(arguments, count);
    return code;
}

#else

int main(int count, char **arguments) {
    return run(count, arguments);
}

#endif
