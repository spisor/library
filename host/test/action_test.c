#include "action_result.h"
#include "host_fixture.h"
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
    expect_contains(starts, RUNTIME_FIXTURE_BINARY " javascript " LOADER_ID " action " SESSION_PLUGIN " render", "the session host ran the action");
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

/**
 * The OS reuses a process id, so an earlier spisor-host killed before it removed its result left a file at the name
 * this process takes; it is gone before the run starts, and only the fresh result is taken.
 *
 * @implNote It runs first, in this test's own process, since the name counts from this process's first result.
 */
static void clears_what_an_earlier_process_left_at_its_result_name(void) {
    host_world world;
    start_world_at(&world, "action", "result-stale");
    host_context context;
    memset(&context, 0, sizeof context);
    context.state = world.base.state;
    char rest[96];
    snprintf(rest, sizeof rest, "results/%.0f-1.json", host_process_id());
    write_in(world.base.state, rest, "\"stale\"");
    int removable = 0;
    char *path = host_result_path(&context, &removable);
    CHECK(path != NULL && removable);
    CHECK(path == NULL || spisor_probe_file(path) == SPISOR_FILE_MISSING);
    if (path != NULL) {
        write_file(path, "\"fresh\"");
    }
    int out_of_memory = 0;
    char *text = path == NULL ? NULL : host_take_result(path, 0, &out_of_memory);
    check_text(text, "\"fresh\"", "the fresh result taken");
    CHECK(!out_of_memory && results_left(&world) == 0);
    spisor_memory_release(text);
    spisor_memory_release(path);
    path = host_result_path(&context, &removable);
    if (path != NULL) {
        write_file(path, "\"of a failed run\"");
    }
    text = path == NULL ? NULL : host_take_result(path, 1, &out_of_memory);
    CHECK(text == NULL && results_left(&world) == 0);
    spisor_memory_release(text);
    spisor_memory_release(path);
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
    clears_what_an_earlier_process_left_at_its_result_name();
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
