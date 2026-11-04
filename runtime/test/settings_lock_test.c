#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#include "catalog_call.h"
#include "check.h"
#include "config.h"
#include "config_hooks.h"
#include "directory_tree.h"
#include "paths.h"
#include "scratch_directory.h"
#include "spisor/file.h"
#include "spisor/path.h"

/** One settings write a thread makes: key set to true in the portable layer of home. */
typedef struct settings_write {
    const char *home;
    const char *portable;
    const char *key;
    int32_t status;
} settings_write;

static char *joined(const char *base, const char *rest) {
    const char *segments[2] = {base, rest};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static void pause_milliseconds(long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

static void write_setting(settings_write *write) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "home", write->home);
    cJSON_AddStringToObject(argument, "portable", write->portable);
    cJSON_AddStringToObject(argument, "key", write->key);
    cJSON_AddTrueToObject(argument, "value");
    cJSON_AddStringToObject(argument, "layer", "portable");
    catalog_answer answer = call_catalog(runtime_settings_set, argument);
    write->status = answer.status;
    cJSON_Delete(answer.tree);
}

#if defined(_WIN32)
typedef HANDLE writer_thread;

static DWORD WINAPI run_write(LPVOID write) {
    write_setting(write);
    return 0;
}

static writer_thread start_writer(settings_write *write) {
    return CreateThread(NULL, 0, run_write, write, 0, NULL);
}

static void join_writer(writer_thread thread) {
    WaitForSingleObject(thread, 30000);
    CloseHandle(thread);
}
#else
typedef pthread_t writer_thread;

static void *run_write(void *write) {
    write_setting(write);
    return NULL;
}

static writer_thread start_writer(settings_write *write) {
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_write, write) == 0);
    return thread;
}

static void join_writer(writer_thread thread) {
    pthread_join(thread, NULL);
}
#endif

static int wait_for(const char *path) {
    for (int waited = 0; waited < 500; waited++) {
        if (spisor_file_exists(path)) {
            return 1;
        }
        pause_milliseconds(20);
    }
    return 0;
}

/**
 * A writer that read the layer and has not written yet holds the lock, so a second writer of another key
 * waits for it and reads what it wrote; without the lock the first writer's stale read would drop the
 * second key every time.
 */
static void keeps_both_keys_of_two_writers_one_paused_between_its_read_and_its_write(void) {
    char root[1024];
    snprintf(root, sizeof root, "%s/settings-lock-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(root);
    char *home = joined(root, "home");
    char *portable = joined(root, "portable");
    char *signals = joined(root, "signals");
    char *paused = joined(signals, "paused");
    char *release = joined(signals, "release");
    CHECK(spisor_make_directories(home) == 1 && spisor_make_directories(signals) == 1);
    runtime_config_pause_after_read(signals);
    settings_write first = {home, portable, "plugins.first.enabled", -1};
    settings_write second = {home, portable, "plugins.second.enabled", -1};
    writer_thread first_thread = start_writer(&first);
    CHECK(wait_for(paused));
    writer_thread second_thread = start_writer(&second);
    pause_milliseconds(500);
    CHECK(spisor_replace_file(release, (const uint8_t *)"", 0) == 1);
    join_writer(first_thread);
    join_writer(second_thread);
    CHECK(first.status == SPISOR_STATUS_SUCCESS && second.status == SPISOR_STATUS_SUCCESS);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "home", home);
    cJSON_AddStringToObject(argument, "portable", portable);
    catalog_answer answer = call_catalog(runtime_settings_all, argument);
    const cJSON *plugins = value_at(answer.tree, "values/plugins");
    CHECK(cJSON_IsTrue(value_at(plugins, "first/enabled")));
    CHECK(cJSON_IsTrue(value_at(plugins, "second/enabled")));
    cJSON_Delete(answer.tree);
    runtime_config_pause_after_read(NULL);
    free(release);
    free(paused);
    free(signals);
    free(portable);
    free(home);
    CHECK(runtime_remove_tree(root) == 1);
}

int main(void) {
    keeps_both_keys_of_two_writers_one_paused_between_its_read_and_its_write();
    return check_summary("settings_lock_test");
}
