#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_marker.h"
#include "cache_refusal.h"
#include "catalog_build.h"
#include "catalog_fold.h"
#include "catalog_patterns.h"
#include "catalog_semver.h"
#include "catalog_text.h"
#include "host_context.h"
#include "identity.h"
#include "paths.h"
#include "platform_key.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define CATALOG_FILE "catalog.json"
#define FILES_DIRECTORY "files"

static char *label_of(const catalog_input *input) {
    const char *parts[4] = {input->id, "@", input->version, ""};
    char *base = runtime_cache_text(parts, 3);
    if (base == NULL || input->platform == NULL) {
        return base;
    }
    const char *platform_parts[3] = {base, " for ", input->platform};
    char *label = runtime_cache_text(platform_parts, 3);
    free(base);
    return label;
}

static char *shown(const char *text) {
    cJSON *string = cJSON_CreateString(text);
    char *printed = string == NULL ? NULL : cJSON_PrintUnformatted(string);
    cJSON_Delete(string);
    char *copy = printed == NULL ? NULL : runtime_catalog_copy(printed);
    cJSON_free(printed);
    return copy;
}

static int refuse(cJSON **refusal, const char *const *parts, size_t count, const char *fix) {
    *refusal = runtime_cache_refusal(parts, count, fix);
    return 0;
}

static int check_files(const catalog_input *input, const char *label, cJSON **refusal) {
    for (const cJSON *file = input->files->child; file != NULL; file = file->next) {
        if (!runtime_catalog_is_file_key(file->string)) {
            char *key = shown(file->string);
            const char *parts[4] = {label, " lists the file ", key, ", which a catalog refuses"};
            refuse(refusal, parts, 4, "give every file a relative path with / separators and no . or .. segment");
            free(key);
            return 0;
        }
        if (host_equals_ignoring_case(file->string, RUNTIME_INSTALL_MARKER)) {
            const char *parts[4] = {label, " lists the file ", file->string,
                                    ", which the cache would take for its completion marker " RUNTIME_INSTALL_MARKER};
            return refuse(refusal, parts, 4, "rename the file");
        }
    }
    return 1;
}

/** The folder an item's files live in: its id when that is a name, the id's folder, which holds no ":", when it is an id. */
static char *folder_of(const catalog_input *input) {
    runtime_identity identity;
    char *folder = NULL;
    if (runtime_identity_parse_id(input->id, &identity)) {
        runtime_identity_folder_name(input->id, &folder);
        return folder;
    }
    return runtime_cache_text((const char *const *)&input->id, 1);
}

static int names_a_folder(const catalog_input *input) {
    char *folder = folder_of(input);
    free(folder);
    return folder != NULL;
}

static int check_input(const catalog_input *input, cJSON **refusal) {
    char *label = label_of(input);
    int checked = label != NULL;
    if (checked && input->application != NULL && strcmp(input->kind, "host") != 0) {
        const char *parts[2] = {label, " declares an application, which only a host may"};
        checked = refuse(refusal, parts, 2, "give the application to a host, or remove it from this build");
    } else if (checked && !runtime_catalog_is_item_id(input->id)) {
        char *id = shown(input->id);
        const char *parts[2] = {id, " is not an id a catalog accepts"};
        checked = refuse(refusal, parts, 2, "use lowercase letters and digits joined by single hyphens");
        free(id);
    } else if (checked && !names_a_folder(input)) {
        const char *parts[2] = {input->id, " is an id that cannot name a folder"};
        checked = refuse(refusal, parts, 2, "use an id whose folder is portable: a local name in lowercase, without +, = or :");
    } else if (checked && !runtime_catalog_is_version(input->version)) {
        char *version = shown(input->version);
        const char *parts[4] = {input->id, " has the version ", version, ", which a catalog refuses"};
        checked = refuse(refusal, parts, 4, "use a semver version such as 1.0.0, with no trailing dot");
        free(version);
    } else if (checked && input->platform != NULL && !runtime_is_platform_key(input->platform)) {
        const char *parts[2] = {label, " names a platform that is not an <os>-<arch> key"};
        checked = refuse(refusal, parts, 2, "name the platform as Node spells it, for example linux-x64");
    } else if (checked) {
        checked = check_files(input, label, refusal);
    }
    free(label);
    return checked;
}

static int same_text(const char *left, const char *right) {
    return (left == NULL && right == NULL) || (left != NULL && right != NULL && strcmp(left, right) == 0);
}

static int check_consistent(const catalog_inputs *inputs, cJSON **refusal) {
    for (size_t index = 0; index < inputs->count; index++) {
        const catalog_input *input = &inputs->items[index];
        for (size_t earlier = 0; earlier < index; earlier++) {
            const catalog_input *other = &inputs->items[earlier];
            if (strcmp(other->id, input->id) != 0) {
                continue;
            }
            if (strcmp(other->kind, input->kind) != 0) {
                const char *parts[5] = {input->id, " is given both as a ", other->kind, " and as a ", input->kind};
                return refuse(refusal, parts, 5, "give each id one kind");
            }
            if (strcmp(other->version, input->version) == 0 && same_text(other->platform, input->platform)) {
                char *label = label_of(input);
                const char *parts[2] = {label == NULL ? input->id : label, " is given twice"};
                refuse(refusal, parts, 2, "give each version of an item, per platform, once");
                free(label);
                return 0;
            }
        }
    }
    return 1;
}

/** The keys input lists, after those of the same version given for every platform when input is per platform. */
static const char **keys_of(const catalog_inputs *inputs, const catalog_input *input, size_t *count) {
    const catalog_input *shared = NULL;
    for (size_t index = 0; input->platform != NULL && shared == NULL && index < inputs->count; index++) {
        const catalog_input *other = &inputs->items[index];
        if (other->platform == NULL && strcmp(other->id, input->id) == 0 && strcmp(other->version, input->version) == 0) {
            shared = other;
        }
    }
    size_t total = (size_t)cJSON_GetArraySize(input->files) + (shared == NULL ? 0 : (size_t)cJSON_GetArraySize(shared->files));
    const char **keys = calloc(total + 1, sizeof *keys);
    *count = 0;
    for (const cJSON *file = keys == NULL || shared == NULL ? NULL : shared->files->child; file != NULL; file = file->next) {
        keys[(*count)++] = file->string;
    }
    for (const cJSON *file = keys == NULL ? NULL : input->files->child; file != NULL; file = file->next) {
        keys[(*count)++] = file->string;
    }
    return keys;
}

static int check_folded_keys(const catalog_inputs *inputs, cJSON **refusal) {
    for (size_t index = 0; index < inputs->count; index++) {
        const catalog_input *input = &inputs->items[index];
        size_t count = 0;
        const char **keys = keys_of(inputs, input, &count);
        size_t first = 0;
        size_t second = 0;
        int duplicate = keys == NULL ? -1 : runtime_catalog_folded_duplicate(keys, count, &first, &second);
        if (duplicate == 1) {
            char *label = label_of(input);
            const char *parts[6] = {label == NULL ? input->id : label, " lists the files ", keys[first], " and ", keys[second],
                                    ", which differ only by case and would install into one file on Windows and macOS"};
            refuse(refusal, parts, 6, "rename one of the files");
            free(label);
        }
        free(keys);
        if (duplicate != 0) {
            return 0;
        }
    }
    return 1;
}

static int is_unreserved(unsigned char character) {
    return isalnum(character) || strchr("-_.!~*'()", character) != NULL;
}

/** text as encodeURIComponent writes it, each / kept when keep_slashes is set, so each segment of a key is encoded alone. */
static int add_encoded(runtime_catalog_text *text, const char *value, int keep_slashes) {
    char escaped[4];
    for (const unsigned char *character = (const unsigned char *)value; *character != 0; character++) {
        int kept = is_unreserved(*character) || (keep_slashes && *character == '/');
        snprintf(escaped, sizeof escaped, "%%%02X", *character);
        if (!(kept ? runtime_catalog_text_add_character(text, (char)*character) : runtime_catalog_text_add(text, escaped))) {
            return 0;
        }
    }
    return 1;
}

/** files/<folder>/<version>/[<platform>/]<key>, each segment encoded as a url's. */
static char *url_of(const catalog_input *input, const char *folder, const char *key) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    const char *heads[4] = {FILES_DIRECTORY, folder, input->version, input->platform};
    int built = 1;
    for (size_t index = 0; built && index < 4 && heads[index] != NULL; index++) {
        built = add_encoded(&text, heads[index], 0) && runtime_catalog_text_add_character(&text, '/');
    }
    if (!built || !add_encoded(&text, key, 1)) {
        runtime_catalog_text_discard(&text);
        return NULL;
    }
    return runtime_catalog_text_finish(&text);
}

static char *destination_of(const char *out, const catalog_input *input, const char *folder, const char *key) {
    const char *segments[6] = {out, FILES_DIRECTORY, folder, input->version, input->platform == NULL ? key : input->platform,
                               key};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, input->platform == NULL ? 5 : 6);
}

/** The bytes of a files value: the file at its path, or its generated text. */
static uint8_t *bytes_of(const cJSON *file, size_t *length) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(file, HOST_GENERATED_TEXT));
    if (!cJSON_IsObject(file)) {
        return spisor_read_file(file->valuestring, length);
    }
    *length = text == NULL ? 0 : strlen(text);
    uint8_t *bytes = text == NULL ? NULL : malloc(*length + 1);
    if (bytes != NULL) {
        memcpy(bytes, text, *length + 1);
    }
    return bytes;
}

/** Writes file to its place under out, answering its FileRef {url, sha256, size}. */
static cJSON *place_file(const char *out, const catalog_input *input, const char *folder, const cJSON *file,
                         cJSON **refusal) {
    const char *key = file->string;
    const char *source = cJSON_IsString(file) ? file->valuestring : key;
    char *destination = destination_of(out, input, folder, key);
    const char *parent_segments[2] = {destination, ".."};
    char *parent = destination == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, parent_segments, 2);
    size_t length = 0;
    uint8_t *bytes = parent == NULL ? NULL : bytes_of(file, &length);
    char hash[RUNTIME_SHA256_HEX_SIZE];
    int copied = bytes != NULL && spisor_make_directories(parent) == 1 && spisor_replace_file(destination, bytes, length) == 1;
    cJSON *reference = copied ? cJSON_CreateObject() : NULL;
    char *url = reference == NULL ? NULL : url_of(input, folder, key);
    if (reference != NULL) {
        runtime_sha256 sha256;
        runtime_sha256_start(&sha256);
        runtime_sha256_add(&sha256, bytes, length);
        runtime_sha256_finish_hex(&sha256, hash);
        if (url == NULL || cJSON_AddStringToObject(reference, "url", url) == NULL ||
            cJSON_AddStringToObject(reference, "sha256", hash) == NULL ||
            cJSON_AddNumberToObject(reference, "size", (double)length) == NULL) {
            cJSON_Delete(reference);
            reference = NULL;
        }
    } else if (parent != NULL) {
        const char *parts[4] = {source, " could not be copied to ", destination == NULL ? out : destination, ""};
        *refusal = runtime_cache_refusal(parts, 4, "check that the file is readable and --out names a writable folder");
    }
    free(url);
    free(bytes);
    free(parent);
    free(destination);
    return reference;
}

static cJSON *placed_files(const char *out, const catalog_input *input, cJSON **refusal) {
    cJSON *files = cJSON_CreateObject();
    char *folder = files == NULL ? NULL : folder_of(input);
    for (const cJSON *file = folder == NULL ? NULL : input->files->child; file != NULL; file = file->next) {
        cJSON *reference = place_file(out, input, folder, file, refusal);
        if (reference == NULL || !cJSON_AddItemToObject(files, file->string, reference)) {
            cJSON_Delete(reference);
            cJSON_Delete(files);
            files = NULL;
            break;
        }
    }
    if (folder == NULL) {
        cJSON_Delete(files);
        files = NULL;
    }
    free(folder);
    return files;
}

static int set_member(cJSON *item, const char *name, cJSON *value) {
    if (value == NULL) {
        return 0;
    }
    if (cJSON_HasObjectItem(item, name)) {
        return cJSON_ReplaceItemInObjectCaseSensitive(item, name, value);
    }
    return cJSON_AddItemToObject(item, name, value);
}

static cJSON *item_for(cJSON *items, const catalog_input *input) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(items, input->id);
    if (item == NULL) {
        item = cJSON_AddObjectToObject(items, input->id);
        int built = item != NULL && cJSON_AddStringToObject(item, "kind", input->kind) != NULL &&
                    cJSON_AddObjectToObject(item, "versions") != NULL && cJSON_AddStringToObject(item, "latest", input->version) != NULL;
        item = built ? item : NULL;
    }
    int complete = item != NULL && (input->runtime == NULL || set_member(item, "runtime", cJSON_CreateString(input->runtime))) &&
                   (input->application == NULL || set_member(item, "application", cJSON_Duplicate(input->application, 1))) &&
                   (input->install_method == NULL || set_member(item, "installMethod", cJSON_CreateString(input->install_method)));
    return complete ? item : NULL;
}

static int add_version_files(cJSON *item, const catalog_input *input, cJSON *files) {
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(item, "versions");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(versions, input->version);
    version = version != NULL ? version : cJSON_AddObjectToObject(versions, input->version);
    if (version == NULL) {
        return 0;
    }
    if (input->platform == NULL) {
        return set_member(version, "files", files);
    }
    cJSON *platforms = cJSON_GetObjectItemCaseSensitive(version, "platforms");
    platforms = platforms != NULL ? platforms : cJSON_AddObjectToObject(version, "platforms");
    cJSON *platform = platforms == NULL ? NULL : cJSON_CreateObject();
    if (platform == NULL || !cJSON_AddItemToObject(platform, "files", files)) {
        cJSON_Delete(platform);
        return 0;
    }
    return set_member(platforms, input->platform, platform);
}

static int set_latest(cJSON *items) {
    for (cJSON *item = items->child; item != NULL; item = item->next) {
        const char *newest = NULL;
        for (const cJSON *version = cJSON_GetObjectItemCaseSensitive(item, "versions")->child; version != NULL; version = version->next) {
            newest = newest == NULL || runtime_catalog_compare_versions(version->string, newest) > 0 ? version->string : newest;
        }
        if (newest != NULL && !set_member(item, "latest", cJSON_CreateString(newest))) {
            return 0;
        }
    }
    return 1;
}

static cJSON *catalog_of(const catalog_inputs *inputs, const char *out, cJSON **refusal) {
    cJSON *catalog = cJSON_CreateObject();
    cJSON *items = catalog == NULL || cJSON_AddNumberToObject(catalog, "format", 1) == NULL ? NULL
                                                                                           : cJSON_AddObjectToObject(catalog, "items");
    int complete = items != NULL;
    for (size_t index = 0; complete && index < inputs->count; index++) {
        const catalog_input *input = &inputs->items[index];
        cJSON *item = item_for(items, input);
        cJSON *files = item == NULL ? NULL : placed_files(out, input, refusal);
        complete = files != NULL && add_version_files(item, input, files);
    }
    if (!complete || !set_latest(items)) {
        cJSON_Delete(catalog);
        return NULL;
    }
    return catalog;
}

static int write_catalog(const cJSON *catalog, const char *path) {
    char *pretty = spisor_json_stringify_pretty(catalog);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    int written = text != NULL && spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1;
    free(text);
    free(pretty);
    return written;
}

char *host_build_local_catalog(const catalog_inputs *inputs, const char *out, cJSON **refusal) {
    *refusal = NULL;
    for (size_t index = 0; index < inputs->count; index++) {
        if (!check_input(&inputs->items[index], refusal)) {
            return NULL;
        }
    }
    if (!check_consistent(inputs, refusal) || !check_folded_keys(inputs, refusal)) {
        return NULL;
    }
    char *absolute = host_absolute_path(out);
    const char *segments[2] = {absolute, CATALOG_FILE};
    char *path = absolute == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    cJSON *catalog = path == NULL || spisor_make_directories(absolute) != 1 ? NULL : catalog_of(inputs, absolute, refusal);
    if (catalog == NULL || !write_catalog(catalog, path)) {
        if (catalog != NULL || *refusal == NULL) {
            const char *parts[2] = {"the catalog could not be written into ", absolute == NULL ? out : absolute};
            cJSON_Delete(*refusal);
            *refusal = runtime_cache_refusal(parts, 2, "check that --out names a writable folder");
        }
        free(path);
        path = NULL;
    }
    cJSON_Delete(catalog);
    free(absolute);
    return path;
}
