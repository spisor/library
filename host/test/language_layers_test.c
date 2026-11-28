#include "host_fixture.h"
#include "spisor/allocation.h"

#define FRONT_DOOR_PLUGIN "compiled-one"
#define SERVING_PLUGIN "serving-one"
#define SECOND_PLUGIN "compiled-two"
#define FRONT_DOOR_LANGUAGE "fixture-one"
#define SECOND_LANGUAGE "fixture-two"
#define TYPESCRIPT_IDENTITY LOADER_ID ".typescript"
#define SECOND_IDENTITY LOADER_ID "." SECOND_LANGUAGE
/** runtime's own interval between a run's prunes, after which a C layer that lost its holders ends. */
#define RUNTIME_PRUNE_MILLISECONDS 5000
#define GREET_ACTION "{\"greet\": {\"call\": {\"service\": \"%s:greeter\", \"method\": \"greet\", \"argument\": {\"name\": \"world\"}}}}"

/** The file name of path, after its last separator. */
static const char *file_name_of(const char *path) {
    const char *name = path;
    for (const char *at = path; *at != 0; at++) {
        if (*at == '/' || *at == '\\') {
            name = at + 1;
        }
    }
    return name;
}

/** Lists every file below folder, at its path relative to the item, as the platform files of version of item. */
static void list_tree(install_base *base, cJSON *item, const char *id, const char *version, const char *folder,
                      const char *prefix) {
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(folder, &listing) == SPISOR_OK);
    for (size_t index = 0; index < listing.count; index++) {
        const char *name = listing.entries[index].name;
        char key[256];
        int written = snprintf(key, sizeof key, "%s%s%s", prefix, prefix[0] == 0 ? "" : "/", name);
        CHECK(written > 0 && (size_t)written < sizeof key);
        if (written <= 0 || (size_t)written >= sizeof key) {
            continue;
        }
        char *path = joined(folder, name);
        if (listing.entries[index].is_directory) {
            list_tree(base, item, id, version, path, key);
        } else {
            list_platform_copy(base, item, id, version, key, path);
        }
        spisor_memory_release(path);
    }
    runtime_free_directory_listing(&listing);
}

/** The language host stage-bundle staged for language at build time, listed as the catalog item it is. */
static void list_staged_language_host(install_base *base, const char *language, const char *runtime) {
    char id[128];
    snprintf(id, sizeof id, "%s-host", language);
    cJSON *item = catalog_item(base, id, "language-host", runtime, "1.0.0");
    cJSON_AddStringToObject(item, "language", language);
    char *folder = joined(STAGED_LANGUAGE_HOSTS_PATH, id);
    list_tree(base, item, id, "1.0.0", folder, "");
    spisor_memory_release(folder);
}

/** runtime as the build staged it, its binary the one a C layer binds weivel.host from. */
static void list_staged_runtime(install_base *base) {
    char *folder = joined(STAGED_BUNDLES_PATH, "runtime");
    cJSON *declaration = json_in(folder, "library.json");
    const char *version = text_at(declaration, "version");
    cJSON *item = catalog_item(base, "runtime", "library", NULL, version);
    char *declaration_path = joined(folder, "library.json");
    char *binary = joined(folder, RUNTIME_BINARY_NAME);
    list_platform_copy(base, item, "runtime", version, "library.json", declaration_path);
    list_platform_copy(base, item, "runtime", version, RUNTIME_BINARY_NAME, binary);
    spisor_memory_release(binary);
    spisor_memory_release(declaration_path);
    cJSON_Delete(declaration);
    spisor_memory_release(folder);
}

/**
 * The C fixture plugin as id in language: its manifest providing `<id>:greeter` and, when it greets, declaring the
 * action greet that calls it; its entry the plugin library.
 */
static void list_c_plugin(host_world *world, const char *id, const char *language, int greets) {
    install_base *base = &world->base;
    const char *library = file_name_of(C_FIXTURE_PLUGIN_PATH);
    char actions[512] = "";
    if (greets) {
        char action[400];
        snprintf(action, sizeof action, GREET_ACTION, id);
        snprintf(actions, sizeof actions, ", \"actions\": %s", action);
    }
    char manifest[2048];
    snprintf(manifest, sizeof manifest,
             "{\"id\": \"%s\", \"importName\": \"%s\", \"api\": 1, \"version\": \"1.0.0\", \"language\": \"%s\", "
             "\"entry\": \"%s\", \"services\": {\"provides\": [\"%s:greeter\"]}%s}",
             id, id, language, library, id, actions);
    char rest[256];
    snprintf(rest, sizeof rest, "inputs/%s", id);
    char *folder = joined(base->root, rest);
    write_in(folder, "plugin.json", manifest);
    char *manifest_path = joined(folder, "plugin.json");
    cJSON *item = catalog_item(base, id, "plugin", language, "1.0.0");
    list_platform_copy(base, item, id, "1.0.0", "plugin.json", manifest_path);
    list_platform_copy(base, item, id, "1.0.0", library, C_FIXTURE_PLUGIN_PATH);
    spisor_memory_release(manifest_path);
    spisor_memory_release(folder);
}

/**
 * A loader requiring FRONT_DOOR_PLUGIN, so the fixture-one layer holds the front door, with SERVING_PLUGIN beside it,
 * which declares no action, SECOND_PLUGIN in fixture-two and SESSION_PLUGIN in TypeScript, each language's host the
 * item the build staged and installed by install.
 */
static void install_language_world(host_world *world, const char *label) {
    start_world_at(world, "language-layers", label);
    install_fake_application(world);
    install_base *base = &world->base;
    list_native_spisor_host(base, "1.0.0");
    list_staged_runtime(base);
    list_loader_requiring(base, LOADER_ID, "1.0.0", application_json(APPLICATION_ID, APPLICATION_ID, "{\"fake\": \"fixture-pkg\"}"),
                          "[]", 0, "[]", "[\"" FRONT_DOOR_PLUGIN "\"]");
    list_c_plugin(world, FRONT_DOOR_PLUGIN, FRONT_DOOR_LANGUAGE, 1);
    list_c_plugin(world, SERVING_PLUGIN, FRONT_DOOR_LANGUAGE, 0);
    list_c_plugin(world, SECOND_PLUGIN, SECOND_LANGUAGE, 1);
    list_plugin(base, SESSION_PLUGIN, "1.0.0", "typescript");
    list_file(base, catalog_item(base, "forebay-library", "library", NULL, "0.1.2"), "forebay-library", "0.1.2",
              "package.json", "{\"version\": \"0.1.2\"}", 1);
    list_staged_language_host(base, FRONT_DOOR_LANGUAGE, "native");
    list_staged_language_host(base, SECOND_LANGUAGE, "native");
    list_staged_language_host(base, "typescript", "javascript");
    write_catalog(base);
    char *node = install_fake_node(world);
    char path[4096];
    snprintf(path, sizeof path, "%s" PATH_SEPARATOR "%s", world->bin, node);
    const char *plugins[] = {"--plugin", SERVING_PLUGIN, "--plugin", SECOND_PLUGIN, "--plugin", SESSION_PLUGIN, NULL};
    CHECK(world_install(world, plugins, path) == 0);
    spisor_memory_release(node);
    world->front_door_language = FRONT_DOOR_LANGUAGE;
}

/** Where part first stands in text, or -1 when it does not. */
static long place_in(const char *text, const char *part) {
    const char *found = text == NULL ? NULL : strstr(text, part);
    return found == NULL ? -1 : (long)(found - text);
}

/** How many lines of the activations file begin with "<plugin> ". */
static int activations_of(const host_world *world, const char *plugin) {
    char *text = text_in(world->activations, "");
    char start[128];
    snprintf(start, sizeof start, "%s ", plugin);
    int count = 0;
    for (const char *line = text; line != NULL && *line != 0;) {
        count += strncmp(line, start, strlen(start)) == 0;
        const char *end = strchr(line, '\n');
        line = end == NULL ? NULL : end + 1;
    }
    spisor_memory_release(text);
    return count;
}

static void expect_activated_in(const host_world *world, const char *plugin, long pid, const char *label) {
    char *text = text_in(world->activations, "");
    char line[128];
    snprintf(line, sizeof line, "%s %ld\n", plugin, pid);
    expect_contains(text, line, label);
    spisor_memory_release(text);
}

/** Whether the loader's runs folder holds a run record written by pid, as `<pid>.<time>-<number>.json`. */
static int has_run_record_of(const host_world *world, long pid) {
    char *folder = joined(world->base.state, "bundles/runs/" LOADER_ID);
    char start[32];
    snprintf(start, sizeof start, "%ld.", pid);
    runtime_directory_listing listing;
    int found = 0;
    if (runtime_list_directory(folder, &listing) == SPISOR_OK) {
        for (size_t index = 0; index < listing.count; index++) {
            found |= strncmp(listing.entries[index].name, start, strlen(start)) == 0;
        }
        runtime_free_directory_listing(&listing);
    }
    spisor_memory_release(folder);
    return found;
}

/** Runs spisor-host action for the loader's plugin's greet with no input; the line it printed, freed. */
static char *greeted_by(host_world *world, const char *plugin) {
    program run;
    const char *arguments[] = {"action", LOADER_ID, plugin, "greet", NULL};
    int code = world_run(world, &run, arguments, NULL);
    if (code != 0) {
        fprintf(stderr, "action %s answered %d: %s\n%s\n", plugin, code, run.out, run.err);
        check_failures++;
    }
    char *printed = copy_text(run.out == NULL ? "" : run.out);
    printed[strcspn(printed, "\r\n")] = 0;
    program_free(&run);
    return printed;
}

static void starts_a_layer_per_language_the_front_door_s_first_and_runs_actions_beside_the_live_session(void) {
    host_world world;
    install_language_world(&world, "three-layers");
    program launcher;
    const char *passed[] = {"layers", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "layers");
    long front = await_holders(&world, LOADER_ID, 2);
    long second = await_holders(&world, SECOND_IDENTITY, 2);
    long typescript = await_holders(&world, TYPESCRIPT_IDENTITY, 2);
    CHECK(front != second && front != typescript && second != typescript);
    char *starts = await_starts_holding(&world, " " SECOND_LANGUAGE " " LOADER_ID "\n");
    spisor_memory_release(starts);
    starts = await_starts_holding(&world, " typescript " LOADER_ID);
    long front_start = place_in(starts, FRONT_DOOR_LANGUAGE "-host ");
    long second_start = place_in(starts, SECOND_LANGUAGE "-host ");
    long typescript_start = place_in(starts, " typescript " LOADER_ID);
    expect_contains(starts, " " FRONT_DOOR_LANGUAGE " " LOADER_ID "\n", "the front door's layer run for its language");
    expect_contains(starts, " " SECOND_LANGUAGE " " LOADER_ID "\n", "the second layer run for its language");
    expect_contains(starts, "typescript-host", "the TypeScript layer from its own item");
    if (front_start < 0 || second_start < front_start || typescript_start < front_start) {
        fprintf(stderr, "the front door's layer did not start first:\n%s\n", starts == NULL ? "(none)" : starts);
        check_failures++;
    }
    CHECK(has_run_record_of(&world, front));
    CHECK(has_run_record_of(&world, second));
    expect_activated_in(&world, FRONT_DOOR_PLUGIN, front, "the front door's plugin in its layer");
    expect_activated_in(&world, SERVING_PLUGIN, front, "the serving plugin beside it");
    expect_activated_in(&world, SECOND_PLUGIN, second, "the second language's plugin in its own layer");

    char *greeted = greeted_by(&world, FRONT_DOOR_PLUGIN);
    check_text(greeted, "\"hello, world, from " FRONT_DOOR_PLUGIN "\"", "the front door's action result");
    CHECK(activations_of(&world, FRONT_DOOR_PLUGIN) == 2);
    CHECK(activations_of(&world, SERVING_PLUGIN) == 1);
    CHECK(runtime_process_is_alive((double)front));
    expect_holders(&world, LOADER_ID, launcher.child.pid, started_pid(started), "the live session still held");
    spisor_memory_release(greeted);
    greeted = greeted_by(&world, SECOND_PLUGIN);
    check_text(greeted, "\"hello, world, from " SECOND_PLUGIN "\"", "the second language's action result");
    CHECK(activations_of(&world, SECOND_PLUGIN) == 2);

    release_application(&world, "layers");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(front, RUNTIME_PRUNE_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(second, RUNTIME_PRUNE_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(typescript, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(front);
    stop_if_alive(second);
    stop_if_alive(typescript);
    spisor_memory_release(greeted);
    spisor_memory_release(starts);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    starts_a_layer_per_language_the_front_door_s_first_and_runs_actions_beside_the_live_session();
    expect_user_path_unchanged(&guard);
    return check_summary("language_layers_test");
}
