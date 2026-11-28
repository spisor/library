#include "host_fixture.h"

#include "host_output.h"
#include "spisor/allocation.h"

/**
 * @file
 * `spisor-host ui` as an application's user command and a person run it: it starts the entry of the interface item the
 * install chose, whatever that item is called, with the command line every interface takes, and refuses on a headless
 * install with a fix naming the `interface` setting and the catalog's interface items. The interface here is the fake
 * application, which records how it was started.
 */

#define INTERFACE_ID "fixture-interface"
#define OTHER_INTERFACE_ID "other-interface"
#define INTERFACE_ENTRY "interface" EXECUTABLE_ENDING

/** Lists interface item id, native, its bundle the fake application as its entry. */
static void list_interface(install_base *base, const char *id) {
    cJSON *item = catalog_item(base, id, "interface", "native", "1.0.0");
    cJSON *files = cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(item, "versions"),
                                                                   "1.0.0"),
                                           "files");
    cJSON_AddItemToObject(files, "host.json",
                          served_reference(base, id, "1.0.0", "host.json",
                                           "{\"kind\": \"interface\", \"runtime\": \"native\", \"entry\": \""
                                           INTERFACE_ENTRY "\"}"));
    cJSON_AddItemToObject(files, INTERFACE_ENTRY, served_copy(base, id, "1.0.0", INTERFACE_ENTRY, FAKE_APPLICATION_PATH));
}

/**
 * The launch ecosystem with the interfaces `ids` lists, `interface_setting` set first unless NULL, installed, the
 * install answering 0 unless two interfaces and no setting leave the interface reported unavailable.
 */
static void install_ui_world(host_world *world, const char *label, const char *const *ids,
                             const char *interface_setting) {
    start_world_at(world, "ui", label);
    install_fake_application(world);
    list_launch_catalog(world, "[]", 0);
    for (const char *const *id = ids; id != NULL && *id != NULL; id++) {
        list_interface(&world->base, *id);
    }
    write_catalog(&world->base);
    if (interface_setting != NULL) {
        set_portable(&world->base, "interface", cJSON_CreateString(interface_setting));
    }
    int unchosen = interface_setting == NULL && ids != NULL && ids[0] != NULL && ids[1] != NULL;
    CHECK((world_install(world, NULL, world->bin) == 0) != unchosen);
}

static int run_ui(host_world *world, program *run, const char *const *arguments) {
    const char *command[8] = {"ui"};
    size_t count = 1;
    for (; arguments != NULL && *arguments != NULL && count < 7; arguments++) {
        command[count++] = *arguments;
    }
    command[count] = NULL;
    return world_run(world, run, command, NULL);
}

/** The interface's start once it recorded one, its arguments checked against expected_json, then let go. */
static void expect_interface_started_with(host_world *world, const char *expected_json, const char *label) {
    cJSON *started = await_application(world, "ui");
    cJSON *argv = cJSON_GetObjectItemCaseSensitive(started, "argv");
    const char *binary = cJSON_GetStringValue(cJSON_GetArrayItem(argv, 1));
    CHECK(binary != NULL && strstr(binary, RUNTIME_FIXTURE_BINARY) != NULL);
    cJSON_ReplaceItemInArray(argv, 1, cJSON_CreateString("<runtime binary>"));
    expect_json(argv, expected_json, label);
#if defined(_WIN32)
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(started, "consoleWindow")));
#endif
    long pid = started_pid(started);
    release_application(world, "ui");
    CHECK(gone_within(pid, START_TIMEOUT));
    cJSON_Delete(started);
}

static void a_window_start_runs_the_chosen_interface_whatever_it_is_called(void) {
    static const char *const interfaces[] = {INTERFACE_ID, NULL};
    host_world world;
    program run;
    install_ui_world(&world, "window", interfaces, NULL);
    const char *arguments[] = {LOADER_ID, "--window", NULL};
    CHECK(run_ui(&world, &run, arguments) == 0);
    expect_interface_started_with(&world, "[\"ui\", \"<runtime binary>\", \"" LOADER_ID "\", \"--window\"]",
                                  "the window start");
    program_free(&run);
    finish_world(&world);
}

static void with_no_terminal_attached_the_interface_starts_as_a_window(void) {
    static const char *const interfaces[] = {INTERFACE_ID, NULL};
    host_world world;
    program run;
    install_ui_world(&world, "no-terminal", interfaces, NULL);
    CHECK(run_ui(&world, &run, NULL) == 0);
    expect_interface_started_with(&world, "[\"ui\", \"<runtime binary>\", \"--window\"]", "a start with no terminal");
    program_free(&run);
    finish_world(&world);
}

static void the_interface_the_setting_chooses_starts_when_the_catalog_lists_two(void) {
    static const char *const interfaces[] = {INTERFACE_ID, OTHER_INTERFACE_ID, NULL};
    host_world world;
    program run;
    install_ui_world(&world, "chosen", interfaces, OTHER_INTERFACE_ID);
    const char *arguments[] = {"--window", NULL};
    CHECK(run_ui(&world, &run, arguments) == 0);
    expect_interface_started_with(&world, "[\"ui\", \"<runtime binary>\", \"--window\"]", "the chosen interface");
    program_free(&run);
    finish_world(&world);
}

static void expect_refused_with(const program *run, int code, const char *message_part, const char *fix_part,
                                const char *label) {
    if (code != HOST_EXIT_REFUSED) {
        fprintf(stderr, "%s: exited %d, expected a refusal\n", label, code);
        check_failures++;
    }
    expect_contains(run->err, message_part, label);
    expect_contains(run->err, fix_part, label);
    expect_contains(run->err, "interface setting", label);
}

static void a_headless_install_refuses_naming_the_setting_and_the_catalogs_interfaces(void) {
    static const char *const interfaces[] = {INTERFACE_ID, NULL};
    host_world world;
    program run;
    install_ui_world(&world, "headless-none", interfaces, "none");
    expect_refused_with(&run, run_ui(&world, &run, NULL), "interface setting is none", INTERFACE_ID,
                        "an install the setting made headless");
    CHECK(application_start(&world, "ui") == NULL);
    program_free(&run);
    finish_world(&world);
    install_ui_world(&world, "headless-unlisted", NULL, NULL);
    expect_refused_with(&run, run_ui(&world, &run, NULL), "lists no interface item", "add a catalog source",
                        "an install whose catalog lists no interface");
    program_free(&run);
    finish_world(&world);
}

static void two_interfaces_and_none_chosen_are_refused_naming_both(void) {
    static const char *const interfaces[] = {INTERFACE_ID, OTHER_INTERFACE_ID, NULL};
    host_world world;
    program run;
    install_ui_world(&world, "unchosen", interfaces, NULL);
    expect_refused_with(&run, run_ui(&world, &run, NULL), OTHER_INTERFACE_ID, INTERFACE_ID,
                        "two interfaces and none chosen");
    program_free(&run);
    finish_world(&world);
}

static void an_argument_it_does_not_take_is_a_misuse(void) {
    static const char *const interfaces[] = {INTERFACE_ID, NULL};
    host_world world;
    program run;
    install_ui_world(&world, "misuse", interfaces, NULL);
    const char *arguments[] = {LOADER_ID, "--tabs", NULL};
    CHECK(run_ui(&world, &run, arguments) == HOST_EXIT_MISUSED);
    program_free(&run);
    const char *two_loaders[] = {LOADER_ID, LOADER_ID, NULL};
    CHECK(run_ui(&world, &run, two_loaders) == HOST_EXIT_MISUSED);
    program_free(&run);
    CHECK(application_start(&world, "ui") == NULL);
    finish_world(&world);
}

int main(void) {
    a_window_start_runs_the_chosen_interface_whatever_it_is_called();
    with_no_terminal_attached_the_interface_starts_as_a_window();
    the_interface_the_setting_chooses_starts_when_the_catalog_lists_two();
    a_headless_install_refuses_naming_the_setting_and_the_catalogs_interfaces();
    two_interfaces_and_none_chosen_are_refused_naming_both();
    an_argument_it_does_not_take_is_a_misuse();
    return check_summary("ui_command_test");
}
