#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "child_start_fixture.h"

#include "process_run.h"

/**
 * @file
 * processes.run_action as the interface calls it: the child each declared action runs in, started by C and never by
 * the program, answering the result its run wrote, leaving no file behind, and on Windows opening no console window
 * from a caller that has no console.
 */

typedef struct run_answer {
    spisor_status status;
    runtime_processes_run_action_answer answer;
    spisor_refusal refusal;
} run_answer;

static run_answer run(const runtime_processes_run_action_argument *argument) {
    runtime_processes_run_action_answer answer = SPISOR_INIT(runtime_processes_run_action_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    run_answer ran;
    ran.answer = answer;
    ran.refusal = refusal;
    ran.status = processes()->run_action(NULL, argument, &ran.answer, &ran.refusal);
    return ran;
}

static void release_run(run_answer *ran) {
    processes()->release_run_action_answer(NULL, &ran->answer);
    processes()->release_refusal(NULL, &ran->refusal);
}

static cJSON *report(void) {
    char *text = text_in(scratch, "report.json");
    cJSON *parsed_report = text == NULL ? NULL : cJSON_Parse(text);
    spisor_memory_release(text);
    return parsed_report;
}

static void greet_answers_hello_and_leaves_no_result_file(void) {
    start_child_world("greet");
    runtime_processes_run_action_argument argument = action_argument("greet");
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 0 && ran.answer.failure == NULL);
    CHECK(ran.answer.result != NULL && ran.answer.result->kind == SPISOR_VALUE_TEXT);
    check_text(ran.answer.result == NULL ? NULL : ran.answer.result->text, "hello", "greet's result");
    CHECK(results_left() == 0);
    cJSON *reported = report();
    check_text(text_at(reported, "mark"), "the caller's environment", "the child's environment");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(reported, "arguments");
    const char *result = cJSON_GetStringValue(cJSON_GetArrayItem(arguments, 7));
    CHECK(cJSON_GetArraySize(arguments) == 8 && result != NULL && strstr(result, "results") != NULL);
    cJSON_Delete(reported);
    release_run(&ran);
    finish_base();
}

static void a_failing_action_answers_its_exit_code_and_what_it_wrote_to_standard_error(void) {
    start_child_world("fail");
    runtime_processes_run_action_argument argument = action_argument("fail");
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 3 && ran.answer.result == NULL);
    check_text(ran.answer.failure, "the fixture's action failed", "the failure, trimmed");
    CHECK(results_left() == 0);
    release_run(&ran);
    argument = action_argument("fail-silently");
    ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 4);
    check_text(ran.answer.failure, "the action's run exited with code 4", "a silent failure");
    release_run(&ran);
    finish_base();
}

/**
 * A run that writes more standard error than is kept answers its end, where its error is, as text: the warning before it
 * dropped, the euro sign the cut split skipped rather than half kept, and an invalid byte and a NUL each U+FFFD.
 */
static void a_long_standard_error_answers_its_end_as_valid_text(void) {
    start_child_world("noisy");
    runtime_processes_run_action_argument argument = action_argument("noisy");
    run_answer ran = run(&argument);
    const char *failure = ran.answer.failure;
    const char *error = "invalid \xEF\xBF\xBD here, nul \xEF\xBF\xBD here, the action's own error";
    size_t length = failure == NULL ? 0 : strlen(failure);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 5 && failure != NULL);
    CHECK(failure != NULL && failure[0] == 'y' && strstr(failure, "ww") == NULL);
    CHECK(length >= strlen(error) && strcmp(failure + length - strlen(error), error) == 0);
    CHECK(length == 4096 - 2 - (strlen(error) - 4) + strlen(error));
    release_run(&ran);
    finish_base();
}

#if defined(_WIN32)
/** A crash's NTSTATUS stays the unsigned code Windows reports, and its sentence names it in hex as well. */
static void a_crashed_run_answers_its_exit_code_unsigned(void) {
    start_child_world("crash");
    runtime_processes_run_action_argument argument = action_argument("crash");
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 3221225477LL);
    check_text(ran.answer.failure, "the action's run exited with code 3221225477 (0xC0000005)", "a crash's failure");
    release_run(&ran);
    finish_base();
}
#endif

/** A run handed a variable whose value is no text starts nothing, on every OS, rather than dropping it. */
static void an_environment_value_that_is_no_text_is_refused(void) {
    start_child_world("not-text");
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, "SPISOR_FIXTURE_REPORT", report_path);
    cJSON_AddNumberToObject(environment, "NUMBERED", 1);
    int64_t exit_code = 0;
    char *errors = NULL;
    char *problem = NULL;
    CHECK(!runtime_run_reporting_errors(RUNTIME_ACTION_FIXTURE_PATH, NULL, environment, &exit_code, &errors, &problem));
    CHECK(problem != NULL && strstr(problem, "no text") != NULL && errors == NULL);
    CHECK(!exists_in(scratch, "report.json"));
    spisor_memory_release(problem);
    cJSON_Delete(environment);
    finish_base();
}

static char *copy_of(const char *text) {
    char *copy = spisor_memory_allocate(strlen(text) + 1);
    if (copy != NULL) {
        strcpy(copy, text);
    }
    return copy;
}

static void set_variable(const char *name, const char *value) {
#if defined(_WIN32)
    CHECK(_putenv_s(name, value == NULL ? "" : value) == 0);
#else
    CHECK(value == NULL ? unsetenv(name) == 0 : setenv(name, value, 1) == 0);
#endif
}

/** A NULL environment with a count of 0 is this process's own, as it is wherever runtime takes the user's. */
static void no_environment_runs_the_action_in_this_processs_own(void) {
    start_child_world("own-environment");
    size_t count = 0;
    const runtime_variable *variables = environment_of(&count);
    char *saved[8] = {NULL};
    for (size_t index = 0; index < count && index < 8; index++) {
        const char *before = getenv(variables[index].name);
        saved[index] = before == NULL ? NULL : copy_of(before);
        set_variable(variables[index].name, variables[index].value);
    }
    set_variable("SPISOR_FIXTURE_MARK", "this process's own environment");
    runtime_processes_run_action_argument argument = action_argument("greet");
    argument.environment = NULL;
    argument.environment_count = 0;
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 0);
    cJSON *reported = report();
    check_text(text_at(reported, "mark"), "this process's own environment", "the child's environment");
    cJSON_Delete(reported);
    release_run(&ran);
    for (size_t index = 0; index < count && index < 8; index++) {
        set_variable(variables[index].name, saved[index]);
        spisor_memory_release(saved[index]);
    }
    finish_base();
}

static void an_action_that_writes_no_result_answers_none(void) {
    start_child_world("nothing");
    runtime_processes_run_action_argument argument = action_argument("nothing");
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 0 && ran.answer.result == NULL &&
          ran.answer.failure == NULL);
    release_run(&ran);
    finish_base();
}

static void a_result_that_is_no_json_is_refused_and_removed(void) {
    start_child_world("garbage");
    runtime_processes_run_action_argument argument = action_argument("garbage");
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_REFUSED && ran.answer.storage == NULL);
    CHECK(ran.refusal.detail != NULL && strstr(ran.refusal.detail, "could not be read as JSON") != NULL);
    CHECK(results_left() == 0);
    release_run(&ran);
    finish_base();
}

static void a_start_layer_start_refuses_is_refused_with_its_reason_and_nothing_written(void) {
    start_child_world("unregistered");
    runtime_processes_run_action_argument argument = action_argument("greet");
    argument.loader_id = "no-such-loader";
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_REFUSED && ran.refusal.detail != NULL &&
          strstr(ran.refusal.detail, "no-such-loader") != NULL);
    CHECK(results_left() == 0 && !exists_in(scratch, "report.json"));
    release_run(&ran);
    finish_base();
}

static void places_the_environment_does_not_resolve_to_are_refused(void) {
    start_child_world("other-places");
    runtime_processes_run_action_argument argument = action_argument("greet");
    argument.state = cache;
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_REFUSED && !exists_in(scratch, "report.json"));
    release_run(&ran);
    finish_base();
}

static void an_argument_out_of_shape_is_malformed(void) {
    start_child_world("malformed");
    runtime_processes_run_action_argument argument = action_argument("greet");
    argument.size = (uint32_t)offsetof(runtime_processes_run_action_argument, input);
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_MALFORMED);
    release_run(&ran);
    argument = action_argument("greet");
    argument.state = "relative/state";
    ran = run(&argument);
    CHECK(ran.status == SPISOR_MALFORMED);
    release_run(&ran);
    argument = action_argument("greet");
    argument.loader_id = "no/loader";
    ran = run(&argument);
    CHECK(ran.status == SPISOR_MALFORMED);
    release_run(&ran);
    argument = action_argument("");
    ran = run(&argument);
    CHECK(ran.status == SPISOR_MALFORMED && !exists_in(scratch, "report.json"));
    release_run(&ran);
    finish_base();
}

#if defined(_WIN32)
/**
 * A window start leaves the interface with no console, and a console program it starts would get a console window of
 * its own unless C starts it without one, so the child must find no console window.
 *
 * @implNote Runs last: FreeConsole leaves this process without a console for the rest of its life.
 */
static void a_child_of_a_caller_with_no_console_opens_no_console_window(void) {
    start_child_world("no-console");
    FreeConsole();
    CHECK(GetConsoleProcessList((DWORD[1]){0}, 1) == 0);
    runtime_processes_run_action_argument argument = action_argument("greet");
    run_answer ran = run(&argument);
    CHECK(ran.status == SPISOR_OK && ran.answer.exit_code == 0);
    cJSON *reported = report();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reported, "consoleWindow")));
    cJSON_Delete(reported);
    release_run(&ran);
    finish_base();
}
#endif

int main(void) {
    greet_answers_hello_and_leaves_no_result_file();
    a_failing_action_answers_its_exit_code_and_what_it_wrote_to_standard_error();
    an_action_that_writes_no_result_answers_none();
    a_long_standard_error_answers_its_end_as_valid_text();
#if defined(_WIN32)
    a_crashed_run_answers_its_exit_code_unsigned();
#endif
    an_environment_value_that_is_no_text_is_refused();
    no_environment_runs_the_action_in_this_processs_own();
    a_result_that_is_no_json_is_refused_and_removed();
    a_start_layer_start_refuses_is_refused_with_its_reason_and_nothing_written();
    places_the_environment_does_not_resolve_to_are_refused();
    an_argument_out_of_shape_is_malformed();
#if defined(_WIN32)
    a_child_of_a_caller_with_no_console_opens_no_console_window();
#endif
    return check_summary("child_start_test");
}
