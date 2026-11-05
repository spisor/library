#include "proof_fixture.h"

#include "tree_snapshot.h"

#define STEP_COUNT 5

static const char *const STEP_NAMES[STEP_COUNT] = {"install", "status", "update", ("remove " LOADER_ID), "remove forebay-host"};
static const char *const FIRST_LOADER[] = {"1.0.0", NULL};
static const char *const BOTH_LOADERS[] = {"1.0.0", "1.1.0", NULL};

/** What one host left after each step of the lifecycle, and what its status printed. */
typedef struct lifecycle_trees {
    char *tree[STEP_COUNT];
    size_t length[STEP_COUNT];
    char *user_before;
    char *user_after;
    char *status;
} lifecycle_trees;

/** How one host is asked for each step: the executable and whether it takes forebay-host's flags. */
typedef struct host_under_proof {
    const char *name;
    char *executable;
    int takes_flags;
} host_under_proof;

static void append_tree(char **tree, size_t *length, const char *label, const char *directory) {
    char header[1200];
    snprintf(header, sizeof header, "== %s\n", label);
    snapshot_append(tree, length, header, strlen(header));
    if (!spisor_file_exists(directory)) {
        snapshot_append(tree, length, "absent\n", 7);
        return;
    }
    size_t part_length = 0;
    char *part = tree_snapshot_sized(directory, NULL, &part_length);
    snapshot_append(tree, length, part, part_length);
    free(part);
}

#if defined(_WIN32)
static void append_user_path(char **tree, size_t *length) {
    registry_text value;
    read_registry_value(RUNTIME_PATH_ENTRY_TEST_VALUE, &value);
    char line[16600];
    snprintf(line, sizeof line, "== user path %s %ls\n", value.present ? "present" : "absent", value.text);
    snapshot_append(tree, length, line, strlen(line));
}
#endif

static int matches_shape(const char *text, size_t length, const char *shape) {
    size_t at = 0;
    for (; shape[at] != 0; at++) {
        int digit = at < length && text[at] >= '0' && text[at] <= '9';
        if (at >= length || (shape[at] == '9' ? !digit : text[at] != shape[at])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Writes every digit of each ISO time in tree as 0, so two runs a moment apart compare alike.
 *
 * @implNote Only times are masked: installedAt and the like record when a step ran, which no two runs
 * share, and every other byte, ids, versions, paths and file contents, still has to match exactly.
 */
static void mask_times(char *tree, size_t length) {
    static const char *const SHAPE = "9999-99-99T99:99:99";
    for (size_t at = 0; at < length; at++) {
        if (!matches_shape(tree + at, length - at, SHAPE)) {
            continue;
        }
        for (size_t digit = at; digit < length && digit < at + 30 && tree[digit] != 'Z' && tree[digit] != '"'; digit++) {
            tree[digit] = tree[digit] >= '0' && tree[digit] <= '9' ? '0' : tree[digit];
        }
    }
}

/** Every byte the places, the user's home and the user's PATH hold, as one comparable snapshot, times masked. */
static char *world_tree(const host_world *world, size_t *length) {
    char *tree = NULL;
    *length = 0;
    snapshot_append(&tree, length, "", 0);
    append_tree(&tree, length, "user", world->base.user);
    append_tree(&tree, length, "portable", world->base.portable);
    append_tree(&tree, length, "state", world->base.state);
    append_tree(&tree, length, "cache", world->base.cache);
#if defined(_WIN32)
    append_user_path(&tree, length);
#endif
    mask_times(tree, *length);
    return tree;
}

/** Empties every place and the user's home and writes the first catalog again, so both hosts start alike. */
static void reset_world(host_world *world) {
    CHECK(runtime_remove_tree(world->base.portable) == 1 && runtime_remove_tree(world->base.state) == 1 &&
          runtime_remove_tree(world->base.cache) == 1 && runtime_remove_tree(world->base.user) == 1);
#if defined(_WIN32)
    delete_test_path_value();
#endif
    prepare_user(world);
    build_proof_catalog(world, FIRST_LOADER);
}

static int run_step(host_world *world, const host_under_proof *host, program *run, int step) {
    char *index = index_path_of(&world->base);
    const char *flagged_install[] = {"install", "--catalog", index, "--plugin", PROOF_PLUGIN_ID, NULL};
    const char *plain_install[] = {"install", index, PROOF_PLUGIN_ID, NULL};
    const char *status[] = {"status", NULL};
    const char *update[] = {"update", NULL};
    const char *remove_loader[] = {"remove", LOADER_ID, NULL};
    const char *remove_forebay_host[] = {"remove", "forebay-host", NULL};
    const char *const *arguments[STEP_COUNT] = {host->takes_flags ? flagged_install : plain_install, status, update, remove_loader,
                                                remove_forebay_host};
    if (step == 2) {
        build_proof_catalog(world, BOTH_LOADERS);
    }
    int code = proof_run(world, run, host->executable, arguments[step], world->bin);
    free(index);
    return code;
}

/** What install must leave whichever host ran it: the command requested, and no JavaScript runtime or host. */
static void expect_installed_without_javascript(const host_world *world, const char *name) {
    if (!exists_in(world->base.state, "bin/" PROOF_COMMAND EXECUTABLE_ENDING)) {
        fprintf(stderr, "%s did not put the requested command in place\n", name);
        check_failures++;
    }
    if (exists_in(world->base.cache, "runtimes") || exists_in(world->base.cache, "bundles/javascript-host")) {
        fprintf(stderr, "%s installed a JavaScript runtime or host\n", name);
        check_failures++;
    }
}

/** Runs install, status, update and both removes through host, recording the tree each one leaves. */
static void run_lifecycle(host_world *world, const host_under_proof *host, lifecycle_trees *trees) {
    reset_world(world);
    trees->user_before = tree_snapshot(world->base.user, NULL);
    for (int step = 0; step < STEP_COUNT; step++) {
        program run;
        if (run_step(world, host, &run, step) != 0) {
            fprintf(stderr, "%s: %s failed\n", host->name, STEP_NAMES[step]);
            check_failures++;
        }
        if (step == 0) {
            expect_installed_without_javascript(world, host->name);
        }
        if (step == 2 && !exists_in(world->base.cache, "bundles/" LOADER_ID "/1.1.0")) {
            fprintf(stderr, "%s did not fetch the loader's newer version\n", host->name);
            check_failures++;
        }
        if (step == 1) {
            trees->status = copy_text(run.out);
            mask_times(trees->status, strlen(trees->status));
        }
        program_free(&run);
        trees->tree[step] = world_tree(world, &trees->length[step]);
    }
    trees->user_after = tree_snapshot(world->base.user, NULL);
}

static void free_trees(lifecycle_trees *trees) {
    for (int step = 0; step < STEP_COUNT; step++) {
        free(trees->tree[step]);
    }
    free(trees->user_before);
    free(trees->user_after);
    free(trees->status);
}

static size_t first_difference(const char *left, size_t left_length, const char *right, size_t right_length) {
    size_t shorter = left_length < right_length ? left_length : right_length;
    size_t at = 0;
    while (at < shorter && left[at] == right[at]) {
        at++;
    }
    return at;
}

static void print_around(const char *label, const char *tree, size_t length, size_t at) {
    size_t start = at > 400 ? at - 400 : 0;
    size_t end = at + 400 < length ? at + 400 : length;
    fprintf(stderr, "--- %s, bytes %zu to %zu:\n%.*s\n", label, start, end, (int)(end - start), tree + start);
}

static void expect_same_trees(const lifecycle_trees *c_host, const lifecycle_trees *forebay_host) {
    for (int step = 0; step < STEP_COUNT; step++) {
        size_t c_length = c_host->length[step];
        size_t forebay_length = forebay_host->length[step];
        if (c_length == forebay_length && memcmp(c_host->tree[step], forebay_host->tree[step], c_length) == 0) {
            continue;
        }
        size_t at = first_difference(c_host->tree[step], c_length, forebay_host->tree[step], forebay_length);
        fprintf(stderr, "after %s the c host's tree differs from forebay-host's at byte %zu\n", STEP_NAMES[step], at);
        print_around("c host", c_host->tree[step], c_length, at);
        print_around("forebay-host", forebay_host->tree[step], forebay_length, at);
        check_failures++;
    }
}

static void expect_home_restored(const lifecycle_trees *trees, const char *name) {
    if (trees->user_before == NULL || trees->user_after == NULL || strcmp(trees->user_before, trees->user_after) != 0) {
        fprintf(stderr, "%s left the user's home changed:\nbefore:\n%s\nafter:\n%s\n", name, trees->user_before, trees->user_after);
        check_failures++;
    }
}

static void prepare_proof_world(host_world *world) {
    start_world_at(world, "c-host-proof", "lifecycle");
    install_fake_application(world);
    expect_no_javascript_on(world->bin, "the lifecycle PATH");
}

/**
 * A host in C, linking runtime and calling its services in-process with no JavaScript runtime on its PATH,
 * leaves after install, status, update and each remove the tree the native forebay-host leaves for the same
 * local catalog, prints the same status, and both leave the user's home as they found it.
 */
static void a_host_in_c_leaves_the_tree_forebay_host_leaves_with_no_javascript_runtime(void) {
    host_world world;
    prepare_proof_world(&world);
    host_under_proof c_host = {"the c host", copy_text(C_HOST_PATH), 0};
    host_under_proof forebay_host = {"forebay-host", staged_forebay_host(), 1};
    lifecycle_trees c_trees;
    lifecycle_trees forebay_trees;
    memset(&c_trees, 0, sizeof c_trees);
    memset(&forebay_trees, 0, sizeof forebay_trees);
    run_lifecycle(&world, &c_host, &c_trees);
    run_lifecycle(&world, &forebay_host, &forebay_trees);
    expect_same_trees(&c_trees, &forebay_trees);
    check_text(c_trees.status, forebay_trees.status == NULL ? "" : forebay_trees.status, "the same status");
    expect_home_restored(&c_trees, c_host.name);
    expect_home_restored(&forebay_trees, forebay_host.name);
    free_trees(&forebay_trees);
    free_trees(&c_trees);
    free(forebay_host.executable);
    free(c_host.executable);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    a_host_in_c_leaves_the_tree_forebay_host_leaves_with_no_javascript_runtime();
    expect_user_path_unchanged(&guard);
    return check_summary("c_host_proof");
}
