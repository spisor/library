#include "host_fixture.h"

static int run_command(host_world *world, program *run, const char *const *arguments) {
    return world_run(world, run, arguments, NULL);
}

static int lists_host(const cJSON *status, const char *id) {
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(status, "hosts")) {
        if (strcmp(text_at(cJSON_GetObjectItemCaseSensitive(row, "registration"), "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void reports_every_registered_host_and_what_the_cache_holds(void) {
    host_world world;
    install_launch_world(&world, "lifecycle", "status", 0);
    program run;
    const char *arguments[] = {"status", NULL};
    CHECK(run_command(&world, &run, arguments) == 0);
    cJSON *status = parsed(run.out);
    CHECK(lists_host(status, "forebay-host") && lists_host(status, LOADER_ID) && lists_host(status, BACKGROUND_ID));
    expect_json(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(status, "installed"),
                                                                                   "bundles"),
                                                 LOADER_ID),
                "[\"1.0.0\"]", "installed loader");
    expect_json(cJSON_GetObjectItemCaseSensitive(status, "damaged"), "[]", "nothing damaged");
    program misused;
    const char *extra[] = {"status", "now", NULL};
    CHECK(run_command(&world, &misused, extra) == 2);
    cJSON_Delete(status);
    program_free(&misused);
    program_free(&run);
    finish_world(&world);
}

static void checks_every_host_for_updates_and_updates_an_application_through_its_recorded_method(void) {
    host_world world;
    start_world_at(&world, "lifecycle", "update");
    list_launch_catalog(&world, "[]", 0);
    const char *choice[] = {"--application", APPLICATION_ID "=fake", NULL};
    CHECK(world_install(&world, choice, NULL) == 0);
    list_loader(&world.base, LOADER_ID, "1.1.0", application_json(APPLICATION_ID, APPLICATION_ID, "{\"fake\": \"fixture-pkg\"}"),
                "[]", 0, "[\"" BACKGROUND_ID "\"]");
    write_catalog(&world.base);
    program run;
    const char *arguments[] = {"update", NULL};
    CHECK(run_command(&world, &run, arguments) == 0);
    cJSON *summary = parsed(run.out);
    const cJSON *checks = cJSON_GetObjectItemCaseSensitive(summary, "checks");
    CHECK(cJSON_HasObjectItem(checks, "forebay-host") && cJSON_HasObjectItem(checks, LOADER_ID) &&
          cJSON_HasObjectItem(checks, BACKGROUND_ID));
    CHECK(exists_in(world.base.cache, "bundles/" LOADER_ID "/1.1.0"));
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(summary, "applications"), LOADER_ID),
                       "version"),
               "2.0.0", "the application updated through fake");
    expect_json(cJSON_GetObjectItemCaseSensitive(summary, "refused"), "[]", "nothing refused");
    cJSON *references = references_of(&world, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(references, "next"), LOADER_ID), "1.1.0", "scheduled for the next start");
    cJSON_Delete(references);
    cJSON_Delete(summary);
    program_free(&run);
    finish_world(&world);
}

static void removes_a_host_its_registration_and_its_references_and_sweeps_what_nothing_needs(void) {
    host_world world;
    install_launch_world(&world, "lifecycle", "remove", 0);
    program run;
    const char *arguments[] = {"remove", LOADER_ID, NULL};
    CHECK(run_command(&world, &run, arguments) == 0);
    cJSON *summary = parsed(run.out);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(summary, "removed"), "forgotten")));
    cJSON *registration = registration_in(&world, LOADER_ID);
    CHECK(registration == NULL);
    CHECK(!exists_in(world.base.state, "bundles/" LOADER_ID ".json"));
    CHECK(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(summary, "swept"), "removed")));
    program misused;
    const char *none[] = {"remove", NULL};
    CHECK(run_command(&world, &misused, none) == 2);
    expect_contains(misused.err, "remove needs exactly one host id", "misused");
    program_free(&misused);
    cJSON_Delete(registration);
    cJSON_Delete(summary);
    program_free(&run);
    finish_world(&world);
}

static void strip_marker_sizes(const host_world *world, const char *rest) {
    cJSON *marker = json_in(world->base.cache, rest);
    CHECK(cJSON_HasObjectItem(marker, "sizes"));
    cJSON_DeleteItemFromObjectCaseSensitive(marker, "sizes");
    char *text = cJSON_PrintUnformatted(marker);
    write_in(world->base.cache, rest, text);
    cJSON_free(text);
    cJSON_Delete(marker);
}

/** A marker from before sizes were recorded: status reports it as damaged with its fix, and update installs it again. */
static void reports_a_marker_that_records_no_sizes_and_update_repairs_it(void) {
    host_world world;
    install_launch_world(&world, "lifecycle", "no-sizes", 0);
    strip_marker_sizes(&world, "bundles/" LOADER_ID "/1.0.0/.installed.json");
    program status;
    const char *status_arguments[] = {"status", NULL};
    CHECK(run_command(&world, &status, status_arguments) == 1);
    cJSON *answer = parsed(status.out);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(answer, "damaged"), 0);
    check_text(text_at(first, "id"), LOADER_ID, "the loader is damaged");
    expect_contains(status.err, "records no size for", "names what is missing");
    expect_contains(status.err, "fix: install the version again", "with the reinstall fix");
    program update;
    const char *update_arguments[] = {"update", NULL};
    CHECK(run_command(&world, &update, update_arguments) == 0);
    cJSON *marker = json_in(world.base.cache, "bundles/" LOADER_ID "/1.0.0/.installed.json");
    CHECK(cJSON_HasObjectItem(marker, "sizes"));
    program again;
    CHECK(run_command(&world, &again, status_arguments) == 0);
    cJSON_Delete(marker);
    cJSON_Delete(answer);
    program_free(&again);
    program_free(&update);
    program_free(&status);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    reports_every_registered_host_and_what_the_cache_holds();
    checks_every_host_for_updates_and_updates_an_application_through_its_recorded_method();
    removes_a_host_its_registration_and_its_references_and_sweeps_what_nothing_needs();
    reports_a_marker_that_records_no_sizes_and_update_repairs_it();
    expect_user_path_unchanged(&guard);
    return check_summary("lifecycle_command_test");
}
