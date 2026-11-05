#ifndef HOST_TEST_PROOF_FIXTURE_H
#define HOST_TEST_PROOF_FIXTURE_H

#include "host_fixture.h"

#define PROOF_PLUGIN_ID "fixture-plugin"
#define PROOF_COMMAND "fx"

static const char *const JAVASCRIPT_RUNTIMES[] = {"node", "bun"};

/** The forebay-host the build staged into its bundle directory, the one a catalog lists. */
static inline char *staged_forebay_host(void) {
    char rest[256];
    snprintf(rest, sizeof rest, "forebay-host/%s/forebay-host" EXECUTABLE_ENDING, runtime_platform_key());
    return joined(STAGED_BUNDLES_PATH, rest);
}

static inline int holds_runtime(const char *directory, const char *name) {
    char file[64];
    snprintf(file, sizeof file, "%s" EXECUTABLE_ENDING, name);
    char *path = joined(directory, file);
    int held = spisor_file_exists(path);
    free(path);
    return held;
}

/** Whether directory holds node or bun under the name a PATH lookup would find. */
static inline int holds_javascript_runtime(const char *directory) {
    for (size_t index = 0; index < sizeof JAVASCRIPT_RUNTIMES / sizeof JAVASCRIPT_RUNTIMES[0]; index++) {
        if (holds_runtime(directory, JAVASCRIPT_RUNTIMES[index])) {
            return 1;
        }
    }
    return 0;
}

/** The directories of path_variable that hold no JavaScript runtime, joined again; freed with free. */
static inline char *path_without_javascript(const char *path_variable) {
    char *copy = copy_text(path_variable == NULL ? "" : path_variable);
    char *kept = calloc(strlen(copy) + 1, 1);
    for (char *directory = strtok(copy, PATH_SEPARATOR); directory != NULL; directory = strtok(NULL, PATH_SEPARATOR)) {
        if (directory[0] != 0 && !holds_javascript_runtime(directory)) {
            strcat(kept, kept[0] == 0 ? "" : PATH_SEPARATOR);
            strcat(kept, directory);
        }
    }
    free(copy);
    return kept;
}

/** Fails the test naming the directory of path_variable that holds a JavaScript runtime, when any does. */
static inline void expect_no_javascript_on(const char *path_variable, const char *label) {
    char *copy = copy_text(path_variable);
    for (char *directory = strtok(copy, PATH_SEPARATOR); directory != NULL; directory = strtok(NULL, PATH_SEPARATOR)) {
        if (directory[0] != 0 && holds_javascript_runtime(directory)) {
            fprintf(stderr, "%s: %s holds a JavaScript runtime\n", label, directory);
            check_failures++;
        }
    }
    free(copy);
}

/** Runs executable with the NULL-ended arguments under the world's environment and path to its end; its exit code. */
static inline int proof_run(host_world *world, program *run, const char *executable, const char *const *arguments,
                            const char *path) {
    world_start(world, run, executable, arguments, path, 1);
    int code = program_finish(run, 120000);
    if (code != 0) {
        fprintf(stderr, "%s %s answered %d:\n%s\n%s\n", executable, arguments[0], code, run->out, run->err);
    }
    return code;
}

/** A loader for Claude Code at version, served by the fake application and requesting one command. */
static inline char *write_proof_loader(const host_world *world, const char *version) {
    char rest[256];
    snprintf(rest, sizeof rest, "inputs/loader-%s", version);
    char *directory = joined(world->base.root, rest);
    char manifest[2048];
    snprintf(manifest, sizeof manifest,
             "{\"id\": \"" LOADER_ID "\", \"version\": \"%s\", \"runtime\": \"native\", \"application\": {\"id\": \"claude\", "
             "\"label\": \"Claude Code\", \"binary\": \"" APPLICATION_ID "\", \"home\": {\"candidates\": [\"~/homes/claude\"]}, "
             "\"packages\": {\"fake\": \"fixture-pkg\"}}, \"pathCommands\": {\"requested\": [\"" PROOF_COMMAND "\"], \"minimum\": 0}, "
             "\"background\": []}",
             version);
    write_in(directory, "host.json", manifest);
    return directory;
}

/** A plugin that runs on no JavaScript runtime. */
static inline char *write_proof_plugin(const host_world *world) {
    char *directory = joined(world->base.root, "inputs/plugin");
    write_in(directory, "plugin.json",
             "{\"id\": \"" PROOF_PLUGIN_ID "\", \"version\": \"1.0.0\", \"runtime\": \"native\", \"entry\": \"plugin.bin\"}");
    write_in(directory, "plugin.bin", "native plugin bytes");
    return directory;
}

/**
 * catalog build over the staged forebay-host and runtime bundles, the fixture plugin and the loader at
 * each of the NULL-ended loader_versions, written into the world's catalog, run with no PATH at all.
 */
static inline void build_proof_catalog(host_world *world, const char *const *loader_versions) {
    CHECK(runtime_remove_tree(world->base.catalog) == 1);
    char host_argument[1200];
    char library_argument[1200];
    snprintf(host_argument, sizeof host_argument, "%s/forebay-host@%s", STAGED_BUNDLES_PATH, runtime_platform_key());
    snprintf(library_argument, sizeof library_argument, "runtime=%s/runtime@%s", STAGED_BUNDLES_PATH, runtime_platform_key());
    char *plugin = write_proof_plugin(world);
    const char *arguments[32] = {"catalog", "build", "--host", host_argument, "--library", library_argument, "--plugin", plugin};
    size_t count = 8;
    char *loaders[8] = {NULL};
    size_t loader_count = 0;
    for (; *loader_versions != NULL && loader_count < 8; loader_versions++) {
        loaders[loader_count] = write_proof_loader(world, *loader_versions);
        arguments[count++] = "--host";
        arguments[count++] = loaders[loader_count++];
    }
    arguments[count++] = "--out";
    arguments[count++] = world->base.catalog;
    arguments[count] = NULL;
    char *empty = joined(world->base.root, "empty-path");
    CHECK(spisor_make_directories(empty) == 1);
    char *forebay_host = staged_forebay_host();
    program run;
    CHECK(proof_run(world, &run, forebay_host, arguments, empty) == 0);
    program_free(&run);
    free(forebay_host);
    free(empty);
    for (size_t index = 0; index < loader_count; index++) {
        free(loaders[index]);
    }
    free(plugin);
}

/** The home of the user the world installs for, holding Claude Code's home with settings of its own. */
static inline void prepare_user(const host_world *world) {
    CHECK(spisor_make_directories(world->base.user) == 1);
    write_in(world->base.user, ".profile", "# profile\n");
    write_in(world->base.user, "homes/claude/settings.json", "{\n  \"theme\": \"dark\"\n}\n");
}

#endif
