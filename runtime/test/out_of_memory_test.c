#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "allocation.h"
#include "check.h"
#include "definition_fixture.h"
#include "runtime/config.h"
#include "runtime/log.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"
#include "scratch_directory.h"
#include "spisor/file.h"

/**
 * @file
 * Fails each allocation of one operation per service in turn, cJSON's among them, and requires every failure to answer
 * OUT_OF_MEMORY and to leave no block of the binary's allocator live.
 */

#if defined(_WIN32)
#define LOCAL_APP_DATA {"LOCALAPPDATA", "C:\\L"}
#else
#define LOCAL_APP_DATA {"UNUSED", ""}
#endif

#define START "2026-10-27T19:00:00.000Z"
#define TIME "2026-10-27T19:34:53.123Z"

typedef struct scratch {
    char base[1024];
    char home[1100];
    char portable[1100];
    char secrets_file[1200];
    char config_directory[1200];
} scratch;

static scratch place;
static spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree config = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree value = SPISOR_INIT(spisor_value_tree);

static void write_text(const char *directory, const char *name, const char *text) {
    char path[1400];
    CHECK(spisor_make_directories(directory) == 1);
    snprintf(path, sizeof path, "%s/%s", directory, name);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

/** The files every operation reads, written afresh before each attempt so no attempt sees another's writes. */
static void fresh_files(void) {
    start_empty_directory(place.base);
    write_text(place.config_directory, "p.json", "{\"a\":{\"b\":1},\"logging\":true}");
    write_text(place.config_directory, "settings.json", "{\"logConsole\":true,\"ui\":{\"size\":2}}");
    write_text(place.portable, "settings.json", "{\"ui\":{\"theme\":\"dark\"}}");
}

typedef spisor_status (*operation)(void);

static spisor_status resolve_paths(void) {
    const runtime_paths_definition *paths = runtime_paths_definition_of();
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.home = place.home;
    argument.names = &names.root;
    status = paths->resolve(paths->context, &argument, &answer, &refusal);
    paths->release_answer(paths->context, &answer);
    paths->release_refusal(paths->context, &refusal);
    return status;
}

static spisor_status resolve_places(void) {
    const runtime_places_definition *places = runtime_places_definition_of();
    runtime_variable environment[] = {LOCAL_APP_DATA, {"SPISOR_HOME", ""}};
    runtime_places_resolve_argument argument = SPISOR_INIT(runtime_places_resolve_argument);
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.home = place.home;
    argument.environment = environment;
    argument.environment_count = 2;
    status = places->resolve(places->context, &argument, &answer, &refusal);
    places->release_answer(places->context, &answer);
    places->release_refusal(places->context, &refusal);
    return status;
}

static spisor_status set_secret(void) {
    const runtime_secrets_definition *secrets = runtime_secrets_definition_of();
    runtime_secret_places places = SPISOR_INIT(runtime_secret_places);
    runtime_secrets_set_argument argument = SPISOR_INIT(runtime_secrets_set_argument);
    runtime_secrets_set_answer answer = SPISOR_INIT(runtime_secrets_set_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    places.secrets_file = place.secrets_file;
    places.force_file_store = 1;
    argument.places = &places;
    argument.plugin = "p";
    argument.name = "token";
    argument.value = "v1";
    status = secrets->set(secrets->context, &argument, &answer, &refusal);
    secrets->release_refusal(secrets->context, &refusal);
    return status;
}

static spisor_status set_config(void) {
    const runtime_config_definition *definition = runtime_config_definition_of();
    runtime_config_plugin plugin = SPISOR_INIT(runtime_config_plugin);
    runtime_settings_change change = SPISOR_INIT(runtime_settings_change);
    runtime_config_set_answer answer = SPISOR_INIT(runtime_config_set_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    plugin.home = place.home;
    plugin.portable = place.portable;
    plugin.plugin = "p";
    plugin.config = &config.root;
    change.key = "a.c";
    change.value = &value.root;
    status = definition->set(definition->context, &plugin, &change, &answer, &refusal);
    definition->release_set_answer(definition->context, &answer);
    definition->release_refusal(definition->context, &refusal);
    return status;
}

static spisor_status all_settings(void) {
    const runtime_settings_definition *settings = runtime_settings_definition_of();
    runtime_settings_scope scope = SPISOR_INIT(runtime_settings_scope);
    spisor_value_tree values = SPISOR_INIT(spisor_value_tree);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    scope.home = place.home;
    scope.portable = place.portable;
    status = settings->all(settings->context, &scope, &values, &refusal);
    settings->release_tree(settings->context, &values);
    settings->release_refusal(settings->context, &refusal);
    return status;
}

static spisor_status write_log(void) {
    const runtime_log_definition *log = runtime_log_definition_of();
    runtime_log_entry entry = SPISOR_INIT(runtime_log_entry);
    runtime_log_answer answer = SPISOR_INIT(runtime_log_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    entry.home = place.home;
    entry.portable = place.portable;
    entry.plugin = "p";
    entry.config = &config.root;
    entry.start = START;
    entry.time = TIME;
    entry.level = RUNTIME_LOG_INFO;
    entry.message = "m";
    status = log->write(log->context, &entry, &answer, &refusal);
    log->release_answer(log->context, &answer);
    log->release_refusal(log->context, &refusal);
    return status;
}

/** Fails the operation's allocations one at a time until it runs whole, checking each failure as the file says. */
static void every_failure_answers_out_of_memory(const char *label, operation run) {
    for (uint64_t attempt = 1; attempt < 100000; attempt++) {
        fresh_files();
        int64_t baseline = spisor_memory_live_blocks();
        spisor_memory_fail_at(attempt);
        spisor_status status = run();
        int failed = spisor_memory_failed();
        spisor_memory_fail_at(0);
        if (spisor_memory_live_blocks() != baseline) {
            fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", label, (unsigned long long)attempt,
                    (long long)(spisor_memory_live_blocks() - baseline));
            check_failures++;
            return;
        }
        if (!failed) {
            if (status != SPISOR_OK) {
                fprintf(stderr, "%s: with no failure it answered %d\n", label, (int)status);
                check_failures++;
            }
            printf("%s: %llu allocations, each failure answered out of memory\n", label,
                   (unsigned long long)(attempt - 1));
            return;
        }
        if (status != SPISOR_OUT_OF_MEMORY) {
            fprintf(stderr, "%s: failing allocation %llu answered %d\n", label, (unsigned long long)attempt,
                    (int)status);
            check_failures++;
            return;
        }
    }
    fprintf(stderr, "%s: never ran whole\n", label);
    check_failures++;
}

int main(void) {
    runtime_paths_definition_of();
    snprintf(place.base, sizeof place.base, RUNTIME_SCRATCH_PATH "/out-of-memory-%d", (int)CURRENT_PROCESS_ID);
    snprintf(place.home, sizeof place.home, "%s/home", place.base);
    snprintf(place.portable, sizeof place.portable, "%s/portable", place.base);
    snprintf(place.secrets_file, sizeof place.secrets_file, "%s/state/secrets.json", place.base);
    snprintf(place.config_directory, sizeof place.config_directory, "%s/config", place.home);
    CHECK(value_of_text("{\"cache\":\"c\"}", &names));
    CHECK(value_of_text("{\"defaults\":{\"a\":{\"d\":[1,\"two\"]}}}", &config));
    CHECK(value_of_text("{\"x\":[true,null,3]}", &value));
    every_failure_answers_out_of_memory("paths' resolve", resolve_paths);
    every_failure_answers_out_of_memory("places' resolve", resolve_places);
    every_failure_answers_out_of_memory("secrets' set", set_secret);
    every_failure_answers_out_of_memory("config's set", set_config);
    every_failure_answers_out_of_memory("settings' all", all_settings);
    every_failure_answers_out_of_memory("log's write", write_log);
    spisor_value_release(&names);
    spisor_value_release(&config);
    spisor_value_release(&value);
    CHECK(runtime_remove_tree(place.base) == 1);
    return check_summary("out_of_memory_test");
}
