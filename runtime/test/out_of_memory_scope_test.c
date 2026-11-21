#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "allocation_failure.h"
#include "check.h"
#include "scope_fixture.h"
#include "spisor/json_allocation.h"

/**
 * @file
 * Fails every allocation of `weivel.scope`'s open and release and of its config's operations, one at a time and then
 * from one on, and requires each to answer OUT_OF_MEMORY with its answer and refusal empty and nothing left allocated,
 * or to answer as a clean run does.
 */

#define WRONG_RUN ((spisor_status)99)
#define PRINT_CAPACITY 2048

typedef struct operation_case {
    const char *label;
    spisor_status (*run)(void);
    /** Whether the case runs on a scope opened before any allocation is failed. */
    int on_open_scope;
} operation_case;

typedef struct clean_run {
    spisor_status status;
    char printed[PRINT_CAPACITY];
    uint64_t allocations;
} clean_run;

static scope_place place;
static char *refused_manifest;
static weivel_standard_scope opened;
static char printed[PRINT_CAPACITY];

static void put(const char *text) {
    size_t length = strlen(printed);
    snprintf(printed + length, sizeof printed - length, "%s;", text == NULL ? "(none)" : text);
}

static void put_value(const weivel_standard_setting_value *value) {
    char text[64];
    snprintf(text, sizeof text, "%u:%u:%.17g:%zu", (unsigned)value->kind, (unsigned)value->boolean, value->number,
             value->text_length);
    put(text);
    put(value->text);
}

static spisor_status answered(weivel_status status, weivel_standard_service_refusal *refusal) {
    put(refusal->detail);
    scopes_definition()->release_refusal(NULL, refusal);
    return (spisor_status)status;
}

static spisor_status run_open(const char *manifest) {
    const weivel_standard_scopes *scopes = scopes_definition();
    weivel_standard_scope scope;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    spisor_status status;
    start_empty(&scope, sizeof scope);
    status = (spisor_status)scopes->open(scopes->context, manifest, &scope, &refusal);
    put(scope.config == NULL ? "no config" : "config");
    status = empty_unless_answered(status, &scope, sizeof scope);
    scopes->release_scope(scopes->context, &scope);
    return answered((weivel_status)status, &refusal);
}

static spisor_status run_open_viewer(void) {
    return run_open(place.manifest);
}

static spisor_status run_open_refused(void) {
    return run_open(refused_manifest);
}

static cJSON *names;

static spisor_status run_begin(void) {
    runtime_scope_host host = {NULL, NULL, NULL};
    host.home = place.home;
    host.portable = place.portable;
    host.names = names;
    return runtime_scopes_begin(&host);
}

static spisor_status run_all(void) {
    const weivel_standard_config *config = opened.config;
    weivel_standard_settings settings;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    spisor_status status;
    start_empty(&settings, sizeof settings);
    status = (spisor_status)config->all(config->context, &settings, &refusal);
    for (size_t index = 0; index < settings.count; index++) {
        put(settings.settings[index].key);
        put_value(&settings.settings[index].value);
    }
    if (status == SPISOR_OK && settings.count != 6) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &settings, sizeof settings);
    config->release_settings(config->context, &settings);
    return answered((weivel_status)status, &refusal);
}

static spisor_status run_get(void) {
    const weivel_standard_config *config = opened.config;
    weivel_standard_setting_answer answer;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    spisor_status status;
    start_empty(&answer, sizeof answer);
    status = (spisor_status)config->get(config->context, "theme", &answer, &refusal);
    put_value(&answer.value);
    status = empty_unless_answered(status, &answer, sizeof answer);
    config->release_answer(config->context, &answer);
    return answered((weivel_status)status, &refusal);
}

static spisor_status run_set(void) {
    const weivel_standard_config *config = opened.config;
    weivel_standard_setting_value value = {WEIVEL_STANDARD_SETTING_TEXT, 0, 0.0, "blue", 4};
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    return answered(config->set(config->context, "theme", &value, &refusal), &refusal);
}

static void prepare(const operation_case *each) {
    const weivel_standard_scopes *scopes = scopes_definition();
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    printed[0] = '\0';
    start_empty(&opened, sizeof opened);
    if (each->on_open_scope) {
        CHECK(scopes->open(scopes->context, place.manifest, &opened, &refusal) == WEIVEL_STATUS_OK);
        scopes->release_refusal(scopes->context, &refusal);
    }
}

static void finish(void) {
    scopes_definition()->release_scope(NULL, &opened);
}

static void run_clean(const operation_case *each, clean_run *clean) {
    prepare(each);
    spisor_memory_fail_at(0);
    clean->status = each->run();
    clean->allocations = spisor_memory_attempts();
    snprintf(clean->printed, sizeof clean->printed, "%s", printed);
    finish();
}

static allocation_failure_mode failure_mode = FAIL_ONE_ALLOCATION;

/** Fails allocation `attempt`; 1 to go on, 0 after an answer the case does not allow, *ran_whole when none failed. */
static int fail_once(const operation_case *each, const clean_run *clean, uint64_t attempt, int *ran_whole) {
    int64_t baseline;
    spisor_status status;
    int kept;
    prepare(each);
    baseline = spisor_memory_live_blocks();
    fail_allocation(failure_mode, attempt);
    status = each->run();
    *ran_whole = !spisor_memory_failed();
    spisor_memory_fail_at(0);
    kept = spisor_memory_live_blocks() == baseline;
    if (!kept) {
        fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", each->label,
                (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
    } else if ((status != SPISOR_OUT_OF_MEMORY || *ran_whole) &&
               (status != clean->status || strcmp(printed, clean->printed) != 0)) {
        fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"; a clean run answers %d \"%s\"\n",
                each->label, (unsigned long long)attempt, (int)status, printed, (int)clean->status, clean->printed);
        kept = 0;
    }
    finish();
    return kept;
}

static void every_failure_in_mode(const operation_case *each, spisor_status expected) {
    clean_run clean;
    uint64_t attempt = 1;
    int ran_whole = 0;
    run_clean(each, &clean);
    if (clean.status != expected) {
        fprintf(stderr, "%s: a clean run answered %d \"%s\"\n", each->label, (int)clean.status, clean.printed);
        check_failures++;
    }
    for (; check_failures == 0 && !ran_whole; attempt++) {
        if (!fail_once(each, &clean, attempt, &ran_whole)) {
            check_failures++;
        }
    }
    printf("%s%s: %llu allocations, failures 1 to %llu checked\n", each->label,
           allocation_failure_mode_name(failure_mode), (unsigned long long)clean.allocations,
           (unsigned long long)(attempt - 1));
}

int main(void) {
    static const operation_case cases[] = {{"scopes' open and release", run_open_viewer, 0},
                                           {"scopes' open refused", run_open_refused, 0},
                                           {"the host's begin", run_begin, 0},
                                           {"config's all", run_all, 1},
                                           {"config's get", run_get, 1},
                                           {"config's set", run_set, 1}};
    spisor_json_use_library_allocator();
    start_scope_place(&place, "out-of-memory-scope", "fixture");
    names = cJSON_CreateObject();
    CHECK(names != NULL);
    refused_manifest = write_manifest(&place, "layer.json",
                                      "{\"id\": \"viewer\", \"settings\": {\"theme\": {\"layer\": \"aplication\"}}}");
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        spisor_status expected = index == 1 ? SPISOR_REFUSED : SPISOR_OK;
        for (failure_mode = FAIL_ONE_ALLOCATION; failure_mode < ALLOCATION_FAILURE_MODES; failure_mode++) {
            every_failure_in_mode(&cases[index], expected);
        }
    }
    runtime_scopes_end();
    cJSON_Delete(names);
    spisor_memory_release(refused_manifest);
    scope_place_free(&place);
    return check_summary("out_of_memory_scope_test");
}
