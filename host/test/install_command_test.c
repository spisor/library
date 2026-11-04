#include "host_fixture.h"

static int run_install(host_world *world, program *run, const char *const *extra) {
    char *index = index_path_of(&world->base);
    const char *arguments[16] = {"install", "--catalog", index};
    size_t count = 3;
    for (; extra != NULL && *extra != NULL && count < 15; extra++) {
        arguments[count++] = *extra;
    }
    arguments[count] = NULL;
    int code = world_run(world, run, arguments, NULL);
    free(index);
    return code;
}

static const cJSON *application_row(const cJSON *report, const char *id) {
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(report, "applications")) {
        if (strcmp(text_at(row, "id"), id) == 0) {
            return row;
        }
    }
    return NULL;
}

static void start_install_world(host_world *world, const char *label) {
    start_world_at(world, "install-command", label);
    list_launch_catalog(world, "[]", 0);
}

/* plan four's Review Focus 5 */

static void asks_nothing_without_a_terminal_and_reports_the_exact_application_flag(void) {
    host_world world;
    start_install_world(&world, "no-terminal");
    program run;
    CHECK(run_install(&world, &run, NULL) == 0);
    cJSON *report = parsed(run.out);
    const cJSON *row = application_row(report, APPLICATION_ID);
    check_text(text_at(row, "state"), "missing", "missing");
    check_text(text_at(row, "flag"), "--application " APPLICATION_ID "=fake", "the exact flag");
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "missing"), "[{\"application\":\"" APPLICATION_ID "\",\"methods\":[\"fake\"]}]",
                "missing to ask about");
    CHECK(!exists_in(world.base.cache, "methods/fake/installed.json"));
    check_text(run.err, "", "nothing asked");
    cJSON_Delete(report);
    program_free(&run);
    finish_world(&world);
}

static void installs_through_the_method_an_application_flag_names(void) {
    host_world world;
    start_install_world(&world, "flagged");
    program run;
    const char *choice[] = {"--application", APPLICATION_ID "=fake", NULL};
    CHECK(run_install(&world, &run, choice) == 0);
    cJSON *report = parsed(run.out);
    check_text(text_at(application_row(report, APPLICATION_ID), "state"), "installed", "installed");
    CHECK(exists_in(world.base.cache, "methods/fake/installed.json"));
    cJSON *registration = registration_in(&world, LOADER_ID);
    CHECK(registration != NULL);
    cJSON_Delete(registration);
    cJSON_Delete(report);
    program_free(&run);
    finish_world(&world);
}

static void answers_2_naming_the_offered_methods_for_a_method_that_is_not_offered_installing_nothing(void) {
    host_world world;
    start_install_world(&world, "not-offered");
    program run;
    const char *choice[] = {"--application", APPLICATION_ID "=winget", NULL};
    CHECK(run_install(&world, &run, choice) == 2);
    expect_contains(run.err, APPLICATION_ID "=winget", "names the flag");
    expect_contains(run.err, "offered methods are fake, skip", "names the offered methods");
    expect_contains(run.err, "usage:", "with the usage");
    CHECK(!exists_in(world.base.cache, "bundles"));
    program_free(&run);
    finish_world(&world);
}

static void answers_2_for_an_application_flag_that_is_not_id_equals_method_or_names_one_twice(void) {
    host_world world;
    start_install_world(&world, "malformed");
    program shapeless;
    const char *bare[] = {"--application", APPLICATION_ID, NULL};
    CHECK(run_install(&world, &shapeless, bare) == 2);
    expect_contains(shapeless.err, "--application " APPLICATION_ID " is not <id>=<method>", "not id=method");
    program twice;
    const char *repeated[] = {"--application", APPLICATION_ID "=fake", "--application", APPLICATION_ID "=skip", NULL};
    CHECK(run_install(&world, &twice, repeated) == 2);
    expect_contains(twice.err, "--application names " APPLICATION_ID " twice", "named twice");
    program_free(&twice);
    program_free(&shapeless);
    finish_world(&world);
}

static void answers_2_without_a_catalog_or_with_one_that_is_neither_https_nor_a_path(void) {
    host_world world;
    start_install_world(&world, "no-catalog");
    program missing;
    const char *none[] = {"install", NULL};
    CHECK(world_run(&world, &missing, none, NULL) == 2);
    expect_contains(missing.err, "--catalog <path or url> is required", "required");
    program scheme;
    const char *ftp[] = {"install", "--catalog", "ftp://catalog.test/catalog.json", NULL};
    CHECK(world_run(&world, &scheme, ftp, NULL) == 2);
    expect_contains(scheme.err, "is neither an https url nor a local path", "scheme");
    program unknown;
    const char *option[] = {"install", "--catalogue", "x", NULL};
    CHECK(world_run(&world, &unknown, option, NULL) == 2);
    expect_contains(unknown.err, "unknown option --catalogue", "unknown option");
    program_free(&unknown);
    program_free(&scheme);
    program_free(&missing);
    finish_world(&world);
}

static void answers_1_naming_what_could_not_be_installed_after_printing_the_report(void) {
    host_world world;
    start_install_world(&world, "unavailable");
    program run;
    const char *absent[] = {"--plugin", "absent-plugin", NULL};
    CHECK(run_install(&world, &run, absent) == 1);
    expect_contains(run.out, "\"unavailable\"", "report printed");
    expect_contains(run.err, "absent-plugin could not be installed", "named");
    expect_contains(run.err, "fix: see each item's fix under unavailable above, then install again", "its fix");
    program_free(&run);
    finish_world(&world);
}

/* design 7: every command shim and entry runs forebay-boot from the cache */

static void puts_forebay_boot_into_the_cache_from_the_forebay_host_build_it_installed(void) {
    host_world world;
    start_install_world(&world, "boot-stub");
    install_fake_application(&world);
    program run;
    CHECK(run_install(&world, &run, NULL) == 0);
    char *stub = joined(world.base.cache, "boot/forebay-boot" EXECUTABLE_ENDING);
    char stub_hash[RUNTIME_SHA256_HEX_SIZE];
    char built_hash[RUNTIME_SHA256_HEX_SIZE];
    CHECK(spisor_sha256_file(stub, stub_hash) && spisor_sha256_file(FOREBAY_BOOT_PATH, built_hash) &&
          strcmp(stub_hash, built_hash) == 0);
    free(stub);
    program_free(&run);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    asks_nothing_without_a_terminal_and_reports_the_exact_application_flag();
    installs_through_the_method_an_application_flag_names();
    answers_2_naming_the_offered_methods_for_a_method_that_is_not_offered_installing_nothing();
    answers_2_for_an_application_flag_that_is_not_id_equals_method_or_names_one_twice();
    answers_2_without_a_catalog_or_with_one_that_is_neither_https_nor_a_path();
    answers_1_naming_what_could_not_be_installed_after_printing_the_report();
    puts_forebay_boot_into_the_cache_from_the_forebay_host_build_it_installed();
    expect_user_path_unchanged(&guard);
    return check_summary("install_command_test");
}
