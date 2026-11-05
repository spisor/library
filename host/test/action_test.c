#include "host_fixture.h"

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
    free(starts);
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

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    runs_the_recorded_start_with_the_action_arguments_and_the_input_object();
    sends_an_empty_input_object_when_no_input_is_given();
    keeps_an_equals_sign_inside_an_input_value();
    answers_the_exit_code_of_the_action();
    is_misused_by_a_repeated_input_name_a_missing_name_or_missing_arguments();
    refuses_a_host_that_is_not_registered();
    expect_user_path_unchanged(&guard);
    return check_summary("action_test");
}
