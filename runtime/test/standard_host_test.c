#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_fixture.h"

#include "holders.h"
#include "spisor/json_allocation.h"

/**
 * @file
 * Calls `weivel.host`'s start as the host program does, with this process's environment pointed at a registered
 * loader, and requires the plan each argument form names, ui's action merge reaching an ACTION plan, every refusal,
 * and a stop that leaves no run record and no scope behind.
 */

typedef struct started_run {
    weivel_status status;
    weivel_standard_host_plan plan;
    weivel_standard_service_refusal refusal;
} started_run;

static started_run start_with(const char *const *arguments, size_t count) {
    const weivel_standard_host *host = host_definition();
    started_run run;
    memset(&run, 0, sizeof run);
    run.plan.size = sizeof run.plan;
    run.refusal.size = sizeof run.refusal;
    run.status = host->start(host->context, arguments, count, &run.plan, &run.refusal);
    return run;
}

static void release_run(started_run *run) {
    const weivel_standard_host *host = host_definition();
    host->release_plan(host->context, &run->plan);
    host->release_refusal(host->context, &run->refusal);
}

static void stop_run(void) {
    const weivel_standard_host *host = host_definition();
    uint32_t exit_code = 9;
    CHECK(host->stop(host->context, &exit_code, NULL) == WEIVEL_STATUS_OK);
    CHECK(exit_code == 0);
}

static void expect_refused(const started_run *run, weivel_status status, const char *detail_part, const char *label) {
    int matched = run->status == status && run->refusal.detail != NULL && strstr(run->refusal.detail, detail_part);
    if (!matched) {
        fprintf(stderr, "%s: expected %d naming \"%s\", got %d \"%s\"\n", label, (int)status, detail_part,
                (int)run->status, run->refusal.detail == NULL ? "(none)" : run->refusal.detail);
        check_failures++;
    }
    CHECK(run->plan.storage == NULL && run->plan.mode == 0 && run->plan.plugin_count == 0);
}

static void check_ends_with(const char *text, const char *suffix, const char *label) {
    size_t length = text == NULL ? 0 : strlen(text);
    int ends = text != NULL && length >= strlen(suffix) && strcmp(text + length - strlen(suffix), suffix) == 0;
    if (!ends) {
        fprintf(stderr, "%s: expected a path ending %s, got %s\n", label, suffix, text == NULL ? "(none)" : text);
        check_failures++;
    }
}

static weivel_status open_scope(const char *manifest, weivel_standard_scope *scope) {
    const weivel_standard_scopes *scopes = runtime_standard_scope_definition_of();
    memset(scope, 0, sizeof *scope);
    scope->size = sizeof *scope;
    return scopes->open(scopes->context, manifest, scope, NULL);
}

static void release_scope(weivel_standard_scope *scope) {
    const weivel_standard_scopes *scopes = runtime_standard_scope_definition_of();
    scopes->release_scope(scopes->context, scope);
}

static void a_serve_start_plans_the_hinted_libraries_and_the_installed_plugin(void) {
    const char *arguments[1] = {LOADER};
    started_run run;
    start_host_world("serve");
    run = start_with(arguments, 1);
    CHECK(run.status == WEIVEL_STATUS_OK);
    CHECK(run.plan.mode == WEIVEL_STANDARD_HOST_SERVE);
    CHECK(run.plan.library_count == 1);
    if (run.plan.library_count == 1) {
        check_ends_with(run.plan.libraries[0].binary, "/forebay/forebay.dll", "the library's binary");
        char *declaration = joined(forebay_directory, "library.json");
        check_text(run.plan.libraries[0].declaration, declaration, "the library's declaration");
        spisor_memory_release(declaration);
    }
    CHECK(run.plan.plugin_count == 1);
    if (run.plan.plugin_count == 1) {
        check_ends_with(run.plan.plugins[0].manifest, "plugin.json", "the plugin's manifest");
        check_ends_with(run.plan.plugins[0].entry, "index.js", "the plugin's entry");
        CHECK(strstr(run.plan.plugins[0].directory, "1.0.0") != NULL);
    }
    CHECK(run.plan.action_plugin == NULL && run.plan.action_name == NULL);
    CHECK(run.plan.action_arguments.kind == WEIVEL_STANDARD_VALUE_NONE);
    CHECK(run_record_count() == 1);
    release_run(&run);
    stop_run();
    CHECK(run_record_count() == 0);
    finish_base();
}

static void the_scopes_open_from_the_start_until_its_stop(void) {
    const char *arguments[1] = {LOADER};
    weivel_standard_scope scope;
    started_run run;
    start_host_world("scopes");
    run = start_with(arguments, 1);
    CHECK(run.status == WEIVEL_STATUS_OK && run.plan.plugin_count == 1);
    if (run.plan.plugin_count == 1) {
        CHECK(open_scope(run.plan.plugins[0].manifest, &scope) == WEIVEL_STATUS_OK);
        release_scope(&scope);
    }
    if (run.plan.plugin_count == 1) {
        stop_run();
        CHECK(open_scope(run.plan.plugins[0].manifest, &scope) == WEIVEL_STATUS_REFUSED);
    }
    release_run(&run);
    finish_base();
}

static const weivel_standard_entry *entry_at(const weivel_standard_value *record, size_t index, const char *key) {
    const weivel_standard_entry *entry = index < record->count ? &record->entries[index] : NULL;
    if (entry == NULL || strcmp(entry->key, key) != 0) {
        fprintf(stderr, "the merged arguments hold no %s at %zu\n", key, index);
        check_failures++;
        return NULL;
    }
    return entry;
}

static void an_action_start_names_the_plugins_own_manifest_and_merges_its_arguments(void) {
    const char *arguments[5] = {LOADER, "action", "a", "greet", "{\"name\": \"you\", \"times\": 2}"};
    const weivel_standard_entry *entry;
    started_run run;
    start_host_world("action");
    run = start_with(arguments, 5);
    CHECK(run.status == WEIVEL_STATUS_OK);
    CHECK(run.plan.mode == WEIVEL_STANDARD_HOST_ACTION);
    CHECK(run.plan.plugin_count == 1);
    if (run.plan.plugin_count == 1) {
        check_text(run.plan.action_plugin, run.plan.plugins[0].manifest, "the action's plugin is its manifest");
    }
    check_text(run.plan.action_name, "greet", "the action's name");
    CHECK(run.plan.action_arguments.kind == WEIVEL_STANDARD_VALUE_RECORD && run.plan.action_arguments.count == 3);
    entry = entry_at(&run.plan.action_arguments, 0, "name");
    CHECK(entry == NULL || (entry->value.kind == WEIVEL_STANDARD_VALUE_TEXT && strcmp(entry->value.text, "you") == 0));
    entry = entry_at(&run.plan.action_arguments, 1, "loud");
    CHECK(entry == NULL || (entry->value.kind == WEIVEL_STANDARD_VALUE_BOOLEAN && entry->value.boolean == 0));
    entry = entry_at(&run.plan.action_arguments, 2, "times");
    CHECK(entry == NULL || (entry->value.kind == WEIVEL_STANDARD_VALUE_NUMBER && entry->value.number == 2.0));
    release_run(&run);
    stop_run();
    CHECK(run_record_count() == 0);
    finish_base();
}

static void arguments_that_name_no_run_are_refused(void) {
    const char *serve_word[2] = {LOADER, "serve"};
    const char *short_action[4] = {LOADER, "action", "a", "greet"};
    const char *listed_input[5] = {LOADER, "action", "a", "greet", "[1]"};
    const char *broken_input[5] = {LOADER, "action", "a", "greet", "{"};
    started_run run;
    start_host_world("usage");
    run = start_with(NULL, 0);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "takes a loader id", "no arguments");
    release_run(&run);
    run = start_with(serve_word, 2);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "takes a loader id", "a second word");
    release_run(&run);
    run = start_with(short_action, 4);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "takes a loader id", "an action without its input");
    release_run(&run);
    run = start_with(listed_input, 5);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "takes a loader id", "an input that is a list");
    release_run(&run);
    run = start_with(broken_input, 5);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "takes a loader id", "an input that is no JSON");
    release_run(&run);
    CHECK(run_record_count() == 0);
    finish_base();
}

static void a_null_argument_is_malformed(void) {
    const char *holding_null[2] = {LOADER, NULL};
    started_run run = start_with(holding_null, 2);
    expect_refused(&run, WEIVEL_STATUS_MALFORMED, "NULL", "an argument that is NULL");
    release_run(&run);
    run = start_with(NULL, 1);
    expect_refused(&run, WEIVEL_STATUS_MALFORMED, "NULL", "arguments that are NULL beside a count");
    release_run(&run);
}

static void an_action_of_a_plugin_the_plan_does_not_run_is_refused(void) {
    const char *arguments[5] = {LOADER, "action", "b", "greet", "{}"};
    weivel_standard_scope scope;
    char *manifest;
    started_run run;
    start_host_world("not-deployed");
    install_plugin("b", "{\"id\": \"b\", \"version\": \"1.0.0\", \"actions\": {\"greet\": {\"call\": "
                        "{\"service\": \"b\", \"method\": \"greet\"}}}}");
    run = start_with(arguments, 5);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "b is not among the plugins " LOADER " runs", "an undeployed plugin");
    release_run(&run);
    CHECK(run_record_count() == 0);
    manifest = joined(cache, "bundles/a/1.0.0/plugin.json");
    CHECK(open_scope(manifest, &scope) == WEIVEL_STATUS_REFUSED);
    spisor_memory_release(manifest);
    finish_base();
}

static void an_undeclared_action_is_refused(void) {
    const char *arguments[5] = {LOADER, "action", "a", "wave", "{}"};
    started_run run;
    start_host_world("undeclared");
    run = start_with(arguments, 5);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "declares no action wave", "an undeclared action");
    release_run(&run);
    CHECK(run_record_count() == 0);
    finish_base();
}

static void a_layer_with_no_hinted_version_is_refused_and_published(void) {
    const char *arguments[1] = {LOADER};
    char *problem = NULL;
    char *ready;
    started_run run;
    start_host_world("no-layer");
    write_hint("javascript-host", "{\"libraries\": {}}");
    run = start_with(arguments, 1);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "no javascript-host version is recorded", "a layer with no version");
    release_run(&run);
    ready = runtime_holders_path(state, LOADER, ".ready.json", &problem);
    CHECK(ready != NULL && spisor_probe_file(ready) == SPISOR_FILE_PRESENT);
    spisor_memory_release(ready);
    spisor_memory_release(problem);
    finish_base();
}

static void an_unregistered_loader_is_refused(void) {
    const char *arguments[1] = {"other-host"};
    started_run run;
    start_host_world("unregistered");
    run = start_with(arguments, 1);
    expect_refused(&run, WEIVEL_STATUS_REFUSED, "other-host", "an unregistered loader");
    release_run(&run);
    finish_base();
}

static void a_second_start_before_its_stop_is_refused(void) {
    const char *arguments[1] = {LOADER};
    started_run first;
    started_run second;
    start_host_world("second");
    first = start_with(arguments, 1);
    CHECK(first.status == WEIVEL_STATUS_OK);
    second = start_with(arguments, 1);
    expect_refused(&second, WEIVEL_STATUS_REFUSED, "called again before its stop", "a second start");
    release_run(&second);
    release_run(&first);
    stop_run();
    finish_base();
}

static void every_later_operation_refuses_with_no_run(void) {
    const weivel_standard_host *host = host_definition();
    weivel_standard_value nothing = {WEIVEL_STANDARD_VALUE_NONE, 0, 0.0, NULL, 0, NULL, NULL, 0};
    weivel_standard_host_next next = {sizeof next, 0, 0, NULL};
    uint32_t exit_code = 9;
    CHECK(host->started(host->context, NULL, 0, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(host->tick(host->context, 0, &next, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(host->finished(host->context, &nothing, NULL, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(host->stop(host->context, &exit_code, NULL) == WEIVEL_STATUS_REFUSED && exit_code == 9);
    CHECK(host->finished(host->context, &nothing, "threw", NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(host->finished(host->context, NULL, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(host->stop(host->context, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
}

int main(void) {
    spisor_json_use_library_allocator();
    a_serve_start_plans_the_hinted_libraries_and_the_installed_plugin();
    the_scopes_open_from_the_start_until_its_stop();
    an_action_start_names_the_plugins_own_manifest_and_merges_its_arguments();
    arguments_that_name_no_run_are_refused();
    a_null_argument_is_malformed();
    an_action_of_a_plugin_the_plan_does_not_run_is_refused();
    an_undeclared_action_is_refused();
    a_layer_with_no_hinted_version_is_refused_and_published();
    an_unregistered_loader_is_refused();
    a_second_start_before_its_stop_is_refused();
    every_later_operation_refuses_with_no_run();
    return check_summary("standard_host_test");
}
