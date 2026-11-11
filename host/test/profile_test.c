#include "host_fixture.h"

#include "tree_snapshot.h"

#define PROFILE_NAME "J\xc3\xb6rg"
#define COMMAND_NAME "fx"

/**
 * Moves the user's home, the places and the bin folder into a profile folder named with a letter outside ASCII.
 *
 * @implNote The bin folder lies in the left-out spisor folder, because the snapshot is compared as text and
 * the application's bytes hold a NUL that would end the comparison early.
 */
static char *move_into_profile(host_world *world) {
    char *profile = joined(world->base.root, PROFILE_NAME);
    free(world->base.user);
    free(world->base.portable);
    free(world->base.state);
    free(world->base.cache);
    free(world->bin);
    world->base.user = copy_text(profile);
    world->base.portable = joined(profile, "spisor/portable");
    world->base.state = joined(profile, "spisor/state");
    world->base.cache = joined(profile, "spisor/cache");
    world->bin = joined(profile, "spisor/bin");
    CHECK(spisor_make_directories(world->bin) == 1);
    write_in(profile, ".profile", "# J\xc3\xb6rg's profile\n");
    write_in(profile, "notes/caf\xc3\xa9.txt", "kept as it is\n");
    write_in(profile, "homes/claude/settings.json",
             "{\n  \"theme\": \"dark\",\n  \"enabledPlugins\": {\"other@market\": true}\n}\n");
    return profile;
}

static char *shim_of(const host_world *world) {
    return joined(world->base.state, "bin/" COMMAND_NAME EXECUTABLE_ENDING);
}

static void launch_through_the_command(host_world *world) {
    char *shim = shim_of(world);
    CHECK(spisor_file_exists(shim));
    program launcher;
    const char *arguments[] = {"joerg", "4", NULL};
    world_start(world, &launcher, shim, arguments, NULL, 0);
    cJSON *started = await_application(world, "joerg");
    expect_json(cJSON_GetObjectItemCaseSensitive(started, "argv"), "[\"joerg\",\"4\"]", "the command's arguments");
    long background = await_holders(world, BACKGROUND_ID, 2);
    release_application(world, "joerg");
    CHECK(program_finish(&launcher, 30000) == 4);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    free(shim);
}

static void remove_every_host(host_world *world) {
    const char *ids[] = {LOADER_ID, BACKGROUND_ID, "spisor-host"};
    for (size_t index = 0; index < 3; index++) {
        program run;
        const char *arguments[] = {"remove", ids[index], NULL};
        if (world_run(world, &run, arguments, NULL) != 0) {
            fprintf(stderr, "remove %s answered %d: %s\n", ids[index], run.exit_code, run.err);
            check_failures++;
        }
        program_free(&run);
    }
}

static void installs_launches_through_its_command_and_removes_leaving_the_profile_byte_identical(void) {
    host_world world;
    start_world_at(&world, "profile", "joerg");
    char *profile = move_into_profile(&world);
    install_fake_application(&world);
    list_launch_catalog_for(&world, "claude", "[\"" COMMAND_NAME "\"]", 0);
    char *before = tree_snapshot(profile, "/spisor");
    CHECK(world_install(&world, NULL, NULL) == 0);
    char *installed = tree_snapshot(profile, "/spisor");
    CHECK(installed != NULL && before != NULL && strcmp(installed, before) != 0);
    free(installed);
    cJSON *registration = registration_in(&world, LOADER_ID);
    expect_contains(text_at(registration, "home"), PROFILE_NAME, "the home under the profile");
    launch_through_the_command(&world);
    remove_every_host(&world);
    char *after = tree_snapshot(profile, "/spisor");
    CHECK(before != NULL && after != NULL && strcmp(before, after) == 0);
    if (before != NULL && after != NULL && strcmp(before, after) != 0) {
        fprintf(stderr, "before:\n%s\nafter:\n%s\n", before, after);
    }
    free(after);
    free(before);
    cJSON_Delete(registration);
    free(profile);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    installs_launches_through_its_command_and_removes_leaving_the_profile_byte_identical();
    expect_user_path_unchanged(&guard);
    return check_summary("profile_test");
}
