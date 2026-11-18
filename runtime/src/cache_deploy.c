#include "cache_deploy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cache_place.h"
#include "directory_tree.h"
#include "allocation.h"
#include "cache_refusal.h"
#include "file_reading.h"
#include "json_parse.h"
#include "identity.h"
#include "import_name.h"
#include "json_service.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define MANIFEST_NAME "plugin.json"
#define IDENTITY_SUFFIX "_identity.json"
#define MANIFEST_SUFFIX ".json"
#define ENTRY_SUFFIX ".js"
#define MODULE_MARKER_NAME "package.json"
#define MODULE_MARKER_TEXT "{\"type\":\"module\"}\n"

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

static int read_arguments(const cJSON *argument, deployment *plan, cJSON **refusal) {
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(argument, "identity");
    plan->directory = text_member(argument, "directory");
    plan->plugin_directory = text_member(argument, "pluginDir");
    plan->identity_id = text_member(identity, "id");
    plan->identity_name = text_member(identity, "name");
    plan->replaces = text_member(argument, "replaces");
    runtime_identity parsed;
    if (plan->directory == NULL || plan->plugin_directory == NULL || plan->identity_id == NULL ||
        plan->identity_name == NULL) {
        refuse(refusal, "the argument needs directory, pluginDir and identity with an id and a name",
               "send {\"directory\": \"<installed bundle>\", \"pluginDir\": \"<plugins>\", \"identity\": {\"id\": \"<id>\", \"name\": \"<owner/name>\"}}");
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

static int write_destination(const deployment *plan, destination_index index, const uint8_t *bytes, size_t length) {
    return spisor_replace_file(plan->destinations[index], bytes, length) == SPISOR_OK;
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

static int remove_destination(const deployment *plan, destination_index index) {
    return runtime_remove_file_entry(plan->destinations[index]) == RUNTIME_ENTRY_DONE;
}

/** The identity goes first and returns last, so a failure part way leaves a plugin with no identity rather than a wrong one. */
static int write_files(deployment *plan, cJSON **refusal) {
    int written =
        spisor_make_directories(plan->plugin_directory) && remove_destination(plan, IDENTITY_DESTINATION) &&
        (plan->has_entry || remove_destination(plan, ENTRY_DESTINATION)) &&
        (!plan->has_entry || write_destination(plan, ENTRY_DESTINATION, plan->entry_bytes, plan->entry_length)) &&
        write_destination(plan, MANIFEST_DESTINATION, plan->manifest_bytes, plan->manifest_length) &&
        write_destination(plan, MARKER_DESTINATION, (const uint8_t *)MODULE_MARKER_TEXT, strlen(MODULE_MARKER_TEXT)) &&
        write_destination(plan, IDENTITY_DESTINATION, plan->identity_bytes, plan->identity_length);
    if (!written) {
        refuse_unless_out_of_memory(plan, refusal, "the plugin could not be written into pluginDir",
                                    "free the plugin's files or point pluginDir at a writable directory, then try again");
    }
    return written;
}

static int is_deployable(deployment *plan, const cJSON *argument, cJSON **refusal) {
    if (!read_arguments(argument, plan, refusal) || !read_manifest(plan, refusal) || !read_entry(plan, refusal)) {
        return 0;
    }
    if (!resolve_destinations(plan)) {
        return refuse_unless_out_of_memory(plan, refusal, "the import name would be written outside pluginDir",
                                           "use an import name the schema accepts");
    }
    plan->identity_bytes = identity_bytes(plan, &plan->identity_length);
    return plan->identity_bytes != NULL && is_not_another_plugin(plan, refusal);
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

runtime_cache_deployment *runtime_cache_plan_deployment(const cJSON *argument, cJSON **refusal) {
    deployment *plan = spisor_memory_allocate_zeroed(1, sizeof *plan);
    *refusal = NULL;
    if (plan != NULL && !is_deployable(plan, argument, refusal)) {
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

static cJSON *import_name_answer(const char *import_name) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddStringToObject(answer, "importName", import_name) == NULL) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

cJSON *runtime_cache_deploy_operation(const cJSON *argument, cJSON **refusal) {
    runtime_cache_deployment *plan = runtime_cache_plan_deployment(argument, refusal);
    cJSON *answer = plan == NULL ? NULL : import_name_answer(plan->import_name);
    if (answer != NULL && !runtime_cache_carry_out_deployment(plan, refusal)) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    runtime_cache_free_deployment(plan);
    return answer;
}
