#include "host_fixture.h"

#include "host_child.h"

typedef struct child_run {
    const host_world *world;
    const char *tag;
    int terminate;
} child_run;

static void on_started(double pid, void *context) {
    child_run *run = context;
    (void)pid;
#if !defined(_WIN32)
    if (run->terminate) {
        kill(getpid(), SIGTERM);
        return;
    }
#endif
    char *signals = joined(run->world->signals, "");
    char name[256];
    snprintf(name, sizeof name, "%s.release", run->tag);
    write_in(signals, name, "");
    free(signals);
}

static int run_application(const host_world *world, const char *binary, const char *const *arguments, int terminate,
                           cJSON **refusal) {
    child_run run = {world, arguments[0] == NULL ? "" : arguments[0], terminate};
    cJSON *list = strings_of(arguments);
    cJSON *environment = world_environment(world);
    int exit_code = -1;
    *refusal = NULL;
    host_child_watch watch = {on_started, NULL, 0, &run};
    int ran = host_run_child(binary, list, environment, &watch, &exit_code, refusal);
    cJSON_Delete(environment);
    cJSON_Delete(list);
    return ran ? exit_code : -1;
}

static char *fake_binary(const host_world *world) {
    return joined(world->bin, APPLICATION_ID EXECUTABLE_ENDING);
}

static void answers_the_application_s_exit_code(void) {
    host_world world;
    start_world_at(&world, "child", "exit-code");
    install_fake_application(&world);
    char *binary = fake_binary(&world);
    cJSON *refusal = NULL;
    const char *none[] = {NULL};
    const char *three[] = {"", "3", NULL};
    CHECK(run_application(&world, binary, none, 0, &refusal) == 0);
    CHECK(run_application(&world, binary, three, 0, &refusal) == 3);
    free(binary);
    finish_world(&world);
}

static void passes_every_argument_through_unchanged(void) {
    host_world world;
    start_world_at(&world, "child", "passes");
    install_fake_application(&world);
    char *binary = fake_binary(&world);
    cJSON *refusal = NULL;
    const char *given[] = {"passes", "5", "two words", "a&b", "say \"hi\"", "(x|y)", "back\\slash\\", "J\xc3\xb6rg", NULL};
    CHECK(run_application(&world, binary, given, 0, &refusal) == 5);
    cJSON *started = application_start(&world, "passes");
    cJSON *expected = strings_of(given);
    CHECK(started != NULL && cJSON_Compare(cJSON_GetObjectItemCaseSensitive(started, "argv"), expected, 1));
    cJSON_Delete(expected);
    cJSON_Delete(started);
    free(binary);
    finish_world(&world);
}

#if defined(_WIN32)
static void hands_a_batch_shim_each_argument_byte_identical_and_runs_nothing_an_argument_names(void) {
    host_world world;
    start_world_at(&world, "child", "vectors");
    install_fake_application(&world);
    char *application = fake_binary(&world);
    char wrapper[2048];
    snprintf(wrapper, sizeof wrapper, "@\"%s\" %%*\r\n", application);
    write_in(world.bin, "shim-app.cmd", wrapper);
    char *shim = joined(world.bin, "shim-app.cmd");
    char *marker = joined(world.signals, "injected");
    char first[2048];
    char third[2048];
    snprintf(first, sizeof first, "x\" & echo INJECTED> %s & \"y", marker);
    snprintf(third, sizeof third, "\"%%PATH%%\" ^ ! & echo INJECTED> %s", marker);
    const char *given[] = {"vectors", "0", first, "\"x\" & y", third, "a\\\\", "\\\\\"x", NULL};
    cJSON *refusal = NULL;
    CHECK(run_application(&world, shim, given, 0, &refusal) == 0);
    cJSON *started = application_start(&world, "vectors");
    cJSON *expected = strings_of(given);
    CHECK(started != NULL && cJSON_Compare(cJSON_GetObjectItemCaseSensitive(started, "argv"), expected, 1));
    CHECK(!spisor_file_exists(marker));
    cJSON_Delete(expected);
    cJSON_Delete(started);
    free(marker);
    free(shim);
    free(application);
    finish_world(&world);
}
#else
static void passes_a_sigterm_on_to_the_application_and_answers_128_plus_its_number(void) {
    host_world world;
    start_world_at(&world, "child", "sigterm");
    install_fake_application(&world);
    char *binary = fake_binary(&world);
    cJSON *refusal = NULL;
    const char *given[] = {"terminated", "0", NULL};
    CHECK(run_application(&world, binary, given, 1, &refusal) == 143);
    free(binary);
    finish_world(&world);
}
#endif

static void refuses_an_application_that_cannot_be_started_with_what_to_do(void) {
    host_world world;
    start_world_at(&world, "child", "missing");
    char *missing = joined(world.base.root, "absent/missing-app" EXECUTABLE_ENDING);
    cJSON *refusal = NULL;
    const char *none[] = {NULL};
    CHECK(run_application(&world, missing, none, 0, &refusal) == -1);
    expect_contains(text_at(refusal, "fix"), "install", "the fix");
    cJSON_Delete(refusal);
    free(missing);
    finish_world(&world);
}

int main(void) {
    answers_the_application_s_exit_code();
    passes_every_argument_through_unchanged();
#if defined(_WIN32)
    hands_a_batch_shim_each_argument_byte_identical_and_runs_nothing_an_argument_names();
#else
    passes_a_sigterm_on_to_the_application_and_answers_128_plus_its_number();
#endif
    refuses_an_application_that_cannot_be_started_with_what_to_do();
    return check_summary("child_test");
}
