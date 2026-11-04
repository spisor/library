#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_build.h"
#include "catalog_patterns.h"
#include "catalog_text.h"
#include "directory_tree.h"
#include "host_context.h"
#include "loader.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define PLUGIN_MANIFEST "plugin.json"
#define HOST_MANIFEST "host.json"
#define MODULE_MARKER "package.json"

static char *copy_of(const char *text) {
    return text == NULL ? NULL : runtime_catalog_copy(text);
}

static char *joined(const char *base, const char *rest) {
    const char *segments[2] = {base, rest};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

/** value as JSON.stringify writes it, undefined for none; freed with free. */
static char *shown(const cJSON *value) {
    char *printed = value == NULL ? NULL : cJSON_PrintUnformatted(value);
    char *copy = copy_of(printed == NULL ? "undefined" : printed);
    cJSON_free(printed);
    return copy;
}

static cJSON *refused(const char *const *parts, size_t count, const char *fix) {
    return host_refusal_of(parts, count, fix);
}

static cJSON *read_manifest(const char *path, cJSON **refusal) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    cJSON *manifest = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    free(bytes);
    if (manifest == NULL) {
        const char *parts[2] = {path, " could not be read as JSON"};
        *refusal = refused(parts, 2, "write the manifest as a JSON object");
    } else if (!cJSON_IsObject(manifest)) {
        const char *parts[2] = {path, " does not hold a JSON object"};
        *refusal = refused(parts, 2, "write the manifest as a JSON object");
        cJSON_Delete(manifest);
        manifest = NULL;
    }
    return manifest;
}

static int accepts_id(const cJSON *manifest, const char *path, cJSON **refusal) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(manifest, "id");
    if (cJSON_IsString(id) && runtime_catalog_is_item_id(id->valuestring)) {
        return 1;
    }
    char *named = shown(id);
    const char *parts[4] = {path, " names the id ", named, ", which a catalog refuses"};
    *refusal = refused(parts, 4, "give the manifest an id of lowercase letters and digits joined by single hyphens");
    free(named);
    return 0;
}

static int accepts_version(const cJSON *manifest, const char *path, cJSON **refusal) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(manifest, "version");
    if (cJSON_IsString(version) && runtime_catalog_is_version(version->valuestring)) {
        return 1;
    }
    if (version == NULL) {
        const char *parts[2] = {path, " has no version"};
        *refusal = refused(parts, 2, "add a version to the manifest, for example \"version\": \"1.0.0\"");
        return 0;
    }
    char *named = shown(version);
    const char *parts[4] = {path, " names the version ", named, ", which the catalog's version pattern refuses"};
    *refusal = refused(parts, 4, "give the manifest a semver version such as 1.0.0 or 1.0.0-beta.1, with no trailing dot and no empty part");
    free(named);
    return 0;
}

/** The manifest file_name in directory, refused unless it names an id and a version a catalog accepts. */
static cJSON *identified_manifest(const char *directory, const char *file_name, char **path, cJSON **refusal) {
    *path = joined(directory, file_name);
    if (*path != NULL && !spisor_file_exists(*path)) {
        const char *parts[3] = {directory, " has no ", file_name};
        const char *fix_parts[3] = {"give the directory the ", file_name, " that names its id and version"};
        char *fix = runtime_cache_text(fix_parts, 3);
        *refusal = fix == NULL ? NULL : refused(parts, 3, fix);
        free(fix);
        return NULL;
    }
    cJSON *manifest = *path == NULL ? NULL : read_manifest(*path, refusal);
    if (manifest != NULL && (!accepts_id(manifest, *path, refusal) || !accepts_version(manifest, *path, refusal))) {
        cJSON_Delete(manifest);
        manifest = NULL;
    }
    return manifest;
}

static const char *base_name(const char *key) {
    const char *slash = strrchr(key, '/');
    return slash == NULL ? key : slash + 1;
}

static int equals_ignoring_case(const char *left, const char *right) {
    for (; *left != 0 && *right != 0; left++, right++) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
    }
    return *left == *right;
}

/** The entry the manifest names, refused unless it is a relative path the bundle's own files would not replace. */
static const char *entry_of(const cJSON *manifest, const char *path, cJSON **refusal) {
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(manifest, "entry");
    if (!cJSON_IsString(entry) || !runtime_catalog_is_file_key(entry->valuestring)) {
        char *named = shown(entry);
        const char *parts[4] = {path, " names the entry ", named, ", which is not a relative path inside the bundle"};
        *refusal = refused(parts, 4,
                           "name the entry file in the manifest as a relative path with / separators, for example \"entry\": \"index.js\"");
        free(named);
        return NULL;
    }
    if (equals_ignoring_case(entry->valuestring, PLUGIN_MANIFEST) || equals_ignoring_case(base_name(entry->valuestring), MODULE_MARKER)) {
        const char *parts[4] = {path, " names the entry ", entry->valuestring,
                                ", which the bundle's own " PLUGIN_MANIFEST " or " MODULE_MARKER " would replace"};
        *refusal = refused(parts, 4, "name a script as the entry, for example \"entry\": \"index.js\"");
        return NULL;
    }
    return entry->valuestring;
}

/** The module marker key a .js entry needs beside it, or NULL when its extension says ESM already. */
static char *module_marker_key(const char *entry) {
    size_t length = strlen(entry);
    if (length < 3 || strcmp(entry + length - 3, ".js") != 0) {
        return NULL;
    }
    const char *slash = strrchr(entry, '/');
    size_t directory = slash == NULL ? 0 : (size_t)(slash - entry) + 1;
    char *key = malloc(directory + sizeof MODULE_MARKER);
    if (key != NULL) {
        memcpy(key, entry, directory);
        strcpy(key + directory, MODULE_MARKER);
    }
    return key;
}

static int add_file(cJSON *files, const char *key, const char *path) {
    return cJSON_AddStringToObject(files, key, path) != NULL;
}

static int compare_names(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/** Adds every file under directory as {key: path}, the key relative with / separators. */
static int add_every_file(cJSON *keys, const char *directory, const char *prefix) {
    runtime_directory_listing listing;
    if (!runtime_list_directory(directory, &listing)) {
        return 0;
    }
    int complete = 1;
    for (size_t index = 0; complete && index < listing.count; index++) {
        const runtime_directory_entry *entry = &listing.entries[index];
        char *path = joined(directory, entry->name);
        char *key = malloc(strlen(prefix) + strlen(entry->name) + 2);
        complete = path != NULL && key != NULL;
        if (complete) {
            strcpy(key, prefix);
            strcat(key, prefix[0] == 0 ? "" : "/");
            strcat(key, entry->name);
            complete = entry->is_directory ? add_every_file(keys, path, key) : add_file(keys, key, path);
        }
        free(key);
        free(path);
    }
    runtime_free_directory_listing(&listing);
    return complete;
}

/** Every file under directory, as {key: path} in the order of the keys. */
static cJSON *every_file(const char *directory) {
    cJSON *found = cJSON_CreateObject();
    if (found == NULL || !add_every_file(found, directory, "")) {
        cJSON_Delete(found);
        return NULL;
    }
    int count = cJSON_GetArraySize(found);
    const char **keys = calloc((size_t)count + 1, sizeof *keys);
    cJSON *files = keys == NULL ? NULL : cJSON_CreateObject();
    int index = 0;
    for (const cJSON *file = files == NULL ? NULL : found->child; file != NULL; file = file->next) {
        keys[index++] = file->string;
    }
    qsort(keys, (size_t)index, sizeof *keys, compare_names);
    for (int at = 0; files != NULL && at < index; at++) {
        if (!add_file(files, keys[at], cJSON_GetObjectItemCaseSensitive(found, keys[at])->valuestring)) {
            cJSON_Delete(files);
            files = NULL;
        }
    }
    free(keys);
    cJSON_Delete(found);
    return files;
}

static int append(catalog_inputs *inputs, catalog_input *input) {
    if (inputs->count == inputs->capacity) {
        size_t capacity = inputs->capacity == 0 ? 8 : inputs->capacity * 2;
        catalog_input *grown = realloc(inputs->items, capacity * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        inputs->items = grown;
        inputs->capacity = capacity;
    }
    inputs->items[inputs->count++] = *input;
    return 1;
}

static void free_input(catalog_input *input) {
    free(input->id);
    free(input->version);
    free(input->platform);
    free(input->install_method);
    free(input->runtime);
    cJSON_Delete(input->files);
    cJSON_Delete(input->application);
}

/** Fills id, version and the optional members every manifest may carry, then appends input; 0 when memory runs out. */
static int add_input(catalog_inputs *inputs, catalog_input *input, const cJSON *manifest, const char *platform) {
    const char *install_method = host_text(manifest, "installMethod");
    input->id = copy_of(cJSON_GetObjectItemCaseSensitive(manifest, "id")->valuestring);
    input->version = copy_of(cJSON_GetObjectItemCaseSensitive(manifest, "version")->valuestring);
    input->platform = copy_of(platform);
    input->install_method = install_method == NULL || install_method[0] == 0 ? NULL : copy_of(install_method);
    input->runtime = copy_of(host_text(manifest, "runtime"));
    if (input->id == NULL || input->version == NULL || (platform != NULL && input->platform == NULL) || !append(inputs, input)) {
        free_input(input);
        return 0;
    }
    return 1;
}

static char *absolute(const char *directory) {
    char *path = host_absolute_path(directory);
    const char *segments[1] = {path};
    char *normalized = path == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 1);
    free(path);
    return normalized;
}

static int add_bundled_file(cJSON *files, const char *root, const char *key, cJSON **refusal) {
    char *path = joined(root, key);
    int exists = path != NULL && spisor_file_exists(path);
    if (path != NULL && !exists) {
        const char *parts[4] = {root, " has no ", key, ", which its " PLUGIN_MANIFEST "'s entry calls for"};
        *refusal = refused(parts, 4, "bundle the plugin into this directory first");
    }
    int added = exists && add_file(files, key, path);
    free(path);
    return added;
}

int host_read_plugin_directory(catalog_inputs *inputs, const char *directory, cJSON **refusal) {
    char *root = absolute(directory);
    char *path = NULL;
    cJSON *manifest = root == NULL ? NULL : identified_manifest(root, PLUGIN_MANIFEST, &path, refusal);
    const char *entry = manifest == NULL ? NULL : entry_of(manifest, path, refusal);
    char *marker = entry == NULL ? NULL : module_marker_key(entry);
    catalog_input input = {"plugin", NULL, NULL, NULL, entry == NULL ? NULL : cJSON_CreateObject(), NULL, NULL, NULL};
    int added = input.files != NULL && add_bundled_file(input.files, root, entry, refusal) &&
                (marker == NULL || add_bundled_file(input.files, root, marker, refusal)) &&
                add_file(input.files, PLUGIN_MANIFEST, path);
    if (added) {
        added = add_input(inputs, &input, manifest, NULL);
    } else {
        cJSON_Delete(input.files);
    }
    free(marker);
    cJSON_Delete(manifest);
    free(path);
    free(root);
    return added;
}

static int check_standard(const cJSON *manifest, const char *path, cJSON **refusal) {
    cJSON *issues = runtime_loader_standard_issues(manifest);
    const cJSON *first = cJSON_GetArrayItem(issues, 0);
    if (issues != NULL && first == NULL) {
        cJSON_Delete(issues);
        return 1;
    }
    const char *parts[5] = {path, " has an invalid loader standard at ", host_text(first, "path"), ": ",
                            host_text(first, "detail")};
    const char *fix = host_text(first, "fix");
    *refusal = first == NULL ? NULL : refused(parts, 5, fix == NULL ? "correct the standard" : fix);
    cJSON_Delete(issues);
    return 0;
}

/** A loader holds only its host.json, refused naming whatever else it holds. */
static int only_the_manifest(const cJSON *files, const char *root, cJSON **refusal) {
    runtime_catalog_text beside;
    runtime_catalog_text_start(&beside);
    int others = 0;
    for (const cJSON *file = files->child; file != NULL; file = file->next) {
        if (strcmp(file->string, HOST_MANIFEST) != 0) {
            runtime_catalog_text_add(&beside, others++ == 0 ? "" : ", ");
            runtime_catalog_text_add(&beside, file->string);
        }
    }
    char *listed = runtime_catalog_text_finish(&beside);
    if (others > 0) {
        const char *parts[4] = {root, " is a loader bundle, which holds only " HOST_MANIFEST ", but it also holds ",
                                listed == NULL ? "" : listed, ""};
        *refusal = refused(parts, 4, "bundle the loader with forebay-host bundle-loader into an empty directory");
    }
    free(listed);
    return others == 0;
}

static int names_runtime(const cJSON *manifest, const char *path, cJSON **refusal) {
    if (host_text(manifest, "runtime") != NULL) {
        return 1;
    }
    const char *parts[2] = {path, " names no runtime, which a host's catalog item needs"};
    *refusal = refused(parts, 2, "give host.json \"runtime\": \"native\", \"javascript\" or \"jvm\"");
    return 0;
}

int host_read_host_directory(catalog_inputs *inputs, const char *directory, const char *platform, cJSON **refusal) {
    char *root = absolute(directory);
    char *path = NULL;
    cJSON *manifest = root == NULL ? NULL : identified_manifest(root, HOST_MANIFEST, &path, refusal);
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(manifest, "application");
    int checked = manifest != NULL && names_runtime(manifest, path, refusal) &&
                  (application == NULL ? entry_of(manifest, path, refusal) != NULL : check_standard(manifest, path, refusal));
    catalog_input input = {"host", NULL, NULL, NULL, checked ? every_file(root) : NULL, NULL, NULL, NULL};
    int added = input.files != NULL && (application == NULL || only_the_manifest(input.files, root, refusal));
    if (added && application != NULL) {
        input.application = cJSON_Duplicate(application, 1);
        added = input.application != NULL;
    }
    if (added) {
        added = add_input(inputs, &input, manifest, platform);
    } else {
        cJSON_Delete(input.files);
        cJSON_Delete(input.application);
    }
    cJSON_Delete(manifest);
    free(path);
    free(root);
    return added;
}

int host_read_library_directory(catalog_inputs *inputs, const char *id, const char *directory, const char *platform,
                                cJSON **refusal) {
    char *root = absolute(directory);
    char *library = root == NULL ? NULL : joined(root, "library.json");
    char *package = root == NULL ? NULL : joined(root, "package.json");
    const char *path = library != NULL && spisor_file_exists(library) ? library
                       : package != NULL && spisor_file_exists(package) ? package
                                                                        : NULL;
    if (root != NULL && path == NULL) {
        const char *parts[2] = {root, " has neither library.json nor package.json"};
        *refusal = refused(parts, 2, "give the library directory a manifest naming its version");
    }
    cJSON *manifest = path == NULL ? NULL : read_manifest(path, refusal);
    int versioned = manifest != NULL && accepts_version(manifest, path, refusal);
    catalog_input input = {"library", NULL, NULL, NULL, versioned ? every_file(root) : NULL, NULL, NULL, NULL};
    int added = input.files != NULL;
    if (added) {
        cJSON_ReplaceItemInObjectCaseSensitive(manifest, "id", cJSON_CreateString(id));
        if (!cJSON_HasObjectItem(manifest, "id")) {
            cJSON_AddStringToObject(manifest, "id", id);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(manifest, "installMethod");
        cJSON_DeleteItemFromObjectCaseSensitive(manifest, "runtime");
        added = add_input(inputs, &input, manifest, platform);
    }
    cJSON_Delete(manifest);
    free(package);
    free(library);
    free(root);
    return added;
}

void host_free_catalog_inputs(catalog_inputs *inputs) {
    for (size_t index = 0; index < inputs->count; index++) {
        free_input(&inputs->items[index]);
    }
    free(inputs->items);
    memset(inputs, 0, sizeof *inputs);
}
