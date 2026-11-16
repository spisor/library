#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "runtime/secrets.h"
#include "scratch_directory.h"
#include "secret_backend.h"
#include "secret_linux_hooks.h"
#include "spisor/file.h"
#include "spisor/json_file.h"

static const char *const missing_library = "libspisor-no-such-secret-service.so.0";
static const char *const unreachable_bus = "unix:path=/nonexistent/spisor-test-bus";
static const unsigned hang_guard_seconds = 60;

static void expect_every_call_unavailable(const char *label) {
    char *value = (char *)"untouched";
    runtime_secret_names names = {NULL, 0};
    CHECK(runtime_secret_store_available() == 0);
    CHECK(runtime_secret_store_get("spisor/p/n", &value) == RUNTIME_SECRET_UNAVAILABLE && value == NULL);
    CHECK(runtime_secret_store_set("spisor/p/n", "v") == RUNTIME_SECRET_UNAVAILABLE);
    CHECK(runtime_secret_store_delete("spisor/p/n") == RUNTIME_SECRET_UNAVAILABLE);
    CHECK(runtime_secret_store_list("spisor/p/", &names) == RUNTIME_SECRET_UNAVAILABLE && names.count == 0);
    if (check_failures != 0) {
        fprintf(stderr, "failed in: %s\n", label);
    }
}

static void expect_set_lands_in_the_file(const char *label) {
    const runtime_secrets_definition *secrets = runtime_secrets_definition_of();
    runtime_secrets_set_argument set = SPISOR_INIT(runtime_secrets_set_argument);
    runtime_secrets_set_answer stored = SPISOR_INIT(runtime_secrets_set_answer);
    runtime_secrets_entry_argument entry = SPISOR_INIT(runtime_secrets_entry_argument);
    runtime_secrets_get_answer got = SPISOR_INIT(runtime_secrets_get_answer);
    runtime_secret_places places = SPISOR_INIT(runtime_secret_places);
    char directory[1024];
    char file[1100];
    snprintf(directory, sizeof directory, RUNTIME_SCRATCH_PATH "/secret-linux-%d-%s", (int)getpid(), label);
    start_empty_directory(directory);
    snprintf(file, sizeof file, "%s/state/secrets.json", directory);
    places.secrets_file = file;
    set.places = entry.places = &places;
    set.plugin = entry.plugin = "github";
    set.name = entry.name = "token";
    set.value = "hunter2";
    CHECK(secrets->set(secrets->context, &set, &stored, NULL) == SPISOR_OK);
    if (stored.location != RUNTIME_SECRET_LOCATION_FILE) {
        fprintf(stderr, "%s: expected the secret in the file, got location %u\n", label, (unsigned)stored.location);
        check_failures++;
    }
    spisor_json_file_state state;
    cJSON *entries = spisor_read_json_file(file, &state);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entries, "spisor/github/token")), "hunter2",
               label);
    cJSON_Delete(entries);
    CHECK(secrets->get(secrets->context, &entry, &got, NULL) == SPISOR_OK);
    check_text(got.value, "hunter2", label);
    secrets->release_get_answer(secrets->context, &got);
}

static void an_absent_library_answers_unavailable_and_the_set_lands_in_the_file(void) {
    setenv("DBUS_SESSION_BUS_ADDRESS", unreachable_bus, 1);
    runtime_secret_linux_use_library(missing_library);
    CHECK(!runtime_secret_linux_library_is_loaded());
    expect_every_call_unavailable("absent library");
    expect_set_lands_in_the_file("absent");
}

static void no_session_bus_answers_unavailable_without_calling_the_library(void) {
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    unsetenv("XDG_RUNTIME_DIR");
    runtime_secret_linux_use_library(NULL);
    expect_every_call_unavailable("no session bus");
    expect_set_lands_in_the_file("nobus");
    setenv("DBUS_SESSION_BUS_ADDRESS", "autolaunch:scope=*user", 1);
    expect_every_call_unavailable("autolaunch address");
    expect_set_lands_in_the_file("autolaunch");
}

static int the_real_library_is_installed(void) {
    void *handle = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (handle != NULL) {
        dlclose(handle);
    }
    return handle != NULL;
}

/**
 * @implNote This is the one path through the real library that a runner can exercise: libsecret is
 * called, GIO fails to reach the bus, and the answer must be UNAVAILABLE, promptly. It is skipped, with
 * a loud note and not a failure, when the runner has no libsecret, because then the first test above is
 * the only thing that can be tested. alarm() turns a hang into a killed test.
 */
static void a_real_library_without_a_reachable_bus_answers_unavailable_promptly(void) {
    if (!the_real_library_is_installed()) {
        printf("secret_linux_test: libsecret-1.so.0 is NOT installed here, the unreachable-bus case is skipped\n");
        return;
    }
    printf("secret_linux_test: libsecret-1.so.0 is installed here, exercising the unreachable-bus case\n");
    setenv("DBUS_SESSION_BUS_ADDRESS", unreachable_bus, 1);
    runtime_secret_linux_use_library(NULL);
    CHECK(runtime_secret_linux_library_is_loaded());
    alarm(hang_guard_seconds);
    expect_every_call_unavailable("real library, unreachable bus");
    expect_set_lands_in_the_file("realnobus");
    alarm(0);
}

int main(void) {
    an_absent_library_answers_unavailable_and_the_set_lands_in_the_file();
    no_session_bus_answers_unavailable_without_calling_the_library();
    a_real_library_without_a_reachable_bus_answers_unavailable_promptly();
    return check_summary("secret_linux_test");
}
