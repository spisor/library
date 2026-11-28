#include "host_fixture.h"

#include "host_context.h"
#include "run_result.h"
#include "spisor/allocation.h"

/** What layer_fixture writes as an action's result when it is given a path for one. */
#define LAYER_FIXTURE_RESULT "{\"fixture\":\"result\"}"

static int run_action(host_world *world, program *run, const char *const *arguments) {
    const char *command[16] = {"action"};
    size_t count = 1;
    for (; *arguments != NULL && count < 15; arguments++) {
        command[count++] = *arguments;
    }
    command[count] = NULL;
    return world_run(world, run, command, NULL);
}

static void runs_the_recorded_start_with_the_action_arguments_and_the_input_object(void) {
    host_world world;
    install_launch_world(&world, "action", "arguments", 0);
    program run;
    const char *arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "render", "--input", "name=value", "--input=count=2", NULL};
    CHECK(run_action(&world, &run, arguments) == 0);
    expect_contains(run.out, "action", "action keyword");
    expect_contains(run.out, SESSION_PLUGIN, "plugin");
    expect_contains(run.out, "render", "action name");
    expect_contains(run.out, "{\"name\":\"value\",\"count\":\"2\"}", "input object");
    char *starts = await_starts_holding(&world, "action");
    expect_contains(starts, BACKGROUND_ID " action " SESSION_PLUGIN " render", "the recorded start ran with the arguments");
    spisor_memory_release(starts);
    program_free(&run);
    finish_world(&world);
}

static void sends_an_empty_input_object_when_no_input_is_given(void) {
    host_world world;
    install_launch_world(&world, "action", "empty", 0);
    program run;
    const char *arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "render", NULL};
    CHECK(run_action(&world, &run, arguments) == 0);
    expect_contains(run.out, "{}", "empty input object");
    program_free(&run);
    finish_world(&world);
}

static void keeps_an_equals_sign_inside_an_input_value(void) {
    host_world world;
    install_launch_world(&world, "action", "equals", 0);
    program run;
    const char *arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "render", "--input", "query=a=b", NULL};
    CHECK(run_action(&world, &run, arguments) == 0);
    expect_contains(run.out, "{\"query\":\"a=b\"}", "value after the first equals sign");
    program_free(&run);
    finish_world(&world);
}

static void answers_the_exit_code_of_the_action(void) {
    host_world world;
    install_launch_world(&world, "action", "exit-code", 0);
    program run;
    const char *arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "exit-5", NULL};
    CHECK(run_action(&world, &run, arguments) == 5);
    program_free(&run);
    finish_world(&world);
}

static void is_misused_by_a_repeated_input_name_a_missing_name_or_missing_arguments(void) {
    host_world world;
    install_launch_world(&world, "action", "misuse", 0);
    program repeated;
    const char *repeated_arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "render", "--input", "a=1", "--input", "a=2", NULL};
    CHECK(run_action(&world, &repeated, repeated_arguments) == 2);
    expect_contains(repeated.err, "a", "repeated name");
    program unnamed;
    const char *unnamed_arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "render", "--input", "novalue", NULL};
    CHECK(run_action(&world, &unnamed, unnamed_arguments) == 2);
    program short_line;
    const char *short_arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, NULL};
    CHECK(run_action(&world, &short_line, short_arguments) == 2);
    program_free(&short_line);
    program_free(&unnamed);
    program_free(&repeated);
    finish_world(&world);
}

static void runs_the_session_host_of_a_loader_that_records_no_start(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "action", "session");
    program run;
    const char *arguments[] = {LOADER_ID, SESSION_PLUGIN, "render", NULL};
    CHECK(run_action(&world, &run, arguments) == 0);
    char *starts = await_starts_holding(&world, "action");
    expect_contains(starts, RUNTIME_FIXTURE_BINARY " typescript " LOADER_ID " action " SESSION_PLUGIN " render", "the session host ran the action");
    spisor_memory_release(starts);
    program_free(&run);
    finish_world(&world);
}

static void expect_absent(const char *text, const char *part, const char *label) {
    if (text != NULL && strstr(text, part) != NULL) {
        fprintf(stderr, "%s: expected no \"%s\" in \"%s\"\n", label, part, text);
        check_failures++;
    }
}

static size_t results_left(const host_world *world) {
    char *results = joined(world->base.state, "results");
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(results, &listing) == SPISOR_OK);
    size_t count = listing.count;
    runtime_free_directory_listing(&listing);
    spisor_memory_release(results);
    return count;
}

static void prints_the_result_the_session_host_wrote_and_leaves_no_file(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "action", "result");
    program run;
    const char *arguments[] = {LOADER_ID, SESSION_PLUGIN, "render", NULL};
    CHECK(run_action(&world, &run, arguments) == 0);
    expect_contains(run.out, LAYER_FIXTURE_RESULT, "the action's result printed");
    char *starts = await_starts_holding(&world, "action");
    expect_contains(starts, "results", "the result path named in the start");
    spisor_memory_release(starts);
    CHECK(results_left(&world) == 0);
    program_free(&run);
    finish_world(&world);
}

static void prints_nothing_and_leaves_no_file_when_the_action_fails(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "action", "result-failed");
    program run;
    const char *arguments[] = {LOADER_ID, SESSION_PLUGIN, "exit-5", NULL};
    CHECK(run_action(&world, &run, arguments) == 5);
    expect_absent(run.out, LAYER_FIXTURE_RESULT, "a failed action's result printed");
    CHECK(results_left(&world) == 0);
    program_free(&run);
    finish_world(&world);
}

static void prints_nothing_and_leaves_no_file_when_the_write_is_refused(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "action", "result-refused");
    program run;
    const char *arguments[] = {LOADER_ID, SESSION_PLUGIN, "refuse-write", NULL};
    CHECK(run_action(&world, &run, arguments) == 1);
    expect_absent(run.out, LAYER_FIXTURE_RESULT, "a refused result printed");
    CHECK(results_left(&world) == 0);
    program_free(&run);
    finish_world(&world);
}

/** A draw that answers the values of `repeated_draws` in turn, the last one again once they run out. */
static uint64_t repeated_draws[2];
static int repeated_draw_count;

static int repeated_draw(uint64_t *value) {
    *value = repeated_draws[repeated_draw_count < 2 ? repeated_draw_count : 1];
    repeated_draw_count++;
    return 1;
}

static int failing_draw(uint64_t *value) {
    (void)value;
    return 0;
}

/** The counter part of a result path, `<pid>-<n>-<r>.json`, read back from its name. */
static long counter_of(const char *path) {
    const char *name = strrchr(path, '-');
    const char *start = name;
    while (start > path && start[-1] != '-') {
        start--;
    }
    return strtol(start, NULL, 10);
}

/**
 * Two processes with the same pid, as two containers sharing one state place have, draw different names, and a name
 * something stands at, another process's live result maybe, is never answered and never touched: the next draw names
 * a free one.
 */
static void a_result_name_is_drawn_free_and_what_stands_at_one_is_kept(void) {
    host_world world;
    start_world_at(&world, "action", "result-names");
    int out_of_memory = 0;
    char *first = runtime_fresh_result_path(world.base.state, &out_of_memory);
    char *second = runtime_fresh_result_path(world.base.state, &out_of_memory);
    CHECK(first != NULL && second != NULL && strcmp(first, second) != 0 && !out_of_memory);
    CHECK(first != NULL && strstr(first, "results") != NULL && strlen(strrchr(first, '-')) == 16 + strlen("-.json"));
    long next = first == NULL ? 0 : counter_of(second) + 1;
    char taken[160];
    snprintf(taken, sizeof taken, "results/%.0f-%ld-%016llx.json", host_process_id(), next, 0x1234ULL);
    write_in(world.base.state, taken, "\"another process's\"");
    repeated_draws[0] = 0x1234ULL;
    repeated_draws[1] = 0x5678ULL;
    repeated_draw_count = 0;
    char *drawn = runtime_fresh_result_path_drawn(world.base.state, repeated_draw, &out_of_memory);
    char expected[160];
    snprintf(expected, sizeof expected, "%.0f-%ld-%016llx.json", host_process_id(), next, 0x5678ULL);
    CHECK(drawn != NULL && strstr(drawn, expected) != NULL && repeated_draw_count == 2);
    char *kept = text_in(world.base.state, taken);
    check_text(kept, "\"another process's\"", "the file at the taken name");
    if (drawn != NULL && second != NULL) {
        write_file(drawn, "\"fresh\"");
        write_file(second, "\"of a failed run\"");
    }
    char *fresh = drawn == NULL ? NULL : runtime_take_result(drawn, 0, &out_of_memory);
    char *failed = second == NULL ? NULL : runtime_take_result(second, 1, &out_of_memory);
    check_text(fresh, "\"fresh\"", "the result taken");
    CHECK(failed == NULL && !out_of_memory && results_left(&world) == 1);
    spisor_memory_release(fresh);
    snprintf(taken, sizeof taken, "results/%.0f-%ld-%016llx.json", host_process_id(), next + 1, 0x1234ULL);
    write_in(world.base.state, taken, "\"another process's\"");
    repeated_draws[1] = 0x1234ULL;
    repeated_draw_count = 0;
    char *none = runtime_fresh_result_path_drawn(world.base.state, repeated_draw, &out_of_memory);
    CHECK(none == NULL && !out_of_memory && repeated_draw_count == 8);
    CHECK(runtime_fresh_result_path_drawn(world.base.state, failing_draw, &out_of_memory) == NULL && !out_of_memory);
    spisor_memory_release(kept);
    spisor_memory_release(drawn);
    spisor_memory_release(second);
    spisor_memory_release(first);
    finish_world(&world);
}

static void refuses_a_host_that_is_not_registered(void) {
    host_world world;
    install_launch_world(&world, "action", "unregistered", 0);
    program run;
    const char *arguments[] = {"absent-host", SESSION_PLUGIN, "render", NULL};
    CHECK(run_action(&world, &run, arguments) == 1);
    expect_contains(run.err, "absent-host", "the host named");
    expect_contains(run.err, "fix: ", "with a fix");
    program_free(&run);
    finish_world(&world);
}

static void refuses_a_host_registered_without_a_home(void) {
    host_world world;
    install_launch_world(&world, "action", "homeless", 0);
    cJSON *registration = registration_in(&world, BACKGROUND_ID);
    cJSON_DeleteItemFromObjectCaseSensitive(registration, "home");
    CHECK(runtime_registry_store(world.base.state, registration) == RUNTIME_REGISTRY_OK);
    cJSON_Delete(registration);
    program run;
    const char *arguments[] = {BACKGROUND_ID, SESSION_PLUGIN, "render", NULL};
    CHECK(run_action(&world, &run, arguments) == 1);
    expect_contains(run.err, BACKGROUND_ID " is not registered in ", "the host named as not registered");
    expect_contains(run.err, "run spisor-host install --catalog <path or url> again", "with the reinstall fix");
    program_free(&run);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    a_result_name_is_drawn_free_and_what_stands_at_one_is_kept();
    runs_the_recorded_start_with_the_action_arguments_and_the_input_object();
    sends_an_empty_input_object_when_no_input_is_given();
    keeps_an_equals_sign_inside_an_input_value();
    answers_the_exit_code_of_the_action();
    is_misused_by_a_repeated_input_name_a_missing_name_or_missing_arguments();
    runs_the_session_host_of_a_loader_that_records_no_start();
    prints_the_result_the_session_host_wrote_and_leaves_no_file();
    prints_nothing_and_leaves_no_file_when_the_action_fails();
    prints_nothing_and_leaves_no_file_when_the_write_is_refused();
    refuses_a_host_that_is_not_registered();
    refuses_a_host_registered_without_a_home();
    expect_user_path_unchanged(&guard);
    return check_summary("action_test");
}
