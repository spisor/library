#ifndef RUNTIME_TEST_RUNTIME_TESTING_FIXTURE_H
#define RUNTIME_TEST_RUNTIME_TESTING_FIXTURE_H

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "directory_tree.h"
#include "paths.h"
#include "runtime_testing/home.h"
#include "scratch_directory.h"
#include "spisor/allocation.h"
#include "spisor/file.h"
#include "spisor/json_value.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define HOST "test-host"
#define ENTRY_TEXT "export default {};\n"

static char parent[1024];
static char base[1100];
static char entry_source[1100];
static char forebay_directory[1100];
static char runtime_directory[1100];
static char runtime_binary[1200];

static inline const runtime_testing_definition *testing(void) {
    return (const runtime_testing_definition *)runtime_testing_definition_of();
}

static inline char *joined(const char *left, const char *right) {
    const char *segments[2] = {left, right};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static inline void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
}

/** A parent folder holding an empty base, an entry source and a declared library beside it. */
static inline void start_parent(const char *label) {
    char declaration[1200];
    snprintf(parent, sizeof parent, "%s/runtime-testing-%d-%s", RUNTIME_SCRATCH_PATH, (int)current_process_id(), label);
    start_empty_directory(parent);
    snprintf(base, sizeof base, "%s/home", parent);
    CHECK(spisor_make_directories(base) == SPISOR_OK);
    snprintf(entry_source, sizeof entry_source, "%s/entry.js", parent);
    write_text(entry_source, ENTRY_TEXT);
    snprintf(forebay_directory, sizeof forebay_directory, "%s/forebay", parent);
    CHECK(spisor_make_directories(forebay_directory) == SPISOR_OK);
    snprintf(declaration, sizeof declaration, "%s/library.json", forebay_directory);
    write_text(declaration, "{\"importName\": \"forebay\", \"version\": \"0.3.0\", \"services\": []}");
    snprintf(runtime_directory, sizeof runtime_directory, "%s/rt", parent);
    snprintf(runtime_binary, sizeof runtime_binary, "%s/rt/runtime.dll", parent);
}

static inline void finish_parent(void) {
    CHECK(runtime_remove_tree(parent) == SPISOR_OK);
}

/** A value tree of json, which it deletes. */
static inline spisor_value_tree tree_of(cJSON *json) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    CHECK(spisor_value_of_json(json, &tree) == SPISOR_OK);
    cJSON_Delete(json);
    return tree;
}

/** runtime and the declared forebay-library, the libraries every home here hints. */
static inline void default_libraries(runtime_boot_library libraries[2]) {
    const runtime_boot_library defaults[2] = {{"runtime", "0.2.0", runtime_directory, runtime_binary},
                                              {"forebay-library", NULL, forebay_directory, NULL}};
    memcpy(libraries, defaults, sizeof defaults);
}

/** An argument laying out the default libraries at `at`, held by this process, with `count` installs. */
static inline runtime_testing_home_argument home_argument(const char *at, const runtime_testing_install *installs,
                                                          size_t count, const runtime_boot_library libraries[2]) {
    runtime_testing_home_argument argument = SPISOR_INIT(runtime_testing_home_argument);
    argument.base = at;
    argument.host_id = HOST;
    argument.installs = installs;
    argument.install_count = count;
    argument.libraries = libraries;
    argument.library_count = 2;
    argument.holder = (int64_t)current_process_id();
    return argument;
}

typedef struct laid_out {
    spisor_status status;
    runtime_testing_home_answer answer;
    spisor_refusal refusal;
} laid_out;

static inline laid_out lay_out_argument(const runtime_testing_home_argument *argument) {
    laid_out result;
    memset(&result, 0, sizeof result);
    result.answer.size = sizeof result.answer;
    result.refusal.size = sizeof result.refusal;
    result.status = testing()->lay_out_home(NULL, argument, &result.answer, &result.refusal);
    return result;
}

static inline laid_out lay_out(const char *at, const runtime_testing_install *install, int64_t holder) {
    runtime_boot_library libraries[2];
    runtime_testing_home_argument argument;
    default_libraries(libraries);
    argument = home_argument(at, install, install == NULL ? 0 : 1, libraries);
    argument.holder = holder;
    return lay_out_argument(&argument);
}

static inline void release(laid_out *result) {
    testing()->release_home_answer(NULL, &result->answer);
    testing()->release_refusal(NULL, &result->refusal);
}

static inline int is_empty_folder(const char *path) {
    runtime_directory_listing listing = {NULL, 0};
    int empty = runtime_list_directory(path, &listing) == SPISOR_OK && listing.count == 0;
    runtime_free_directory_listing(&listing);
    return empty;
}

#endif
