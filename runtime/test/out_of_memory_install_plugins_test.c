#include "install_fixture.h"

#include <stdint.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "allocation.h"
#include "allocation_failure.h"
#include "runtime/install.h"
#include "spisor/json_allocation.h"
#include "timestamp_text.h"

#define CLAUDE "claude-loader"

/**
 * @implNote A repeat is a second install, so only every REPEAT_EVERY-th OUT_OF_MEMORY answer is repeated, while an OK
 * answer is compared with a clean install at every point.
 */
#define REPEAT_EVERY 25

static const char *const PLACES[4] = {"portable", "state", "cache", "user"};
static const char *const ROW_PLUGINS[2] = {"wanted", "other"};
static const char *const UNLISTED_PLUGINS[2] = {"wanted", "ghost"};

static char refused_detail[4096];

static void place_binary(const install_base *base) {
#if defined(_WIN32)
    write_in(base->path, "claude-binary.cmd", "@echo off\r\n");
#else
    char *path = joined(base->path, "claude-binary");
    write_file(path, "#!/bin/sh\n");
    CHECK(chmod(path, 0755) == 0);
    spisor_memory_release(path);
#endif
}

static void copy_tree(const char *from, const char *to) {
    runtime_directory_listing listing;
    CHECK(spisor_make_directories(to) == SPISOR_OK);
    CHECK(runtime_list_directory(from, &listing) == SPISOR_OK);
    for (size_t index = 0; index < listing.count; index++) {
        char *source = joined(from, listing.entries[index].name);
        char *target = joined(to, listing.entries[index].name);
        if (listing.entries[index].is_directory) {
            copy_tree(source, target);
        } else {
            size_t length = 0;
            uint8_t *bytes = spisor_read_file_reporting(source, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
            CHECK(bytes != NULL || length == 0);
            CHECK(spisor_replace_file(target, bytes, length) == SPISOR_OK);
            spisor_memory_release(bytes);
        }
        spisor_memory_release(target);
        spisor_memory_release(source);
    }
    runtime_free_directory_listing(&listing);
}

/** Copies each place between base's root and its pristine folder, in the direction `keep` says. */
static void copy_places(const install_base *base, int keep) {
    char *pristine = joined(base->root, "pristine");
    for (size_t index = 0; index < 4; index++) {
        char *place = joined(base->root, PLACES[index]);
        char *kept = joined(pristine, PLACES[index]);
        if (keep) {
            copy_tree(place, kept);
        } else {
            CHECK(runtime_remove_tree(place) == SPISOR_OK);
            copy_tree(kept, place);
        }
        spisor_memory_release(kept);
        spisor_memory_release(place);
    }
    spisor_memory_release(pristine);
}

/** An installed ecosystem with one loader, its places kept to restore before every attempt. */
static void prepare_world(install_base *base) {
    start_install_base(base, "out-of-memory", "install-plugins");
    list_foundation(base);
    list_plugin(base, "announcer", "1.0.0", "fixture-one");
    list_plugin(base, "wanted", "1.0.0", "fixture-one");
    list_plugin(base, "other", "2.0.0", "fixture-two");
    list_language_host(base, "fixture-two", "native", "1.0.0", "{\"entry\": \"program\", \"runtime\": \"native\"}");
    list_loader(base, CLAUDE, "1.0.0", application_json("claude", "claude-binary", "{}"), "[]", 0, "[]");
    write_catalog(base);
    place_binary(base);
    char *home = joined(base->user, "homes/claude");
    CHECK(spisor_make_directories(home) == SPISOR_OK);
    spisor_memory_release(home);
    cJSON *report = expect_report(install_with(base, "{\"defaults\": [\"announcer\"]}", NULL), "the ecosystem");
    cJSON_Delete(report);
    copy_places(base, 1);
}

static spisor_status install_plugins(const install_base *base, const char *const *plugins) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_variable environment[2] = {{"PATH", base->path}, {"HOME", base->user}};
    runtime_install_plugins_argument argument = SPISOR_INIT(runtime_install_plugins_argument);
    runtime_install_plugins_answer answer;
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    start_empty(&answer, sizeof answer);
    argument.portable = base->portable;
    argument.state = base->state;
    argument.cache = base->cache;
    argument.environment = environment;
    argument.environment_count = 2;
    argument.host = CLAUDE;
    argument.plugins = plugins;
    argument.plugin_count = 2;
    spisor_status status = install->plugins(install->context, &argument, &answer, &refusal);
    status = empty_unless_answered(status, &answer, sizeof answer);
    snprintf(refused_detail, sizeof refused_detail, "%s", refusal.detail == NULL ? "" : refusal.detail);
    install->release_plugins_answer(install->context, &answer);
    install->release_refusal(install->context, &refusal);
    return status;
}

static void add_tree(cJSON *snapshot, const char *root, const char *relative) {
    char *directory = relative[0] == 0 ? joined(root, ".") : joined(root, relative);
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(directory, &listing) == SPISOR_OK);
    for (size_t index = 0; index < listing.count; index++) {
        char child[1024];
        snprintf(child, sizeof child, "%s%s%s", relative, relative[0] == 0 ? "" : "/", listing.entries[index].name);
        if (listing.entries[index].is_directory) {
            cJSON_AddStringToObject(snapshot, child, "(directory)");
            add_tree(snapshot, root, child);
            continue;
        }
        char *path = joined(root, child);
        size_t length = 0;
        uint8_t *content = spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
        size_t normalized_length = 0;
        char *normalized = content == NULL ? NULL : without_timestamps(content, length, &normalized_length);
        cJSON_AddStringToObject(snapshot, child, normalized == NULL ? "(unreadable)" : normalized);
        spisor_memory_release(normalized);
        spisor_memory_release(content);
        spisor_memory_release(path);
    }
    runtime_free_directory_listing(&listing);
    spisor_memory_release(directory);
}

/** Every directory and file of the places, the files' contents without their timestamps. */
static cJSON *snapshot_of(const install_base *base) {
    cJSON *snapshot = cJSON_CreateObject();
    for (size_t index = 0; index < 4; index++) {
        char *place = joined(base->root, PLACES[index]);
        cJSON *tree = cJSON_AddObjectToObject(snapshot, PLACES[index]);
        add_tree(tree, place, "");
        spisor_memory_release(place);
    }
    return snapshot;
}

static int matches(const install_base *base, const cJSON *expected, const char *label, uint64_t attempt) {
    cJSON *snapshot = snapshot_of(base);
    int same = cJSON_Compare(snapshot, expected, 1);
    if (!same) {
        char *printed = cJSON_PrintUnformatted(snapshot);
        fprintf(stderr, "%s after failing allocation %llu differs from a clean run: %s\n", label,
                (unsigned long long)attempt, printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
    }
    cJSON_Delete(snapshot);
    return same;
}

/** Runs the install with allocation attempt failing; 1 when it left nothing live and answered `allowed` or OOM. */
static int fail_once(const install_base *base, const char *const *plugins, allocation_failure_mode mode,
                     uint64_t attempt, spisor_status allowed, spisor_status *status) {
    copy_places(base, 0);
    int64_t baseline = spisor_memory_live_blocks();
    fail_allocation(mode, attempt);
    *status = install_plugins(base, plugins);
    spisor_memory_fail_at(0);
    if (spisor_memory_live_blocks() != baseline) {
        fprintf(stderr, "install's plugins: failing allocation %llu left %lld blocks live\n",
                (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
        return 0;
    }
    if (*status != allowed && *status != SPISOR_OUT_OF_MEMORY) {
        fprintf(stderr, "install's plugins: failing allocation %llu answered %d \"%s\"\n", (unsigned long long)attempt,
                (int)*status, refused_detail);
        return 0;
    }
    return 1;
}

/** A clean run's snapshot and its allocation count, checking it answers `expected` and leaves nothing live. */
static cJSON *clean_run(const install_base *base, const char *const *plugins, spisor_status expected,
                        uint64_t *allocations) {
    copy_places(base, 0);
    int64_t baseline = spisor_memory_live_blocks();
    spisor_memory_fail_at(0);
    CHECK(install_plugins(base, plugins) == expected);
    *allocations = spisor_memory_attempts();
    CHECK(spisor_memory_live_blocks() == baseline);
    return snapshot_of(base);
}

static int installs_as_promised(const install_base *base, const cJSON *clean, allocation_failure_mode mode,
                                uint64_t attempt) {
    spisor_status status = SPISOR_OK;
    int kept = fail_once(base, ROW_PLUGINS, mode, attempt, SPISOR_OK, &status);
    if (kept && status == SPISOR_OK) {
        kept = matches(base, clean, "install's plugins' OK answer", attempt);
    } else if (kept && attempt % REPEAT_EVERY == 0) {
        int64_t baseline = spisor_memory_live_blocks();
        kept = install_plugins(base, ROW_PLUGINS) == SPISOR_OK && spisor_memory_live_blocks() == baseline &&
               matches(base, clean, "install's plugins' repeat", attempt);
    }
    return kept;
}

static void every_install_failure(install_base *base) {
    for (int mode = FAIL_ONE_ALLOCATION; mode < ALLOCATION_FAILURE_MODES; mode++) {
        uint64_t allocations = 0;
        cJSON *clean = clean_run(base, ROW_PLUGINS, SPISOR_OK, &allocations);
        uint64_t attempt = 1;
        for (; attempt <= allocations; attempt++) {
            if (!installs_as_promised(base, clean, (allocation_failure_mode)mode, attempt)) {
                check_failures++;
                break;
            }
        }
        printf("install's plugins%s: %llu allocations, failures 1 to %llu checked\n",
               allocation_failure_mode_name((allocation_failure_mode)mode), (unsigned long long)allocations,
               (unsigned long long)(attempt - 1));
        cJSON_Delete(clean);
    }
}

/** A refusal under any failing allocation refuses as a clean one does, or answers OOM, and writes nothing either way. */
static void every_refusal_failure(install_base *base) {
    static char clean_detail[sizeof refused_detail];
    for (int mode = FAIL_ONE_ALLOCATION; mode < ALLOCATION_FAILURE_MODES; mode++) {
        uint64_t allocations = 0;
        cJSON *clean = clean_run(base, UNLISTED_PLUGINS, SPISOR_REFUSED, &allocations);
        snprintf(clean_detail, sizeof clean_detail, "%s", refused_detail);
        CHECK(strstr(clean_detail, "ghost") != NULL);
        uint64_t attempt = 1;
        for (; attempt <= allocations; attempt++) {
            spisor_status status = SPISOR_OK;
            int kept = fail_once(base, UNLISTED_PLUGINS, (allocation_failure_mode)mode, attempt, SPISOR_REFUSED,
                                 &status) &&
                       (status != SPISOR_REFUSED || strcmp(refused_detail, clean_detail) == 0) &&
                       matches(base, clean, "install's plugins' refusal", attempt);
            if (!kept) {
                fprintf(stderr, "install's plugins refused: failing allocation %llu answered %d \"%s\"\n",
                        (unsigned long long)attempt, (int)status, refused_detail);
                check_failures++;
                break;
            }
        }
        printf("install's plugins refused%s: %llu allocations, failures 1 to %llu checked\n",
               allocation_failure_mode_name((allocation_failure_mode)mode), (unsigned long long)allocations,
               (unsigned long long)(attempt - 1));
        cJSON_Delete(clean);
    }
}

int main(void) {
    install_base base;
    spisor_json_use_library_allocator();
    prepare_world(&base);
    if (check_failures == 0) {
        every_refusal_failure(&base);
        every_install_failure(&base);
    }
    finish_install_base(&base);
    return check_summary("out_of_memory_install_plugins_test");
}
