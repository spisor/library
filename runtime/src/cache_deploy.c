#include "cache_deploy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cache_place.h"
#include "directory_tree.h"
#include "cache_refusal.h"
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

typedef struct deployment {
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
} deployment;

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
    plan->manifest_bytes = path == NULL ? NULL : spisor_read_file(path, &plan->manifest_length);
    plan->manifest = plan->manifest_bytes == NULL ? NULL : spisor_parse_json(plan->manifest_bytes, plan->manifest_length);
    free(path);
    if (plan->manifest == NULL || !cJSON_IsObject(plan->manifest)) {
        refuse(refusal, "the bundle has no plugin.json that holds a JSON object", "install the plugin again, or point directory at an installed bundle");
        return 0;
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
    plan->entry_bytes = inside && !identity_like ? spisor_read_file(path, &plan->entry_length) : NULL;
    plan->has_entry = plan->entry_bytes != NULL;
    free(path);
    if (!plan->has_entry) {
        refuse(refusal, "the plugin.json entry is not a file inside the bundle that can be deployed",
               "name an entry below the bundle that exists and is not named like an identity file");
    }
    return plan->has_entry;
}

static char *destination(const deployment *plan, const char *suffix) {
    const char *file_parts[2] = {plan->import_name, suffix};
    char *file_name = runtime_cache_text(file_parts, 2);
    char *path = file_name == NULL ? NULL : runtime_cache_resolve(plan->plugin_directory, (const char *const *)&file_name, 1);
    if (path != NULL && !runtime_cache_is_strictly_inside(plan->plugin_directory, path)) {
        free(path);
        path = NULL;
    }
    free(file_name);
    return path;
}

static int write_destination(const deployment *plan, const char *suffix, const uint8_t *bytes, size_t length) {
    char *path = destination(plan, suffix);
    int written = path != NULL && spisor_replace_file(path, bytes, length);
    free(path);
    return written;
}

static int destinations_are_inside(const deployment *plan) {
    static const char *const suffixes[] = {MANIFEST_SUFFIX, ENTRY_SUFFIX, IDENTITY_SUFFIX};
    for (size_t index = 0; index < sizeof suffixes / sizeof suffixes[0]; index++) {
        char *path = destination(plan, suffixes[index]);
        int inside = path != NULL;
        free(path);
        if (!inside) {
            return 0;
        }
    }
    return 1;
}

static uint8_t *identity_bytes(const deployment *plan, size_t *length) {
    cJSON *identity = cJSON_CreateObject();
    int built = identity != NULL && cJSON_AddStringToObject(identity, "id", plan->identity_id) != NULL &&
                cJSON_AddStringToObject(identity, "name", plan->identity_name) != NULL;
    char *text = built ? cJSON_Print(identity) : NULL;
    cJSON_Delete(identity);
    *length = text == NULL ? 0 : strlen(text);
    return (uint8_t *)text;
}

static int write_identity(const deployment *plan) {
    size_t length = 0;
    uint8_t *bytes = identity_bytes(plan, &length);
    int written = bytes != NULL && write_destination(plan, IDENTITY_SUFFIX, bytes, length);
    cJSON_free(bytes);
    return written;
}

static int exists_at(const deployment *plan, const char *suffix) {
    char *path = destination(plan, suffix);
    int exists = path != NULL && spisor_file_exists(path);
    free(path);
    return exists;
}

static char *recorded_id(const deployment *plan) {
    char *path = destination(plan, IDENTITY_SUFFIX);
    size_t length = 0;
    uint8_t *bytes = path == NULL ? NULL : spisor_read_file(path, &length);
    cJSON *identity = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    const char *id = text_member(identity, "id");
    char *copy = id == NULL ? NULL : runtime_cache_text(&id, 1);
    cJSON_Delete(identity);
    free(bytes);
    free(path);
    return copy;
}

/** Refuses to put one plugin over another: code already there with no identity, or a different one, is another plugin's. */
static int is_not_another_plugin(const deployment *plan, cJSON **refusal) {
    int has_code = exists_at(plan, MANIFEST_SUFFIX) || exists_at(plan, ENTRY_SUFFIX);
    int has_identity = exists_at(plan, IDENTITY_SUFFIX);
    if (!has_code && !has_identity) {
        return 1;
    }
    char *recorded = recorded_id(plan);
    int same = recorded != NULL && (strcmp(recorded, plan->identity_id) == 0 ||
                                    (plan->replaces != NULL && strcmp(recorded, plan->replaces) == 0));
    if (!same) {
        const char *parts[5] = {plan->import_name, " is already deployed as ", recorded == NULL ? "a plugin with no readable identity" : recorded,
                                ", and this install is ", plan->identity_id};
        char *message = runtime_cache_text(parts, 5);
        const char *fix_parts[5] = {"remove the deployed ", plan->import_name, " first if ", plan->identity_id, " is meant to replace it"};
        char *fix = runtime_cache_text(fix_parts, 5);
        *refusal = message == NULL || fix == NULL ? NULL : runtime_refusal(message, fix);
        free(message);
        free(fix);
    }
    free(recorded);
    return same;
}

static int remove_destination(const deployment *plan, const char *suffix) {
    char *path = destination(plan, suffix);
    int gone = path != NULL && runtime_remove_file_entry(path) == RUNTIME_ENTRY_DONE;
    free(path);
    return gone;
}

static int write_module_marker(const deployment *plan) {
    const char *segments[1] = {MODULE_MARKER_NAME};
    char *path = runtime_cache_resolve(plan->plugin_directory, segments, 1);
    int written = path != NULL && spisor_replace_file(path, (const uint8_t *)MODULE_MARKER_TEXT, strlen(MODULE_MARKER_TEXT));
    free(path);
    return written;
}

/** The identity goes first and returns last, so a failure part way leaves a plugin with no identity rather than a wrong one. */
static int write_files(const deployment *plan, cJSON **refusal) {
    int written = spisor_make_directories(plan->plugin_directory) && remove_destination(plan, IDENTITY_SUFFIX) &&
                  (plan->has_entry || remove_destination(plan, ENTRY_SUFFIX)) &&
                  (!plan->has_entry || write_destination(plan, ENTRY_SUFFIX, plan->entry_bytes, plan->entry_length)) &&
                  write_destination(plan, MANIFEST_SUFFIX, plan->manifest_bytes, plan->manifest_length) &&
                  write_module_marker(plan) && write_identity(plan);
    if (!written) {
        refuse(refusal, "the plugin could not be written into pluginDir",
               "free the plugin's files or point pluginDir at a writable directory, then try again");
    }
    return written;
}

static cJSON *import_name_answer(const deployment *plan) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddStringToObject(answer, "importName", plan->import_name) == NULL) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

static int is_deployable(deployment *plan, const cJSON *argument, cJSON **refusal) {
    if (!read_arguments(argument, plan, refusal) || !read_manifest(plan, refusal) || !read_entry(plan, refusal)) {
        return 0;
    }
    if (!destinations_are_inside(plan)) {
        refuse(refusal, "the import name would be written outside pluginDir", "use an import name the schema accepts");
        return 0;
    }
    return is_not_another_plugin(plan, refusal);
}

static cJSON *deploy(const cJSON *argument, cJSON **refusal) {
    deployment plan;
    memset(&plan, 0, sizeof plan);
    cJSON *answer = NULL;
    if (is_deployable(&plan, argument, refusal) && write_files(&plan, refusal)) {
        answer = import_name_answer(&plan);
    }
    cJSON_Delete(plan.manifest);
    free(plan.resolved_directory);
    free(plan.resolved_plugin_directory);
    free(plan.manifest_bytes);
    free(plan.entry_bytes);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_cache_deploy(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, deploy);
}
