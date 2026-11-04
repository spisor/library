#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "file_times.h"
#include "processes.h"
#include "service_call.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <time.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

/** No fixture outlives a broken test by more than this. */
#define LONGEST_LIFE_MILLISECONDS 60000.0

static void pause_milliseconds(long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

static int prune_says_exit(const char *state, const char *host_id, double interval) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", state);
    cJSON_AddStringToObject(argument, "hostId", host_id);
    cJSON_AddNumberToObject(argument, "self", (double)current_process_id());
    cJSON_AddNumberToObject(argument, "checkMilliseconds", interval);
    int32_t status = 0;
    cJSON *answer = runtime_call_service(runtime_processes_prune, argument, &status);
    cJSON_Delete(argument);
    int exit_now = status != 0 || !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer, "exit"));
    cJSON_Delete(answer);
    return exit_now;
}

/** background <state> <host-id> <interval>: what a background host's language layer loops on. */
static int run_background(int count, char **values) {
    if (count != 5) {
        return 2;
    }
    double interval = atof(values[4]);
    double started = runtime_now_milliseconds();
    while (runtime_now_milliseconds() - started < LONGEST_LIFE_MILLISECONDS) {
        if (prune_says_exit(values[2], values[3], interval)) {
            return 0;
        }
        pause_milliseconds((long)interval);
    }
    return 3;
}

static cJSON *start_of(int count, char **values) {
    cJSON *start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "executable", values[5]);
    cJSON *arguments = cJSON_AddArrayToObject(start, "arguments");
    for (int index = 6; index < count; index++) {
        cJSON_AddItemToArray(arguments, cJSON_CreateString(values[index]));
    }
    return start;
}

/** hold <state> <host-id> <answer-file> <executable> [arguments...]: a launcher that holds and never releases. */
static int run_hold(int count, char **values) {
    if (count < 6) {
        return 2;
    }
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", values[2]);
    cJSON_AddStringToObject(argument, "hostId", values[3]);
    cJSON_AddNumberToObject(argument, "holder", (double)current_process_id());
    cJSON_AddItemToObject(argument, "start", start_of(count, values));
    int32_t status = 0;
    cJSON *answer = runtime_call_service(runtime_processes_hold, argument, &status);
    cJSON_Delete(argument);
    char *text = answer == NULL ? NULL : cJSON_PrintUnformatted(answer);
    cJSON_Delete(answer);
    int written = text != NULL && spisor_replace_file(values[4], (const uint8_t *)text, strlen(text));
    cJSON_free(text);
    if (status != 0 || !written) {
        return 4;
    }
    pause_milliseconds((long)LONGEST_LIFE_MILLISECONDS);
    return 0;
}

static char *working_directory(void) {
    static char directory[4096];
#if defined(_WIN32)
    wchar_t wide[2048];
    DWORD length = GetCurrentDirectoryW(2048, wide);
    if (length == 0 || length >= 2048 ||
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, directory, (int)sizeof directory, NULL, NULL) == 0) {
        return NULL;
    }
#else
    if (getcwd(directory, sizeof directory) == NULL) {
        return NULL;
    }
#endif
    return directory;
}

/** environment <answer-file> <name>: writes where it runs, the variable named, and whether PATH reached it. */
static int run_environment(int count, char **values) {
    if (count != 4) {
        return 2;
    }
    const char *value = getenv(values[3]);
    cJSON *answer = cJSON_CreateObject();
    cJSON_AddStringToObject(answer, "directory", working_directory() == NULL ? "" : working_directory());
    cJSON_AddItemToObject(answer, "value", value == NULL ? cJSON_CreateNull() : cJSON_CreateString(value));
    cJSON_AddBoolToObject(answer, "path", getenv("PATH") != NULL);
    char *text = cJSON_PrintUnformatted(answer);
    cJSON_Delete(answer);
    int written = text != NULL && spisor_replace_file(values[2], (const uint8_t *)text, strlen(text));
    cJSON_free(text);
    return written ? 0 : 4;
}

int main(int count, char **values) {
    if (count >= 2 && strcmp(values[1], "environment") == 0) {
        return run_environment(count, values);
    }
    if (count >= 2 && strcmp(values[1], "background") == 0) {
        return run_background(count, values);
    }
    if (count >= 2 && strcmp(values[1], "hold") == 0) {
        return run_hold(count, values);
    }
    if (count >= 2 && strcmp(values[1], "sleep") == 0) {
        pause_milliseconds((long)LONGEST_LIFE_MILLISECONDS);
        return 0;
    }
    fprintf(stderr, "usage: background_fixture background|hold|environment|sleep ...\n");
    return 2;
}
