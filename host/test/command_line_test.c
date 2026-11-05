#include "install_fixture.h"

#include "application_choices.h"
#include "host_options.h"
#include "terminal_question.h"
#include "windows_command.h"

static char scratch[1024];

static char *scratch_file(const char *name, const char *content) {
    char *path = joined(scratch, name);
    if (content != NULL) {
        write_file(path, content);
    }
    return path;
}

static void writes_the_question_to_the_terminal_and_answers_the_first_line_typed(void) {
    char *input = scratch_file("in", "2\nignored\n");
    char *output = scratch_file("out", "");
    host_terminal terminal = {input, output};
    char *answer = host_ask_on_terminal("Choose: ", &terminal);
    check_text(answer, "2", "first line");
    char *written = text_in(output, "");
    check_text(written, "Choose: ", "question written");
    free(written);
    free(answer);
    free(output);
    free(input);
}

static void answers_none_when_the_terminal_ends_before_a_line_is_typed(void) {
    char *input = scratch_file("empty-in", "");
    char *output = scratch_file("empty-out", "");
    host_terminal terminal = {input, output};
    char *answer = host_ask_on_terminal("Choose: ", &terminal);
    CHECK(answer == NULL);
    free(answer);
    free(output);
    free(input);
}

static void answers_none_when_there_is_no_terminal_to_open(void) {
    char *input = scratch_file("no/such/in", NULL);
    char *output = scratch_file("no/such/out", NULL);
    host_terminal terminal = {input, output};
    char *answer = host_ask_on_terminal("Choose: ", &terminal);
    CHECK(answer == NULL);
    free(output);
    free(input);
}

static void names_the_controlling_terminal_of_this_operating_system(void) {
    host_terminal terminal = host_controlling_terminal();
#if defined(_WIN32)
    check_text(terminal.input, "\\\\.\\CONIN$", "windows input");
    check_text(terminal.output, "\\\\.\\CONOUT$", "windows output");
#else
    check_text(terminal.input, "/dev/tty", "posix input");
    check_text(terminal.output, "/dev/tty", "posix output");
#endif
}

typedef struct scripted_answers {
    const char *const *answers;
    int asked;
    char questions[4096];
} scripted_answers;

static char *scripted(const char *question, void *context) {
    scripted_answers *script = context;
    strncat(script->questions, question, sizeof script->questions - strlen(script->questions) - 1);
    const char *answer = script->answers[script->asked];
    if (answer == NULL) {
        return NULL;
    }
    script->asked++;
    char *copy = malloc(strlen(answer) + 1);
    strcpy(copy, answer);
    return copy;
}

static cJSON *choose(const char *missing_json, const char *const *answers, scripted_answers *script) {
    memset(script, 0, sizeof *script);
    script->answers = answers;
    cJSON *missing = parsed(missing_json);
    cJSON *choices = host_choose_methods(missing, scripted, script);
    cJSON_Delete(missing);
    return choices;
}

static void numbers_the_offered_methods_and_skip_and_installs_the_one_chosen_by_number(void) {
    const char *answers[] = {"1", NULL};
    scripted_answers script;
    cJSON *choices = choose("[{\"application\":\"claude\",\"methods\":[\"fake\",\"npm\"]}]", answers, &script);
    expect_json(choices, "{\"claude\":\"fake\"}", "chosen by number");
    check_text(script.questions,
               "claude is not installed and was not found. Install it through:\n  1) fake\n  2) npm\n  3) skip\nChoose 1-3: ",
               "numbered question");
    cJSON_Delete(choices);
}

static void skips_for_skip_typed_by_name_or_number_or_an_empty_answer(void) {
    const char *by_name[] = {"Skip", NULL};
    const char *by_number[] = {"2", NULL};
    const char *empty[] = {"", NULL};
    const char *const *cases[] = {by_name, by_number, empty};
    for (size_t index = 0; index < 3; index++) {
        scripted_answers script;
        cJSON *choices = choose("[{\"application\":\"claude\",\"methods\":[\"fake\"]}]", cases[index], &script);
        expect_json(choices, "{\"claude\":\"skip\"}", "skipped");
        cJSON_Delete(choices);
    }
}

static void picks_a_method_typed_by_name_and_skips_after_three_answers_that_name_none(void) {
    const char *by_name[] = {"fake", NULL};
    scripted_answers script;
    cJSON *named = choose("[{\"application\":\"claude\",\"methods\":[\"fake\"]}]", by_name, &script);
    expect_json(named, "{\"claude\":\"fake\"}", "by name");
    cJSON_Delete(named);
    const char *wrong[] = {"9", "brew", "x", "fake", NULL};
    cJSON *gave_up = choose("[{\"application\":\"claude\",\"methods\":[\"fake\"]}]", wrong, &script);
    expect_json(gave_up, "{\"claude\":\"skip\"}", "three wrong answers");
    CHECK(script.asked == 3);
    cJSON_Delete(gave_up);
}

static void asks_about_no_further_application_once_the_terminal_answers_nothing(void) {
    const char *none[] = {NULL};
    scripted_answers script;
    cJSON *choices = choose("[{\"application\":\"claude\",\"methods\":[\"fake\"]},{\"application\":\"opencode\",\"methods\":[\"fake\"]}]",
                            none, &script);
    expect_json(choices, "{}", "nothing chosen");
    CHECK(strstr(script.questions, "opencode") == NULL);
    cJSON_Delete(choices);
}

static void runs_a_batch_file_through_the_command_shell_with_each_argument_escaped_for_both_parses(void) {
    cJSON *arguments = parsed("[\"a b\",\"x&y\"]");
    char *line = host_command_shell_line("C:\\bin\\app.cmd", arguments);
    check_text(line, "\"C:\\bin\\app.cmd ^^^\"a^^^ b^^^\" ^^^\"x^^^&y^^^\"\"", "shell line");
    free(line);
    cJSON_Delete(arguments);
    CHECK(host_needs_command_shell("C:\\bin\\app.cmd") && host_needs_command_shell("C:\\bin\\app.BAT") &&
          host_needs_command_shell("C:\\bin\\app") && !host_needs_command_shell("C:\\bin\\app.EXE") &&
          !host_needs_command_shell("C:\\bin\\app.com"));
}

static void escapes_a_quote_and_a_trailing_backslash_as_cross_spawn_does(void) {
    cJSON *arguments = parsed("[\"say \\\"hi\\\"\",\"back\\\\slash\\\\\"]");
    char *line = host_command_shell_line("app.cmd", arguments);
    check_text(line, "\"app.cmd ^^^\"say^^^ \\^^^\"hi\\^^^\"^^^\" ^^^\"back\\slash\\\\^^^\"\"", "quote and backslash");
    free(line);
    cJSON_Delete(arguments);
}

static void doubles_a_run_of_backslashes_before_a_quote_and_at_the_end(void) {
    cJSON *ending = parsed("[\"a\\\\\\\\\"]");
    char *ending_line = host_command_shell_line("x.cmd", ending);
    check_text(ending_line, "\"x.cmd ^^^\"a\\\\\\\\^^^\"\"", "a run at the end");
    cJSON *quoting = parsed("[\"\\\\\\\\\\\"\"]");
    char *quoting_line = host_command_shell_line("x.cmd", quoting);
    check_text(quoting_line, "\"x.cmd ^^^\"\\\\\\\\\\^^^\"^^^\"\"", "a run before a quote");
    free(quoting_line);
    cJSON_Delete(quoting);
    free(ending_line);
    cJSON_Delete(ending);
}

static void reads_options_given_both_ways_and_refuses_an_unknown_one_or_a_missing_value(void) {
    char *given[] = {"--catalog", "one", "--plugin=a", "--plugin", "b"};
    host_option options[2] = {{"catalog", NULL}, {"plugin", NULL}};
    char *problem = NULL;
    CHECK(host_parse_options(5, given, options, 2, NULL, &problem));
    check_text(host_last_value(&options[0]), "one", "catalog");
    expect_json(options[1].values, "[\"a\",\"b\"]", "plugins in order");
    host_free_options(options, 2);
    char *unknown[] = {"--catalogue", "x"};
    CHECK(!host_parse_options(2, unknown, options, 2, NULL, &problem));
    check_text(problem, "unknown option --catalogue", "unknown");
    free(problem);
    host_free_options(options, 2);
    char *missing[] = {"--catalog"};
    CHECK(!host_parse_options(1, missing, options, 2, NULL, &problem));
    check_text(problem, "--catalog needs a value", "missing value");
    free(problem);
    host_free_options(options, 2);
    char *positional[] = {"stray"};
    CHECK(!host_parse_options(1, positional, options, 2, NULL, &problem));
    check_text(problem, "unexpected argument stray", "positional");
    free(problem);
    host_free_options(options, 2);
}

int main(void) {
    snprintf(scratch, sizeof scratch, "%s/command-line-%d", RUNTIME_SCRATCH_PATH, (int)fixture_process_id());
    start_empty_directory(scratch);
    writes_the_question_to_the_terminal_and_answers_the_first_line_typed();
    answers_none_when_the_terminal_ends_before_a_line_is_typed();
    answers_none_when_there_is_no_terminal_to_open();
    names_the_controlling_terminal_of_this_operating_system();
    numbers_the_offered_methods_and_skip_and_installs_the_one_chosen_by_number();
    skips_for_skip_typed_by_name_or_number_or_an_empty_answer();
    picks_a_method_typed_by_name_and_skips_after_three_answers_that_name_none();
    asks_about_no_further_application_once_the_terminal_answers_nothing();
    runs_a_batch_file_through_the_command_shell_with_each_argument_escaped_for_both_parses();
    escapes_a_quote_and_a_trailing_backslash_as_cross_spawn_does();
    doubles_a_run_of_backslashes_before_a_quote_and_at_the_end();
    reads_options_given_both_ways_and_refuses_an_unknown_one_or_a_missing_value();
    CHECK(runtime_remove_tree(scratch) == 1);
    return check_summary("command_line_test");
}
