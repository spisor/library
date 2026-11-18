#include "install_fixture.h"

#include <stdint.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "allocation.h"
#include "json_allocation.h"
#include "runtime/install.h"
#include "timestamp_text.h"

#define BINARY "claude-binary"

/**
 * @implNote A repeat is a second install, so only every REPEAT_EVERY-th OUT_OF_MEMORY answer is repeated, while an OK
 * answer is compared with a clean install at every point. A step that writes before it finishes spans a stretch of
 * failing allocations, each far longer than this spacing.
 */
#define REPEAT_EVERY 50

static const char *const DEFAULT_PLUGINS[2] = {"announcer", "viewer"};
static const char *const REQUESTED_PLUGINS[1] = {"wanted"};

static void place_binary(const install_base *base) {
#if defined(_WIN32)
    write_in(base->path, BINARY ".cmd", "@echo off\r\n");
#else
    char *path = joined(base->path, BINARY);
    write_file(path, "#!/bin/sh\n");
    CHECK(chmod(path, 0755) == 0);
    spisor_memory_release(path);
#endif
}

/** The foundation, two default plugins, one requested plugin and a loader whose application is on PATH. */
static void prepare_ecosystem(install_base *base) {
    start_install_base(base, "out-of-memory", "ecosystem");
    list_foundation(base);
    list_plugin(base, "announcer", "1.0.0", "native");
    list_plugin(base, "viewer", "1.0.0", "native");
    list_plugin(base, "wanted", "1.0.0", "native");
    list_loader(base, "claude-loader", "1.0.0", application_with_hook_entry("claude", BINARY, "{}"), "[]", 0, "[]");
    write_catalog(base);
    place_binary(base);
}

/** Empties every place an install writes, keeping the catalog and PATH, so each attempt starts as the first did. */
static void fresh_places(const install_base *base) {
    CHECK(runtime_remove_tree(base->portable) == 1 && runtime_remove_tree(base->state) == 1 &&
          runtime_remove_tree(base->cache) == 1 && runtime_remove_tree(base->user) == 1);
    write_in(base->user, ".profile", "# profile\n");
    char *homes = joined(base->user, "homes/claude");
    CHECK(spisor_make_directories(homes) == 1);
    spisor_memory_release(homes);
}

static void environment_of_base(const install_base *base, runtime_variable environment[4]) {
    environment[0].name = "PATH";
    environment[0].value = base->path;
    environment[1].name = "HOME";
    environment[1].value = base->user;
    environment[2].name = "USERPROFILE";
    environment[2].value = base->user;
    environment[3].name = "SHELL";
    environment[3].value = "/bin/sh";
}

static spisor_status install_ecosystem(const install_base *base) {
    const runtime_install_definition *install = runtime_install_definition_of();
    char *index = index_path_of(base);
    runtime_catalog_source source;
    runtime_variable environment[4];
    runtime_install_ecosystem_argument argument = SPISOR_INIT(runtime_install_ecosystem_argument);
    runtime_install_ecosystem_answer answer = SPISOR_INIT(runtime_install_ecosystem_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    memset(&source, 0, sizeof source);
    source.kind = RUNTIME_CATALOG_LOCAL;
    source.location = index;
    environment_of_base(base, environment);
    argument.sources = &source;
    argument.source_count = 1;
    argument.portable = base->portable;
    argument.state = base->state;
    argument.cache = base->cache;
    argument.environment = environment;
    argument.environment_count = 4;
    argument.defaults = DEFAULT_PLUGINS;
    argument.default_count = 2;
    argument.requested = REQUESTED_PLUGINS;
    argument.requested_count = 1;
    spisor_status status = install->ecosystem(install->context, &argument, &answer, &refusal);
    install->release_ecosystem_answer(install->context, &answer);
    install->release_refusal(install->context, &refusal);
    spisor_memory_release(index);
    return status;
}

static int is_registration(const char *relative) {
    size_t length = strlen(relative);
    return strncmp(relative, "state/hosts/", 12) == 0 && strchr(relative + 12, '/') == NULL && length > 5 &&
           strcmp(relative + length - 5, ".json") == 0;
}

/** The position in `changes` of the record numbered `sequence`, -1 for none. */
static int ordinal_of(const cJSON *changes, double sequence) {
    int index = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, changes) {
        if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(entry, "sequence")) == sequence) {
            return index;
        }
        index++;
    }
    return -1;
}

/**
 * Numbers every change record by its position, and its parent by the parent's position, dropping nextSequence.
 *
 * @implNote A withdrawn number is never handed out again, so a repeat that completes an install memory stopped may
 * number its records differently from a clean one; it is their content, order and family that must match.
 */
static void renumber_changes(cJSON *registration) {
    cJSON *changes = cJSON_GetObjectItemCaseSensitive(registration, "changes");
    cJSON *entry;
    int index = 0;
    cJSON_DeleteItemFromObjectCaseSensitive(registration, "nextSequence");
    cJSON_ArrayForEach(entry, changes) {
        cJSON *parent = cJSON_GetObjectItemCaseSensitive(entry, "parent");
        if (parent != NULL) {
            cJSON_SetNumberValue(parent, ordinal_of(changes, cJSON_GetNumberValue(parent)));
        }
    }
    cJSON_ArrayForEach(entry, changes) {
        cJSON *sequence = cJSON_GetObjectItemCaseSensitive(entry, "sequence");
        if (sequence != NULL) {
            cJSON_SetNumberValue(sequence, index);
        }
        index++;
    }
}

/** A registration's text with its change records renumbered, or `text` itself when it is no JSON object. */
static char *renumbered(char *text) {
    cJSON *registration = cJSON_Parse(text);
    char *printed;
    if (!cJSON_IsObject(registration)) {
        cJSON_Delete(registration);
        return text;
    }
    renumber_changes(registration);
    printed = cJSON_Print(registration);
    cJSON_Delete(registration);
    CHECK(printed != NULL);
    spisor_memory_release(text);
    size_t length = strlen(printed) + 1;
    text = spisor_memory_allocate(length);
    CHECK(text != NULL);
    memcpy(text, printed, length);
    cJSON_free(printed);
    return text;
}

static void add_file(cJSON *snapshot, const char *path, const char *relative) {
    size_t length = 0;
    uint8_t *content = spisor_read_file(path, &length);
    size_t normalized_length = 0;
    char *normalized = content == NULL ? NULL : without_timestamps(content, length, &normalized_length);
    if (normalized != NULL && is_registration(relative)) {
        normalized = renumbered(normalized);
    }
    cJSON_AddStringToObject(snapshot, relative, normalized == NULL ? "(unreadable)" : normalized);
    spisor_memory_release(normalized);
    spisor_memory_release(content);
}

static void add_tree(cJSON *snapshot, const char *root, const char *relative) {
    char *directory = relative[0] == 0 ? joined(root, ".") : joined(root, relative);
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(directory, &listing) == 1);
    for (size_t index = 0; index < listing.count; index++) {
        char child[1024];
        snprintf(child, sizeof child, "%s%s%s", relative, relative[0] == 0 ? "" : "/", listing.entries[index].name);
        if (listing.entries[index].is_directory) {
            cJSON_AddStringToObject(snapshot, child, "(directory)");
            add_tree(snapshot, root, child);
        } else {
            char *path = joined(root, child);
            add_file(snapshot, path, child);
            spisor_memory_release(path);
        }
    }
    runtime_free_directory_listing(&listing);
    spisor_memory_release(directory);
}

/** Every directory and file the install wrote under base, the files' contents without their timestamps. */
static cJSON *snapshot_of(const install_base *base) {
    cJSON *snapshot = cJSON_CreateObject();
    add_tree(snapshot, base->root, "");
    return snapshot;
}

static void report_member(const char *label, const char *name, const cJSON *member) {
    fprintf(stderr, "  %s %s: %s\n", label, name, member == NULL ? "(absent)" : member->valuestring);
}

/** Whether actual holds exactly what expected holds, naming each path that differs when not. */
static int same_snapshot(const cJSON *expected, const cJSON *actual, const char *label, uint64_t attempt) {
    int same = 1;
    for (const cJSON *member = expected->child; member != NULL; member = member->next) {
        const cJSON *other = cJSON_GetObjectItemCaseSensitive(actual, member->string);
        if (other == NULL || strcmp(other->valuestring, member->valuestring) != 0) {
            fprintf(stderr, "%s after failing allocation %llu differs from a clean install at %s\n", label,
                    (unsigned long long)attempt, member->string);
            report_member("clean", member->string, member);
            report_member("this", member->string, other);
            same = 0;
        }
    }
    for (const cJSON *member = actual->child; member != NULL; member = member->next) {
        if (!cJSON_HasObjectItem(expected, member->string)) {
            fprintf(stderr, "%s after failing allocation %llu wrote %s, which a clean install does not\n", label,
                    (unsigned long long)attempt, member->string);
            same = 0;
        }
    }
    return same;
}

static int matches_clean(const install_base *base, const cJSON *clean, const char *label, uint64_t attempt) {
    cJSON *snapshot = snapshot_of(base);
    int same = same_snapshot(clean, snapshot, label, attempt);
    cJSON_Delete(snapshot);
    return same;
}

/** What a clean install leaves, and how many allocations it makes, checking it answers OK and leaves nothing live. */
static cJSON *clean_install(const install_base *base, uint64_t *allocations) {
    fresh_places(base);
    int64_t baseline = spisor_memory_live_blocks();
    spisor_memory_fail_at(0);
    CHECK(install_ecosystem(base) == SPISOR_OK);
    *allocations = spisor_memory_attempts();
    CHECK(spisor_memory_live_blocks() == baseline);
    return snapshot_of(base);
}

/** Runs the install with allocation attempt failing; 1 when it left nothing live and answered OK or OUT_OF_MEMORY. */
static int fail_once(const install_base *base, uint64_t attempt, spisor_status *status) {
    int64_t baseline = spisor_memory_live_blocks();
    spisor_memory_fail_at(attempt);
    *status = install_ecosystem(base);
    spisor_memory_fail_at(0);
    if (spisor_memory_live_blocks() != baseline) {
        fprintf(stderr, "ecosystem: failing allocation %llu left %lld blocks live\n", (unsigned long long)attempt,
                (long long)(spisor_memory_live_blocks() - baseline));
        return 0;
    }
    if (*status != SPISOR_OK && *status != SPISOR_OUT_OF_MEMORY) {
        fprintf(stderr, "ecosystem: failing allocation %llu answered %d\n", (unsigned long long)attempt, (int)*status);
        return 0;
    }
    return 1;
}

static int repeats_cleanly(const install_base *base, const cJSON *clean, uint64_t attempt) {
    int64_t baseline = spisor_memory_live_blocks();
    spisor_status status = install_ecosystem(base);
    if (status != SPISOR_OK || spisor_memory_live_blocks() != baseline) {
        fprintf(stderr, "ecosystem: the repeat after failing allocation %llu answered %d and left %lld blocks live\n",
                (unsigned long long)attempt, (int)status, (long long)(spisor_memory_live_blocks() - baseline));
        return 0;
    }
    return matches_clean(base, clean, "ecosystem's repeat", attempt);
}

/**
 * Whether failing allocation attempt behaves as install.h promises: nothing left live, OK only when the install is
 * as complete as a clean one, and, where sampled, a repeat that completes it.
 */
static int fails_as_promised(const install_base *base, const cJSON *clean, uint64_t attempt) {
    fresh_places(base);
    spisor_status status = SPISOR_OK;
    int kept = fail_once(base, attempt, &status);
    if (kept && status == SPISOR_OK) {
        kept = matches_clean(base, clean, "ecosystem's OK answer", attempt);
    } else if (kept && attempt % REPEAT_EVERY == 0) {
        kept = repeats_cleanly(base, clean, attempt);
    }
    return kept;
}

/** Fails the ecosystem install allocation by allocation, between first and last when given. */
static void every_ecosystem_failure(uint64_t first, uint64_t last) {
    install_base base;
    prepare_ecosystem(&base);
    uint64_t allocations = 0;
    cJSON *clean = clean_install(&base, &allocations);
    if (last == 0 || last > allocations) {
        last = allocations;
    }
    for (uint64_t attempt = first; attempt <= last; attempt++) {
        if (!fails_as_promised(&base, clean, attempt)) {
            check_failures++;
            break;
        }
    }
    printf("install's ecosystem: %llu allocations, failures %llu to %llu checked\n", (unsigned long long)allocations,
           (unsigned long long)first, (unsigned long long)last);
    cJSON_Delete(clean);
    finish_install_base(&base);
}

int main(int argc, char **argv) {
    spisor_json_use_library_allocator();
    uint64_t first = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    uint64_t last = argc > 2 ? strtoull(argv[2], NULL, 10) : 0;
    every_ecosystem_failure(first == 0 ? 1 : first, last);
    return check_summary("out_of_memory_ecosystem_test");
}
