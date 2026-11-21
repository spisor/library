#include "cache_deploy.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#include "cache_place.h"
#include "directory_tree.h"
#include "file_replacement.h"
#include "file_stream.h"
#include "prepared_undo.h"
#include "allocation.h"
#include "cache_refusal.h"
#include "identity.h"
#include "import_name.h"
#include "json_service.h"
#include "process_alive.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define MANIFEST_NAME "plugin.json"
#define IDENTITY_SUFFIX "_identity.json"
#define MANIFEST_SUFFIX ".json"
#define ENTRY_SUFFIX ".js"
#define MODULE_MARKER_NAME "package.json"
#define MODULE_MARKER_TEXT "{\"type\":\"module\"}\n"
#define STAGING_SUFFIX ".tmp"
#define STAGING_RANDOM_DIGITS 16

/** The files a deploy writes into pluginDir, each resolved before the first is written. */
typedef enum destination_index { MANIFEST_DESTINATION, ENTRY_DESTINATION, IDENTITY_DESTINATION, MARKER_DESTINATION,
                                 DESTINATION_COUNT } destination_index;

struct runtime_cache_deployment {
    const char *directory;
    const char *plugin_directory;
    char *resolved_directory;
    char *resolved_plugin_directory;
    const char *identity_id;
    const char *identity_name;
    const char *replaces;
    const char *import_name;
    cJSON *manifest;
    uint8_t *manifest_bytes;
    size_t manifest_length;
    uint8_t *entry_bytes;
    size_t entry_length;
    int has_entry;
    uint8_t *identity_bytes;
    size_t identity_length;
    char *destinations[DESTINATION_COUNT];
    int out_of_memory;
};

typedef runtime_cache_deployment deployment;

/** The names of the files staged in a deploy's staging folder, by destination. */
static const char *const STAGED_NAMES[DESTINATION_COUNT] = {"manifest.json", "entry.js", "identity.json",
                                                            "package.json"};

/** The steps that move a staged deploy into place, in their order. */
typedef enum move_step {
    REMOVE_IDENTITY,
    PLACE_ENTRY,
    MOVE_MANIFEST,
    MOVE_MARKER,
    MOVE_IDENTITY,
    MOVE_STEP_COUNT
} move_step;

/**
 * The deploy's files staged inside pluginDir, each with the path it moves to, all prepared, and every folder the deploy
 * made for pluginDir, pluginDir first, so a failed deploy removes them again.
 */
typedef struct staging {
    spisor_prepared_path *made_folders;
    size_t made_count;
    spisor_prepared_path folder;
    int made_folder;
    spisor_prepared_path staged[DESTINATION_COUNT];
    spisor_prepared_path destinations[DESTINATION_COUNT];
} staging;


static int ends_with_ignoring_case(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (text_length < suffix_length) {
        return 0;
    }
    for (size_t index = 0; index < suffix_length; index++) {
        if (tolower((unsigned char)text[text_length - suffix_length + index]) != suffix[index]) {
            return 0;
        }
    }
    return 1;
}

static const char *text_member(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) && item->valuestring[0] != '\0' ? item->valuestring : NULL;
}

static const char *nonempty(const char *text) {
    return text != NULL && text[0] != '\0' ? text : NULL;
}

static cJSON *refuse(cJSON **refusal, const char *message, const char *fix) {
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

/** Reads the file at path through the binary's allocator, noting in plan when memory ran out. */
static uint8_t *read_noting(deployment *plan, const char *path, size_t *length) {
    spisor_read_outcome outcome = SPISOR_READ_OUT_OF_MEMORY;
    uint8_t *bytes = path == NULL ? NULL : spisor_read_file_reporting(path, length, &outcome);
    plan->out_of_memory = plan->out_of_memory || outcome == SPISOR_READ_OUT_OF_MEMORY;
    return bytes;
}

/** Refuses unless memory ran out, which answers out of memory with no refusal. */
static int refuse_unless_out_of_memory(deployment *plan, cJSON **refusal, const char *message, const char *fix) {
    if (!plan->out_of_memory) {
        refuse(refusal, message, fix);
    }
    return 0;
}

/** Resolves both directories, as cache_plan does, and refuses a pluginDir that is the bundle or lies inside it or around it. */
static int resolve_directories(deployment *plan, cJSON **refusal) {
    plan->resolved_directory = runtime_cache_resolve(plan->directory, NULL, 0);
    plan->resolved_plugin_directory = runtime_cache_resolve(plan->plugin_directory, NULL, 0);
    if (plan->resolved_directory == NULL || plan->resolved_plugin_directory == NULL) {
        return 0;
    }
    int overlap = strcmp(plan->resolved_directory, plan->resolved_plugin_directory) == 0 ||
                  runtime_cache_is_strictly_inside(plan->resolved_directory, plan->resolved_plugin_directory) ||
                  runtime_cache_is_strictly_inside(plan->resolved_plugin_directory, plan->resolved_directory);
    if (overlap) {
        refuse(refusal, "pluginDir and the installed bundle are the same folder or one holds the other",
               "point pluginDir at a folder of its own beside the cache");
        return 0;
    }
    plan->directory = plan->resolved_directory;
    plan->plugin_directory = plan->resolved_plugin_directory;
    return 1;
}

static int read_request(const runtime_cache_deploy_request *request, deployment *plan, cJSON **refusal) {
    plan->directory = nonempty(request->directory);
    plan->plugin_directory = nonempty(request->plugin_directory);
    plan->identity_id = nonempty(request->identity_id);
    plan->identity_name = nonempty(request->identity_name);
    plan->replaces = nonempty(request->replaces);
    runtime_identity parsed;
    if (plan->directory == NULL || plan->plugin_directory == NULL || plan->identity_id == NULL ||
        plan->identity_name == NULL) {
        refuse(refusal, "the call needs a directory, a pluginDir and an identity with an id and a name",
               "pass the installed bundle's directory, the plugins directory, and the identity's id and owner/name");
        return 0;
    }
    if (!runtime_identity_parse_id(plan->identity_id, &parsed)) {
        refuse(refusal, "the identity id is not a valid id", "send an id such as github:<number> or local:<name>");
        return 0;
    }
    return resolve_directories(plan, refusal);
}

static int read_manifest(deployment *plan, cJSON **refusal) {
    const char *segments[1] = {MANIFEST_NAME};
    char *path = runtime_cache_resolve(plan->directory, segments, 1);
    int out_of_memory = 0;
    plan->manifest_bytes = read_noting(plan, path, &plan->manifest_length);
    plan->manifest = plan->manifest_bytes == NULL
                         ? NULL
                         : spisor_parse_json_reporting(plan->manifest_bytes, plan->manifest_length, &out_of_memory);
    plan->out_of_memory = plan->out_of_memory || out_of_memory;
    spisor_memory_release(path);
    if (plan->manifest == NULL || !cJSON_IsObject(plan->manifest)) {
        return refuse_unless_out_of_memory(plan, refusal, "the bundle has no plugin.json that holds a JSON object",
                                           "install the plugin again, or point directory at an installed bundle");
    }
    plan->import_name = text_member(plan->manifest, "importName");
    if (plan->import_name == NULL || !runtime_is_import_name(plan->import_name)) {
        refuse(refusal, "the plugin.json importName is missing or is not a valid import name",
               "use lowercase words joined by single hyphens or dots, for example \"config-ledger\" or \"api.http\", and not a Windows reserved name");
        return 0;
    }
    return 1;
}

static int read_entry(deployment *plan, cJSON **refusal) {
    const cJSON *stated = cJSON_GetObjectItemCaseSensitive(plan->manifest, "entry");
    if (stated == NULL) {
        return 1;
    }
    const char *entry = text_member(plan->manifest, "entry");
    if (entry == NULL) {
        refuse(refusal, "the plugin.json entry is not a non-empty path", "name the entry as a path below the bundle, or leave it out");
        return 0;
    }
    const char *segments[1] = {entry};
    char *path = runtime_cache_resolve(plan->directory, segments, 1);
    int inside = path != NULL && runtime_cache_is_strictly_inside(plan->directory, path);
    int identity_like = ends_with_ignoring_case(entry, IDENTITY_SUFFIX);
    plan->out_of_memory = path == NULL;
    plan->entry_bytes = inside && !identity_like ? read_noting(plan, path, &plan->entry_length) : NULL;
    plan->has_entry = plan->entry_bytes != NULL;
    spisor_memory_release(path);
    if (!plan->has_entry) {
        return refuse_unless_out_of_memory(plan, refusal,
                                           "the plugin.json entry is not a file inside the bundle that can be deployed",
                                           "name an entry below the bundle that exists and is not named like an identity file");
    }
    return 1;
}

/** The destination for suffix inside pluginDir, NULL when it would lie outside or memory runs out, which plan notes. */
static char *destination(deployment *plan, const char *suffix) {
    const char *file_parts[2] = {plan->import_name, suffix};
    char *file_name = runtime_cache_text(file_parts, 2);
    char *path = file_name == NULL ? NULL : runtime_cache_resolve(plan->plugin_directory, (const char *const *)&file_name, 1);
    plan->out_of_memory = plan->out_of_memory || path == NULL;
    if (path != NULL && !runtime_cache_is_strictly_inside(plan->plugin_directory, path)) {
        spisor_memory_release(path);
        path = NULL;
    }
    spisor_memory_release(file_name);
    return path;
}

/** Resolves every destination, 0 when one lies outside pluginDir or memory runs out. */
static int resolve_destinations(deployment *plan) {
    static const char *const suffixes[] = {MANIFEST_SUFFIX, ENTRY_SUFFIX, IDENTITY_SUFFIX};
    for (size_t index = 0; index < sizeof suffixes / sizeof suffixes[0]; index++) {
        plan->destinations[index] = destination(plan, suffixes[index]);
        if (plan->destinations[index] == NULL) {
            return 0;
        }
    }
    const char *segments[1] = {MODULE_MARKER_NAME};
    plan->destinations[MARKER_DESTINATION] = runtime_cache_resolve(plan->plugin_directory, segments, 1);
    plan->out_of_memory = plan->destinations[MARKER_DESTINATION] == NULL;
    return !plan->out_of_memory;
}

static uint8_t *identity_bytes(deployment *plan, size_t *length) {
    cJSON *identity = cJSON_CreateObject();
    int built = identity != NULL && cJSON_AddStringToObject(identity, "id", plan->identity_id) != NULL &&
                cJSON_AddStringToObject(identity, "name", plan->identity_name) != NULL;
    char *text = built ? cJSON_Print(identity) : NULL;
    cJSON_Delete(identity);
    *length = text == NULL ? 0 : strlen(text);
    return (uint8_t *)text;
}

static int exists_at(const deployment *plan, destination_index index) {
    return spisor_probe_file(plan->destinations[index]) == SPISOR_FILE_PRESENT;
}

static char *recorded_id(deployment *plan) {
    size_t length = 0;
    uint8_t *bytes = read_noting(plan, plan->destinations[IDENTITY_DESTINATION], &length);
    int out_of_memory = 0;
    cJSON *identity = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, &out_of_memory);
    const char *id = text_member(identity, "id");
    char *copy = id == NULL ? NULL : runtime_cache_text(&id, 1);
    plan->out_of_memory = plan->out_of_memory || out_of_memory || (id != NULL && copy == NULL);
    cJSON_Delete(identity);
    spisor_memory_release(bytes);
    return copy;
}

/** Refuses to put one plugin over another: code already there with no identity, or a different one, is another plugin's. */
static int is_not_another_plugin(deployment *plan, cJSON **refusal) {
    int has_code = exists_at(plan, MANIFEST_DESTINATION) || exists_at(plan, ENTRY_DESTINATION);
    int has_identity = exists_at(plan, IDENTITY_DESTINATION);
    if (!has_code && !has_identity) {
        return 1;
    }
    char *recorded = recorded_id(plan);
    if (plan->out_of_memory) {
        spisor_memory_release(recorded);
        return 0;
    }
    int same = recorded != NULL && (strcmp(recorded, plan->identity_id) == 0 ||
                                    (plan->replaces != NULL && strcmp(recorded, plan->replaces) == 0));
    if (!same) {
        const char *parts[5] = {plan->import_name, " is already deployed as ", recorded == NULL ? "a plugin with no readable identity" : recorded,
                                ", and this install is ", plan->identity_id};
        char *message = runtime_cache_text(parts, 5);
        const char *fix_parts[5] = {"remove the deployed ", plan->import_name, " first if ", plan->identity_id, " is meant to replace it"};
        char *fix = runtime_cache_text(fix_parts, 5);
        *refusal = message == NULL || fix == NULL ? NULL : runtime_refusal(message, fix);
        spisor_memory_release(message);
        spisor_memory_release(fix);
    }
    spisor_memory_release(recorded);
    return same;
}

/** Notes in plan when status is out of memory: whether status is SPISOR_OK. */
static int succeeded(deployment *plan, spisor_status status) {
    plan->out_of_memory = plan->out_of_memory || status == SPISOR_OUT_OF_MEMORY;
    return status == SPISOR_OK;
}

static int make_folder(deployment *plan, const char *path, spisor_prepared_path *prepared, int *made) {
    spisor_file_presence presence = spisor_probe_file(path);
    spisor_status ready =
        presence == SPISOR_FILE_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY : spisor_prepare_path(path, prepared);
    if (!succeeded(plan, ready)) {
        return 0;
    }
    int made_now = succeeded(plan, spisor_make_directories(path));
    *made = made_now && presence == SPISOR_FILE_MISSING;
    return made_now;
}

#ifdef SPISOR_CACHE_TEST_HOOKS
#include "cache_deploy_hooks.h"

static long moves_allowed = -1;

void runtime_cache_stop_deploys_after(long moves) {
    moves_allowed = moves;
}

/** Whether the deploy may take its next move, counting it, or stops here as a crash would. */
static int may_move(void) {
    if (moves_allowed < 0) {
        return 1;
    }
    if (moves_allowed == 0) {
        return 0;
    }
    moves_allowed--;
    return 1;
}

static unsigned long staging_process_id(void) {
    return moves_allowed < 0 ? (unsigned long)current_process_id() : 0;
}
#else
static int may_move(void) {
    return 1;
}

static unsigned long staging_process_id(void) {
    return (unsigned long)current_process_id();
}
#endif

/** The parent folder of path, NULL at a root or when memory runs out, which plan notes. */
static char *parent_of(deployment *plan, const char *path) {
    const char *segments[1] = {".."};
    char *parent = runtime_cache_resolve(path, segments, 1);
    plan->out_of_memory = plan->out_of_memory || parent == NULL;
    if (parent != NULL && strcmp(parent, path) == 0) {
        spisor_memory_release(parent);
        parent = NULL;
    }
    return parent;
}

/** Prepares the folder at path among the folders this deploy makes, there being room for it. */
static int note_made_folder(deployment *plan, staging *stage, const char *path) {
    spisor_prepared_path *grown =
        spisor_memory_reallocate(stage->made_folders, (stage->made_count + 1) * sizeof *stage->made_folders);
    if (grown == NULL) {
        plan->out_of_memory = 1;
        return 0;
    }
    stage->made_folders = grown;
    memset(&stage->made_folders[stage->made_count], 0, sizeof *stage->made_folders);
    if (!succeeded(plan, spisor_prepare_path(path, &stage->made_folders[stage->made_count]))) {
        return 0;
    }
    stage->made_count++;
    return 1;
}

/** Prepares every missing folder from pluginDir up to the first that exists, then makes pluginDir. */
static int make_plugin_directory(deployment *plan, staging *stage) {
    char *path = runtime_cache_text(&plan->plugin_directory, 1);
    plan->out_of_memory = plan->out_of_memory || path == NULL;
    int noted = path != NULL;
    while (noted && path != NULL) {
        spisor_file_presence presence = spisor_probe_file(path);
        plan->out_of_memory = plan->out_of_memory || presence == SPISOR_FILE_OUT_OF_MEMORY;
        noted = presence != SPISOR_FILE_OUT_OF_MEMORY && (presence != SPISOR_FILE_MISSING ||
                                                          note_made_folder(plan, stage, path));
        char *parent = noted && presence == SPISOR_FILE_MISSING ? parent_of(plan, path) : NULL;
        spisor_memory_release(path);
        path = parent;
        noted = noted && !plan->out_of_memory;
    }
    spisor_memory_release(path);
    return noted && succeeded(plan, spisor_make_directories(plan->plugin_directory));
}

/** The staging folder's name: the manifest's destination, this process's id, and a part no other name took. */
static char *staging_folder_path(deployment *plan) {
    char process[32];
    snprintf(process, sizeof process, ".%lu", staging_process_id());
    const char *parts[2] = {plan->destinations[MANIFEST_DESTINATION], process};
    char *named = runtime_cache_text(parts, 2);
    char *folder = named == NULL ? NULL : spisor_temporary_path_for(named);
    spisor_memory_release(named);
    return folder;
}

static int make_staging_folder(deployment *plan, staging *stage) {
    char *folder = staging_folder_path(plan);
    int made = folder != NULL && make_folder(plan, folder, &stage->folder, &stage->made_folder);
    plan->out_of_memory = plan->out_of_memory || folder == NULL;
    spisor_memory_release(folder);
    return made;
}

static int prepare_move(deployment *plan, staging *stage, size_t index) {
    char *staged = runtime_cache_resolve(stage->folder.path, &STAGED_NAMES[index], 1);
    spisor_status ready = staged == NULL ? SPISOR_OUT_OF_MEMORY : spisor_prepare_path(staged, &stage->staged[index]);
    spisor_memory_release(staged);
    return succeeded(plan, ready) &&
           succeeded(plan, spisor_prepare_path(plan->destinations[index], &stage->destinations[index]));
}

static int prepare_moves(deployment *plan, staging *stage) {
    int prepared = 1;
    for (size_t index = 0; prepared && index < DESTINATION_COUNT; index++) {
        prepared = prepare_move(plan, stage, index);
    }
    return prepared;
}

static int write_staged(deployment *plan, staging *stage, destination_index index, const uint8_t *bytes,
                        size_t length) {
    return succeeded(plan, spisor_replace_file(stage->staged[index].path, bytes, length));
}

/**
 * @implNote The identity is written last, so a staging folder holding identity.json holds every file of the deploy,
 * which is what tells an interrupted deploy that can be finished from one that never began to move.
 */
static int write_staged_files(deployment *plan, staging *stage) {
    const uint8_t *marker = (const uint8_t *)MODULE_MARKER_TEXT;
    return (!plan->has_entry || write_staged(plan, stage, ENTRY_DESTINATION, plan->entry_bytes, plan->entry_length)) &&
           write_staged(plan, stage, MANIFEST_DESTINATION, plan->manifest_bytes, plan->manifest_length) &&
           write_staged(plan, stage, MARKER_DESTINATION, marker, strlen(MODULE_MARKER_TEXT)) &&
           write_staged(plan, stage, IDENTITY_DESTINATION, plan->identity_bytes, plan->identity_length);
}

/** Writes every file into a staging folder inside pluginDir and prepares every move and the clearing. */
static int stage_files(deployment *plan, staging *stage) {
    return make_plugin_directory(plan, stage) && make_staging_folder(plan, stage) && prepare_moves(plan, stage) &&
           write_staged_files(plan, stage);
}

static int move_staged(const staging *stage, destination_index index) {
    return spisor_move_prepared_file(&stage->staged[index], &stage->destinations[index]);
}

static int took_move_step(const deployment *plan, const staging *stage, move_step step) {
    switch (step) {
    case REMOVE_IDENTITY:
        return spisor_remove_prepared_file(&stage->destinations[IDENTITY_DESTINATION]);
    case PLACE_ENTRY:
        return plan->has_entry ? move_staged(stage, ENTRY_DESTINATION)
                               : spisor_remove_prepared_file(&stage->destinations[ENTRY_DESTINATION]);
    case MOVE_MANIFEST:
        return move_staged(stage, MANIFEST_DESTINATION);
    case MOVE_MARKER:
        return move_staged(stage, MARKER_DESTINATION);
    default:
        return move_staged(stage, IDENTITY_DESTINATION);
    }
}

/**
 * The identity goes first and returns last, so a failure part way leaves a plugin with no identity rather than a
 * wrong one, and the staging folder still holding identity.json then names the deploy that was interrupted.
 */
static int move_staged_files(const deployment *plan, const staging *stage, int *interrupted) {
    for (move_step step = REMOVE_IDENTITY; step < MOVE_STEP_COUNT; step++) {
        *interrupted = !may_move();
        if (*interrupted || !took_move_step(plan, stage, step)) {
            return 0;
        }
    }
    *interrupted = !may_move();
    return !*interrupted;
}

/** Removes the staging folder and what is left in it, and, unless deployed, every folder made for pluginDir. */
static void clear_staging(const staging *stage, int deployed) {
    for (size_t index = 0; index < DESTINATION_COUNT; index++) {
        spisor_remove_prepared_file(&stage->staged[index]);
    }
    if (stage->made_folder) {
        spisor_remove_prepared_directory(&stage->folder);
    }
    for (size_t index = 0; !deployed && index < stage->made_count; index++) {
        spisor_remove_prepared_directory(&stage->made_folders[index]);
    }
}

static void release_staging(staging *stage) {
    for (size_t index = 0; index < stage->made_count; index++) {
        spisor_release_prepared_path(&stage->made_folders[index]);
    }
    spisor_memory_release(stage->made_folders);
    spisor_release_prepared_path(&stage->folder);
    for (size_t index = 0; index < DESTINATION_COUNT; index++) {
        spisor_release_prepared_path(&stage->staged[index]);
        spisor_release_prepared_path(&stage->destinations[index]);
    }
}

/**
 * @implNote Every file is written into a staging folder, and every path and wide form the moves and the clearing need
 * is prepared, before the first file is moved into place; the moves allocate nothing, so memory running out leaves
 * nothing behind, and once they begin it cannot run out. See prepared_undo.h.
 */
static int write_files(deployment *plan, cJSON **refusal) {
    staging stage;
    memset(&stage, 0, sizeof stage);
    int interrupted = 0;
    int written = stage_files(plan, &stage) && move_staged_files(plan, &stage, &interrupted);
    if (!interrupted) {
        clear_staging(&stage, written);
    }
    release_staging(&stage);
    if (!written) {
        refuse_unless_out_of_memory(plan, refusal, "the plugin could not be written into pluginDir",
                                    "free the plugin's files or point pluginDir at a writable directory, then try again");
    }
    return written;
}

/** Moves the staged file at index of folder into place when it is still there; 0 only when memory ran out. */
static int finished_move(deployment *plan, const char *folder, destination_index index) {
    char *staged = runtime_cache_resolve(folder, &STAGED_NAMES[index], 1);
    spisor_file_presence presence = staged == NULL ? SPISOR_FILE_OUT_OF_MEMORY : spisor_probe_file(staged);
    spisor_status moved = presence == SPISOR_FILE_PRESENT ? runtime_move_file_over(staged, plan->destinations[index])
                          : presence == SPISOR_FILE_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY
                                                                   : SPISOR_OK;
    spisor_memory_release(staged);
    return succeeded(plan, moved) || moved != SPISOR_OUT_OF_MEMORY;
}

static int removed_destination(deployment *plan, destination_index index) {
    runtime_entry_outcome removed = runtime_remove_file_entry(plan->destinations[index]);
    plan->out_of_memory = plan->out_of_memory || removed == RUNTIME_ENTRY_OUT_OF_MEMORY;
    return removed != RUNTIME_ENTRY_OUT_OF_MEMORY;
}

/** Whether the interrupted deploy's manifest, staged or already moved, names an entry; -1 when memory ran out. */
static int interrupted_names_entry(deployment *plan, const char *folder) {
    char *staged = runtime_cache_resolve(folder, &STAGED_NAMES[MANIFEST_DESTINATION], 1);
    size_t length = 0;
    uint8_t *bytes = staged == NULL ? NULL : read_noting(plan, staged, &length);
    if (bytes == NULL && !plan->out_of_memory) {
        bytes = read_noting(plan, plan->destinations[MANIFEST_DESTINATION], &length);
    }
    int out_of_memory = 0;
    cJSON *manifest = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, &out_of_memory);
    int names_entry = text_member(manifest, "entry") != NULL;
    plan->out_of_memory = plan->out_of_memory || staged == NULL || out_of_memory;
    cJSON_Delete(manifest);
    spisor_memory_release(bytes);
    spisor_memory_release(staged);
    return plan->out_of_memory ? -1 : names_entry;
}

/** Takes the interrupted deploy's remaining moves, in its order; 0 only when memory ran out. */
static int finished_interrupted(deployment *plan, const char *folder) {
    int names_entry = interrupted_names_entry(plan, folder);
    if (names_entry < 0 || !removed_destination(plan, IDENTITY_DESTINATION)) {
        return 0;
    }
    int entry_placed = names_entry ? finished_move(plan, folder, ENTRY_DESTINATION)
                                   : removed_destination(plan, ENTRY_DESTINATION);
    return entry_placed && finished_move(plan, folder, MANIFEST_DESTINATION) &&
           finished_move(plan, folder, MARKER_DESTINATION) && finished_move(plan, folder, IDENTITY_DESTINATION);
}

/**
 * Finishes the deploy interrupted with this staging folder when it holds every file, identity.json written last, and
 * removes the folder; 0 only when memory ran out.
 */
static int settled_staging(deployment *plan, const char *name) {
    const char *segments[1] = {name};
    char *folder = runtime_cache_resolve(plan->plugin_directory, segments, 1);
    char *identity = folder == NULL ? NULL : runtime_cache_resolve(folder, &STAGED_NAMES[IDENTITY_DESTINATION], 1);
    spisor_file_presence presence = identity == NULL ? SPISOR_FILE_OUT_OF_MEMORY : spisor_probe_file(identity);
    plan->out_of_memory = plan->out_of_memory || presence == SPISOR_FILE_OUT_OF_MEMORY;
    int settled = presence != SPISOR_FILE_OUT_OF_MEMORY &&
                  (presence != SPISOR_FILE_PRESENT || finished_interrupted(plan, folder)) &&
                  runtime_remove_tree(folder) != SPISOR_OUT_OF_MEMORY;
    plan->out_of_memory = plan->out_of_memory || !settled;
    spisor_memory_release(identity);
    spisor_memory_release(folder);
    return settled;
}

static int is_hex_digit(char character) {
    return isdigit((unsigned char)character) || (character >= 'a' && character <= 'f');
}

/** The process id a staging folder of this import name is named for, or -1 when name is not one. */
static long staging_owner(const deployment *plan, const char *name) {
    static const char separator[] = MANIFEST_SUFFIX ".";
    size_t import_length = strlen(plan->import_name);
    if (strncmp(name, plan->import_name, import_length) != 0 ||
        strncmp(name + import_length, separator, sizeof separator - 1) != 0) {
        return -1;
    }
    const char *digits = name + import_length + sizeof separator - 1;
    char *end = NULL;
    long owner = isdigit((unsigned char)digits[0]) ? strtol(digits, &end, 10) : -1;
    if (owner < 0 || *end != '.') {
        return -1;
    }
    const char *random = end + 1;
    for (size_t index = 0; index < STAGING_RANDOM_DIGITS; index++) {
        if (!is_hex_digit(random[index])) {
            return -1;
        }
    }
    return strcmp(random + STAGING_RANDOM_DIGITS, STAGING_SUFFIX) == 0 ? owner : -1;
}

static int is_interrupted_staging(const deployment *plan, const runtime_directory_entry *entry) {
    long owner = entry->is_directory ? staging_owner(plan, entry->name) : -1;
    return owner >= 0 && !runtime_process_is_alive((double)owner);
}

/**
 * Finishes or removes every deploy of this import name a process stopped in the middle of, found by its staging folder
 * in pluginDir; 0 only when memory ran out.
 *
 * @implNote A deploy that stopped between its moves leaves a plugin with no identity, which would read as another
 * plugin's code; its staging folder is the evidence of which deploy it was, so it is finished rather than refused.
 */
static int settled_interrupted_deploys(deployment *plan) {
    runtime_directory_listing listing = {NULL, 0};
    spisor_status listed = runtime_list_directory(plan->plugin_directory, &listing);
    plan->out_of_memory = plan->out_of_memory || listed == SPISOR_OUT_OF_MEMORY;
    int settled = listed != SPISOR_OUT_OF_MEMORY;
    for (size_t index = 0; settled && listed == SPISOR_OK && index < listing.count; index++) {
        settled = !is_interrupted_staging(plan, &listing.entries[index]) ||
                  settled_staging(plan, listing.entries[index].name);
    }
    runtime_free_directory_listing(&listing);
    return settled;
}

static int is_deployable(deployment *plan, const runtime_cache_deploy_request *request, cJSON **refusal) {
    if (!read_request(request, plan, refusal) || !read_manifest(plan, refusal) || !read_entry(plan, refusal)) {
        return 0;
    }
    if (!resolve_destinations(plan)) {
        return refuse_unless_out_of_memory(plan, refusal, "the import name would be written outside pluginDir",
                                           "use an import name the schema accepts");
    }
    plan->identity_bytes = identity_bytes(plan, &plan->identity_length);
    return plan->identity_bytes != NULL && settled_interrupted_deploys(plan) && is_not_another_plugin(plan, refusal);
}

void runtime_cache_free_deployment(runtime_cache_deployment *plan) {
    if (plan == NULL) {
        return;
    }
    cJSON_Delete(plan->manifest);
    spisor_memory_release(plan->resolved_directory);
    spisor_memory_release(plan->resolved_plugin_directory);
    spisor_memory_release(plan->manifest_bytes);
    spisor_memory_release(plan->entry_bytes);
    cJSON_free(plan->identity_bytes);
    for (size_t index = 0; index < DESTINATION_COUNT; index++) {
        spisor_memory_release(plan->destinations[index]);
    }
    spisor_memory_release(plan);
}

runtime_cache_deployment *runtime_cache_plan_deployment(const runtime_cache_deploy_request *request,
                                                        cJSON **refusal) {
    deployment *plan = spisor_memory_allocate_zeroed(1, sizeof *plan);
    *refusal = NULL;
    if (plan != NULL && !is_deployable(plan, request, refusal)) {
        runtime_cache_free_deployment(plan);
        plan = NULL;
    }
    return plan;
}

const char *runtime_cache_deployment_import_name(const runtime_cache_deployment *plan) {
    return plan->import_name;
}

int runtime_cache_carry_out_deployment(runtime_cache_deployment *plan, cJSON **refusal) {
    *refusal = NULL;
    return write_files(plan, refusal);
}

int runtime_cache_deploy(const runtime_cache_deploy_request *request, cJSON **refusal) {
    runtime_cache_deployment *plan = runtime_cache_plan_deployment(request, refusal);
    int deployed = plan != NULL && runtime_cache_carry_out_deployment(plan, refusal);
    runtime_cache_free_deployment(plan);
    return deployed;
}
