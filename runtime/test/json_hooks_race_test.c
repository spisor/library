#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "allocation.h"
#include "check.h"
#include "cJSON.h"
#include "json_allocation.h"
#include "runtime/changes.h"
#include "runtime/commands.h"
#include "runtime/config.h"
#include "runtime/detect.h"
#include "runtime/fetch.h"
#include "runtime/identity.h"
#include "runtime/log.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/registry.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"

/**
 * @file
 * Takes every definition on one thread while another allocates and frees cJSON trees, and requires the allocator's
 * live blocks to come back to where they started: taking a definition must never touch cJSON's hooks, since resetting
 * them mid-flight pairs an allocation with the other allocator's free.
 */

#define ROUNDS 300000

static void take_every_definition(void) {
    for (int round = 0; round < ROUNDS; round++) {
        runtime_paths_definition_of();
        runtime_places_definition_of();
        runtime_secrets_definition_of();
        runtime_config_definition_of();
        runtime_settings_definition_of();
        runtime_log_definition_of();
        runtime_identity_definition_of();
        runtime_fetch_definition_of();
        runtime_detect_definition_of();
        runtime_registry_definition_of();
        runtime_changes_definition_of();
        runtime_commands_definition_of();
    }
}

#if defined(_WIN32)
static DWORD WINAPI run_taker(LPVOID ignored) {
    (void)ignored;
    take_every_definition();
    return 0;
}
#else
static void *run_taker(void *ignored) {
    (void)ignored;
    take_every_definition();
    return NULL;
}
#endif

int main(void) {
    spisor_json_use_library_allocator();
    int64_t baseline = spisor_memory_live_blocks();
#if defined(_WIN32)
    HANDLE taker = CreateThread(NULL, 0, run_taker, NULL, 0, NULL);
    CHECK(taker != NULL);
#else
    pthread_t taker;
    CHECK(pthread_create(&taker, NULL, run_taker, NULL) == 0);
#endif
    for (int round = 0; round < ROUNDS; round++) {
        cJSON_Delete(cJSON_CreateObject());
    }
#if defined(_WIN32)
    WaitForSingleObject(taker, INFINITE);
    CloseHandle(taker);
#else
    pthread_join(taker, NULL);
#endif
    if (spisor_memory_live_blocks() != baseline) {
        fprintf(stderr, "live blocks drifted by %lld\n", (long long)(spisor_memory_live_blocks() - baseline));
        check_failures++;
    }
    return check_summary("json_hooks_race_test");
}
