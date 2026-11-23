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
 * Fails every allocation of `weivel.scope`'s open and release and of its config's, secrets', paths' and log's
 * operations, one at a time and then from one on, and requires each to answer OUT_OF_MEMORY with its answer and
 * refusal empty and nothing left allocated, or to answer as a clean run does.
 */

#define WRONG_RUN ((spisor_status)99)
#define PRINT_CAPACITY 2048

typedef struct operation_case {
    const char *label;
    spisor_status (*run)(void);
    /** Whether the case runs on a scope opened before any allocation is failed. */
    int on_open_scope;
    /** What a clean run answers. */
    spisor_status expected;
    /** What brings back what the run handed elsewhere, run once failures are off and before blocks are counted. */
    void (*settle)(void);
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
    runtime_scope_host host = scope_host_of(&place);
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

static spisor_status run_get_tree(void) {
    const weivel_standard_config *config = opened.config;
    weivel_standard_value_tree tree;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    spisor_status status;
    char shape[32];
    start_empty(&tree, sizeof tree);
    status = (spisor_status)config->get_tree(config->context, "cache", &tree, &refusal);
    snprintf(shape, sizeof shape, "%u:%zu", (unsigned)tree.root.kind, tree.root.count);
    put(shape);
    if (status == SPISOR_OK && (tree.root.kind != WEIVEL_STANDARD_VALUE_RECORD || tree.root.count != 3)) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &tree, sizeof tree);
    config->release_tree(config->context, &tree);
    return answered((weivel_status)status, &refusal);
}

static spisor_status run_set(void) {
    const weivel_standard_config *config = opened.config;
    weivel_standard_setting_value value = {WEIVEL_STANDARD_SETTING_TEXT, 0, 0.0, "blue", 4};
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    return answered(config->set(config->context, "theme", &value, &refusal), &refusal);
}

static spisor_status run_secret_get(void) {
    const weivel_standard_secrets *secrets = opened.secrets;
    weivel_standard_secret secret;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    spisor_status status;
    start_empty(&secret, sizeof secret);
    status = (spisor_status)secrets->get(secrets->context, "api", &secret, &refusal);
    if (status == SPISOR_OK && (secret.length != 5 || memcmp(secret.bytes, "value", 5) != 0)) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &secret, sizeof secret);
    secrets->release_secret(secrets->context, &secret);
    return answered((weivel_status)status, &refusal);
}

static spisor_status run_secret_set(void) {
    const weivel_standard_secrets *secrets = opened.secrets;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    return answered(secrets->set(secrets->context, "api", (const uint8_t *)"value", 5, &refusal), &refusal);
}

static spisor_status run_secret_remove(void) {
    const weivel_standard_secrets *secrets = opened.secrets;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    return answered(secrets->remove(secrets->context, "gone", &refusal), &refusal);
}

static spisor_status run_paths(void) {
    const weivel_standard_paths *paths = opened.paths;
    const char *cache = NULL;
    const char *config = NULL;
    const char *plugin = NULL;
    weivel_status status = paths->cache(paths->context, &cache);
    if (status == WEIVEL_STATUS_OK) {
        status = paths->config(paths->context, &config);
    }
    if (status == WEIVEL_STATUS_OK) {
        status = paths->plugin(paths->context, &plugin);
    }
    put(cache);
    put(config);
    put(plugin);
    return (spisor_status)status;
}

static spisor_status run_log_write(void) {
    const weivel_standard_log *log = opened.log;
    weivel_standard_entry entry = {"count", 5, {WEIVEL_STANDARD_VALUE_NUMBER, 0, 2.0, NULL, 0, NULL, NULL, 0}};
    weivel_standard_value fields = {WEIVEL_STANDARD_VALUE_RECORD, 0, 0.0, NULL, 0, NULL, &entry, 1};
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    return answered(log->write(log->context, WEIVEL_STANDARD_LOG_INFO, "written", &fields, &refusal), &refusal);
}

/** Waits for the log's writer to write what the run handed it, by ending the host and beginning it again. */
static void settle_log(void) {
    runtime_scope_host host = scope_host_of(&place);
    host.names = names;
    runtime_scopes_end();
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
}

static void settle(const operation_case *each) {
    if (each->settle != NULL) {
        each->settle();
    }
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
    settle(each);
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
    settle(each);
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

static void every_failure_in_mode(const operation_case *each) {
    clean_run clean;
    uint64_t attempt = 1;
    int ran_whole = 0;
    run_clean(each, &clean);
    if (clean.status != each->expected) {
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

/** A begin that runs out of memory after an end must not leave the writer running with no host started. */
static void a_failed_begin_after_an_end_leaves_the_log_refusing(void) {
    const weivel_standard_scopes *scopes = scopes_definition();
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    runtime_scope_host host = scope_host_of(&place);
    CHECK(scopes->open(scopes->context, place.manifest, &scope, NULL) == WEIVEL_STATUS_OK);
    runtime_scopes_end();
    spisor_memory_fail_from(1);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OUT_OF_MEMORY);
    spisor_memory_fail_at(0);
    if (scope.log != NULL) {
        CHECK(scope.log->write(scope.log->context, WEIVEL_STANDARD_LOG_INFO, "after", NULL, &refusal) ==
              WEIVEL_STATUS_REFUSED);
        CHECK(refusal.detail != NULL && strstr(refusal.detail, "no host is started") != NULL);
        scope.log->release_refusal(scope.log->context, &refusal);
    }
    scopes->release_scope(scopes->context, &scope);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
}

int main(void) {
    static const operation_case cases[] = {
        {"scopes' open and release", run_open_viewer, 0, SPISOR_OK, NULL},
        {"scopes' open refused", run_open_refused, 0, SPISOR_REFUSED, NULL},
        {"the host's begin", run_begin, 0, SPISOR_OK, NULL},
        {"config's all", run_all, 1, SPISOR_OK, NULL},
        {"config's get", run_get, 1, SPISOR_OK, NULL},
        {"config's get_tree", run_get_tree, 1, SPISOR_OK, NULL},
        {"config's set", run_set, 1, SPISOR_OK, NULL},
        {"secrets' set", run_secret_set, 1, SPISOR_OK, NULL},
        {"secrets' get", run_secret_get, 1, SPISOR_OK, NULL},
        {"secrets' remove", run_secret_remove, 1, SPISOR_OK, NULL},
        {"paths' directories", run_paths, 1, SPISOR_OK, NULL},
        {"log's write", run_log_write, 1, SPISOR_OK, settle_log}};
    spisor_json_use_library_allocator();
    start_scope_place(&place, "out-of-memory-scope", "fixture");
    names = cJSON_CreateObject();
    CHECK(names != NULL);
    refused_manifest = write_manifest(&place, "layer.json",
                                      "{\"id\": \"viewer\", \"settings\": {\"theme\": {\"layer\": \"aplication\"}}}");
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        for (failure_mode = FAIL_ONE_ALLOCATION; failure_mode < ALLOCATION_FAILURE_MODES; failure_mode++) {
            every_failure_in_mode(&cases[index]);
        }
    }
    a_failed_begin_after_an_end_leaves_the_log_refusing();
    runtime_scopes_end();
    cJSON_Delete(names);
    spisor_memory_release(refused_manifest);
    scope_place_free(&place);
    return check_summary("out_of_memory_scope_test");
}
