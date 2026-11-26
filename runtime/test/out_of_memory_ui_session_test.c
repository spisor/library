#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "allocation_failure.h"
#include "catalog_call.h"
#include "check.h"
#include "runtime/places.h"
#include "runtime/ui.h"
#include "runtime_testing_fixture.h"
#include "spisor/json_allocation.h"

#define PRINT_CAPACITY 16384
#define ALPHA                                                                                                      \
    "{\"id\": \"alpha\", \"version\": \"1.0.0\", \"entry\": \"index.js\", \"config\": {\"defaults\": {\"depth\": 2}}," \
    " \"settings\": {\"depth\": {\"type\": \"number\", \"layer\": \"application\"}, \"token\": {\"type\": \"secret\"}}}"
#define BETA "{\"id\": \"beta\", \"version\": \"2.0.0\", \"entry\": \"index.js\"}"

/** A text an answer is printed into without allocating, so printing never moves the allocation count. */
typedef struct printed_answer {
    char text[PRINT_CAPACITY];
    size_t length;
} printed_answer;

/** What one session call answered: its status, its refusal's detail and every field it filled. */
typedef struct session_run {
    spisor_status status;
    char refusal[1200];
    printed_answer printed;
} session_run;

static const runtime_ui_definition *ui;
static runtime_places_answer places;
static printed_answer printed;

static void put(const char *text) {
    for (size_t index = 0; text != NULL && text[index] != '\0' && printed.length + 1 < PRINT_CAPACITY; index++) {
        printed.text[printed.length++] = text[index];
    }
    printed.text[printed.length] = '\0';
}

static void put_number(double number) {
    char text[40];
    snprintf(text, sizeof text, "%.17g;", number);
    put(text);
}

static void put_text(const char *text) {
    put(text == NULL ? "(none)" : text);
    put(";");
}

static void put_value(const spisor_value *value) {
    if (value == NULL) {
        put("(none);");
        return;
    }
    put_number((double)value->kind);
    put(value->kind == SPISOR_VALUE_TEXT ? value->text : "");
    put("[");
    for (size_t index = 0; value->kind == SPISOR_VALUE_LIST && index < value->count; index++) {
        put_value(&value->items[index]);
    }
    for (size_t index = 0; value->kind == SPISOR_VALUE_RECORD && index < value->count; index++) {
        put_text(value->entries[index].key);
        put_value(&value->entries[index].value);
    }
    put("]");
}

static void put_config(const runtime_config_plugin *config) {
    put_text(config->home);
    put_text(config->portable);
    put_text(config->plugin);
    put_value(config->config);
    for (size_t index = 0; index < config->setting_count; index++) {
        put_text(config->settings[index].key);
        put_text(config->settings[index].type);
        put_number(config->settings[index].layer);
    }
    put_text(config->places == NULL ? NULL : config->places->secrets_file);
}

static void put_session(const runtime_ui_session_answer *answer) {
    put_text(answer->host);
    for (size_t index = 0; index < answer->plugin_count; index++) {
        put_text(answer->plugins[index].id);
        put_text(answer->plugins[index].version);
        put_value(answer->plugins[index].description);
        put_config(answer->plugins[index].config);
    }
    put_value(answer->descriptions);
    put_value(answer->ui);
    put_value(answer->installed);
    put_value(answer->detected);
    put_value(answer->available);
    for (size_t index = 0; index < answer->source_count; index++) {
        put_number(answer->sources[index].kind);
        put_text(answer->sources[index].location);
    }
}

static spisor_status run_session(const char *loader, char *refusal_detail, size_t detail_size) {
    runtime_ui_session_argument argument = SPISOR_INIT(runtime_ui_session_argument);
    runtime_ui_session_answer answer;
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    start_empty(&answer, sizeof answer);
    argument.places = &places;
    argument.loader = loader;
    status = ui->session(NULL, &argument, &answer, &refusal);
    put_session(&answer);
    status = empty_unless_answered(status, &answer, sizeof answer);
    snprintf(refusal_detail, detail_size, "%s", refusal.detail == NULL ? "" : refusal.detail);
    ui->release_session_answer(NULL, &answer);
    ui->release_refusal(NULL, &refusal);
    return status;
}

static void run_once(const char *loader, session_run *run) {
    printed.length = 0;
    printed.text[0] = '\0';
    run->status = run_session(loader, run->refusal, sizeof run->refusal);
    run->printed = printed;
}

/** Whether a run under failure answered OUT_OF_MEMORY, or else exactly what the clean run answered. */
static int matches_clean(const session_run *clean, const session_run *failed, const char *label, uint64_t attempt) {
    if (failed->status == SPISOR_OUT_OF_MEMORY) {
        return 1;
    }
    if (failed->status == clean->status && strcmp(failed->refusal, clean->refusal) == 0 &&
        failed->printed.length == clean->printed.length && strcmp(failed->printed.text, clean->printed.text) == 0) {
        return 1;
    }
    fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"; a clean run answers %d \"%s\"\n", label,
            (unsigned long long)attempt, (int)failed->status, failed->refusal, (int)clean->status, clean->refusal);
    return 0;
}

/** Fails the session's allocations one at a time in every mode until a run allocates without failing. */
static void every_failure_of(const char *label, const char *loader, spisor_status expected) {
    static session_run clean;
    static session_run failed;
    for (int mode = FAIL_ONE_ALLOCATION; mode < ALLOCATION_FAILURE_MODES; mode++) {
        uint64_t attempt = 1;
        int ran_whole = 0;
        uint64_t allocations;
        spisor_memory_fail_at(0);
        run_once(loader, &clean);
        allocations = spisor_memory_attempts();
        if (clean.status != expected) {
            fprintf(stderr, "%s: a clean run answered %d \"%s\"\n", label, (int)clean.status, clean.refusal);
            check_failures++;
        }
        for (; check_failures == 0 && !ran_whole; attempt++) {
            int64_t baseline = spisor_memory_live_blocks();
            fail_allocation((allocation_failure_mode)mode, attempt);
            run_once(loader, &failed);
            ran_whole = !spisor_memory_failed();
            spisor_memory_fail_at(0);
            if (spisor_memory_live_blocks() != baseline) {
                fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", label,
                        (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
                check_failures++;
            } else if (!matches_clean(&clean, &failed, label, attempt)) {
                check_failures++;
            }
        }
        printf("%s%s: %llu allocations, failures 1 to %llu checked\n", label,
               allocation_failure_mode_name((allocation_failure_mode)mode), (unsigned long long)allocations,
               (unsigned long long)(attempt - 1));
    }
}

/** Lays out the home of two plugins and resolves its places, allocating without failure. */
static laid_out lay_out_two_plugins(spisor_value_tree manifests[2], runtime_testing_install installs[2]) {
    runtime_boot_library libraries[2];
    runtime_testing_home_argument argument;
    runtime_places_resolve_argument resolve = SPISOR_INIT(runtime_places_resolve_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    const runtime_places_definition *service = (const runtime_places_definition *)runtime_places_definition_of();
    laid_out home;
    manifests[0] = tree_of(parsed(ALPHA));
    manifests[1] = tree_of(parsed(BETA));
    for (size_t index = 0; index < 2; index++) {
        installs[index].manifest = &manifests[index].root;
        installs[index].entry_source = entry_source;
        installs[index].settings = NULL;
    }
    default_libraries(libraries);
    argument = home_argument(base, installs, 2, libraries);
    home = lay_out_argument(&argument);
    CHECK(home.status == SPISOR_OK);
    places.size = (uint32_t)sizeof places;
    resolve.environment = home.answer.variables;
    resolve.environment_count = home.answer.variable_count;
    CHECK(service->resolve(NULL, &resolve, &places, &refusal) == SPISOR_OK);
    service->release_refusal(NULL, &refusal);
    return home;
}

static void break_manifest(const laid_out *home) {
    char *manifest = joined(home->answer.installs[1].bundle, "plugin.json");
    size_t length = 0;
    uint8_t *content = spisor_read_file_reporting(manifest, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    CHECK(content != NULL && length > 0);
    if (content != NULL && length > 0) {
        content[0] = 'x';
        CHECK(spisor_replace_file(manifest, content, length) == SPISOR_OK);
    }
    spisor_memory_release(content);
    spisor_memory_release(manifest);
}

int main(void) {
    spisor_value_tree manifests[2];
    runtime_testing_install installs[2];
    laid_out home;
    spisor_json_use_library_allocator();
    ui = (const runtime_ui_definition *)runtime_ui_definition_of();
    start_parent("session");
    home = lay_out_two_plugins(manifests, installs);
    if (check_failures == 0) {
        every_failure_of("ui's session", HOST, SPISOR_OK);
        break_manifest(&home);
        every_failure_of("ui's session refused", HOST, SPISOR_REFUSED);
    }
    ((const runtime_places_definition *)runtime_places_definition_of())->release_answer(NULL, &places);
    testing()->release_home_answer(NULL, &home.answer);
    testing()->release_refusal(NULL, &home.refusal);
    spisor_value_release(&manifests[0]);
    spisor_value_release(&manifests[1]);
    finish_parent();
    return check_summary("out_of_memory_ui_session_test");
}
