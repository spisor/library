#include "cache_plan.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cache_marker.h"
#include "cache_place.h"
#include "cache_refusal.h"
#include "platform_key.h"
#include "spisor/json_write.h"

#define LARGEST_EXACT_INTEGER 9007199254740991.0
#define ITEM_SHAPE_FIX "pass the item as catalog.read answered it"

typedef struct version_files {
    const cJSON *shared;
    const cJSON *own;
} version_files;

static int refuse(cJSON **refusal, const char *const *parts, size_t count, const char *fix) {
    *refusal = runtime_cache_refusal(parts, count, fix);
    return 0;
}

static int is_absent(const cJSON *value) {
    return value == NULL || cJSON_IsNull(value);
}

static int check_platform_shape(const char *platform, cJSON **refusal) {
    if (runtime_is_platform_key(platform)) {
        return 1;
    }
    const char *parts[2] = {"a platform key is <os>-<arch> as Node spells them, for example linux-x64, not ", platform};
    return refuse(refusal, parts, 2, "pass the platform key the catalog names its builds by, or omit it");
}

static const cJSON *entry_of(const cJSON *item, const char *version, cJSON **refusal) {
    const cJSON *versions = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "versions") : NULL;
    const cJSON *entry = cJSON_IsObject(versions) ? cJSON_GetObjectItemCaseSensitive(versions, version) : NULL;
    if (cJSON_IsObject(entry)) {
        return entry;
    }
    const char *parts[2] = {"the catalog lists no version ", version};
    refuse(refusal, parts, 2, "install a version the catalog lists");
    return NULL;
}

static int refuse_shape(const char *member, const char *version, cJSON **refusal) {
    const char *parts[5] = {"the ", member, " of version ", version, " are not an object"};
    return refuse(refusal, parts, 5, ITEM_SHAPE_FIX);
}

static int platform_files(const cJSON *entry, const char *version, const char *platform, version_files *files,
                          cJSON **refusal) {
    const cJSON *platforms = cJSON_GetObjectItemCaseSensitive(entry, "platforms");
    if (is_absent(platforms)) {
        return 1;
    }
    if (!cJSON_IsObject(platforms)) {
        return refuse_shape("platforms", version, refusal);
    }
    const cJSON *built = cJSON_GetObjectItemCaseSensitive(platforms, platform);
    if (built == NULL) {
        const char *parts[2] = {"not built for ", platform};
        const char *fix_parts[3] = {"use a platform the item lists, or ask its publisher for a ", platform, " build"};
        char *fix = runtime_cache_text(fix_parts, 3);
        int answer = fix == NULL ? 0 : refuse(refusal, parts, 2, fix);
        free(fix);
        return answer;
    }
    files->own = cJSON_IsObject(built) ? cJSON_GetObjectItemCaseSensitive(built, "files") : NULL;
    return cJSON_IsObject(files->own) ? 1 : refuse_shape("platform files", version, refusal);
}

static int check_unambiguous(const version_files *files, const char *platform, cJSON **refusal) {
    for (const cJSON *member = files->own->child; files->shared != NULL && member != NULL; member = member->next) {
        if (cJSON_GetObjectItemCaseSensitive(files->shared, member->string) != NULL) {
            const char *parts[5] = {"file ", member->string, " is listed both for every platform and for ", platform,
                                    ", so which one to install is ambiguous"};
            return refuse(refusal, parts, 5, "list each file once, either for every platform or per platform");
        }
    }
    return 1;
}

static int files_of(const cJSON *entry, const char *version, const char *platform, version_files *files,
                    cJSON **refusal) {
    files->shared = cJSON_GetObjectItemCaseSensitive(entry, "files");
    files->own = NULL;
    if (is_absent(files->shared)) {
        files->shared = NULL;
    } else if (!cJSON_IsObject(files->shared)) {
        return refuse_shape("files", version, refusal);
    }
    return platform_files(entry, version, platform, files, refusal) &&
           (files->own == NULL || check_unambiguous(files, platform, refusal));
}

static int is_library(const cJSON *item) {
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
    return cJSON_IsString(kind) && strcmp(kind->valuestring, "library") == 0;
}

static char *version_directory(const char *cache, const char *id, const cJSON *item, const char *version,
                               const char *platform, cJSON **refusal) {
    const char *root_name = is_library(item) ? "libraries" : "bundles";
    char *root = runtime_cache_resolve(cache, &root_name, 1);
    const char *segments[3] = {id, version, platform};
    char *directory = root == NULL ? NULL : runtime_cache_resolve(root, segments, is_library(item) ? 3 : 2);
    if (directory != NULL && !runtime_cache_is_strictly_inside(root, directory)) {
        const char *parts[5] = {id, "@", version, " would be installed outside ", root};
        refuse(refusal, parts, 5, "use an item id and version the catalog schema accepts");
        free(directory);
        directory = NULL;
    }
    free(root);
    return directory;
}

static int is_sha256(const char *text) {
    size_t length = strlen(text);
    for (size_t index = 0; index < length; index++) {
        if (!isdigit((unsigned char)text[index]) && !(text[index] >= 'a' && text[index] <= 'f')) {
            return 0;
        }
    }
    return length == 64;
}

static int is_usable_reference(const cJSON *reference) {
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(reference, "url");
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(reference, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(reference, "size");
    return cJSON_IsString(url) && url->valuestring[0] != 0 && cJSON_IsString(sha256) &&
           is_sha256(sha256->valuestring) && cJSON_IsNumber(size) && size->valuedouble >= 0 &&
           size->valuedouble <= LARGEST_EXACT_INTEGER && floor(size->valuedouble) == size->valuedouble;
}

static int is_marker_path(const char *marker, const char *destination) {
    size_t length = strlen(marker);
    if (strlen(destination) != length) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (tolower((unsigned char)marker[index]) != tolower((unsigned char)destination[index])) {
            return 0;
        }
    }
    return 1;
}

/** @implNote Compared without case, since Windows and macOS file systems would let .INSTALLED.json overwrite the marker. */
static int check_destination(const char *directory, const char *marker, const runtime_cache_placement *placement,
                             cJSON **refusal) {
    if (!runtime_cache_is_strictly_inside(directory, placement->destination)) {
        const char *parts[4] = {"file ", placement->key, " would be written outside ", directory};
        return refuse(refusal, parts, 4, "remove the file from the catalog entry or give it a key inside the version");
    }
    if (is_marker_path(marker, placement->destination)) {
        const char *parts[3] = {"file ", placement->key, " would replace the cache's completion marker " RUNTIME_INSTALL_MARKER};
        return refuse(refusal, parts, 3, "rename the file in the catalog entry");
    }
    if (!is_usable_reference(placement->reference)) {
        const char *parts[3] = {"file ", placement->key, " carries no usable url, sha256 and size"};
        return refuse(refusal, parts, 3, ITEM_SHAPE_FIX);
    }
    return 1;
}

static const cJSON *reference_named(const version_files *files, const char *key) {
    const cJSON *own = files->own == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(files->own, key);
    return own != NULL ? own : cJSON_GetObjectItemCaseSensitive(files->shared, key);
}

/** The merged file keys in the order JavaScript enumerates {...shared, ...own}. */
static const cJSON **merged_keys(const version_files *files, cJSON **merged, size_t *count) {
    *merged = cJSON_CreateObject();
    const cJSON *parts[2] = {files->shared, files->own};
    for (size_t part = 0; *merged != NULL && part < 2; part++) {
        for (const cJSON *member = parts[part] == NULL ? NULL : parts[part]->child; member != NULL; member = member->next) {
            if (!cJSON_AddItemToObject(*merged, member->string, cJSON_CreateNull())) {
                cJSON_Delete(*merged);
                *merged = NULL;
                break;
            }
        }
    }
    *count = 0;
    return *merged == NULL ? NULL : spisor_json_members_in_javascript_order(*merged, count);
}

static int place_files(runtime_cache_plan *plan, const version_files *files, cJSON **refusal) {
    const char *marker_name = RUNTIME_INSTALL_MARKER;
    char *marker = runtime_cache_resolve(plan->directory, &marker_name, 1);
    cJSON *merged = NULL;
    size_t count = 0;
    const cJSON **keys = marker == NULL ? NULL : merged_keys(files, &merged, &count);
    plan->placements = count == 0 ? NULL : calloc(count, sizeof *plan->placements);
    int placed = marker != NULL && merged != NULL && (count == 0 || (keys != NULL && plan->placements != NULL));
    for (size_t index = 0; placed && index < count; index++) {
        runtime_cache_placement *placement = &plan->placements[plan->count++];
        placement->key = reference_named(files, keys[index]->string)->string;
        placement->reference = reference_named(files, keys[index]->string);
        placement->destination = runtime_cache_resolve(plan->directory, &placement->key, 1);
        placed = placement->destination != NULL && check_destination(plan->directory, marker, placement, refusal);
    }
    free((void *)keys);
    cJSON_Delete(merged);
    free(marker);
    return placed;
}

int runtime_cache_plan_install(const char *cache, const char *id, const cJSON *item, const char *version,
                               const char *platform, runtime_cache_plan *plan, cJSON **refusal) {
    memset(plan, 0, sizeof *plan);
    *refusal = NULL;
    version_files files = {NULL, NULL};
    const cJSON *entry = NULL;
    int planned = check_platform_shape(platform, refusal) && (entry = entry_of(item, version, refusal)) != NULL &&
                  files_of(entry, version, platform, &files, refusal) &&
                  (plan->directory = version_directory(cache, id, item, version, platform, refusal)) != NULL &&
                  place_files(plan, &files, refusal);
    if (!planned) {
        runtime_cache_free_plan(plan);
    }
    return planned;
}

void runtime_cache_free_plan(runtime_cache_plan *plan) {
    for (size_t index = 0; index < plan->count; index++) {
        free(plan->placements[index].destination);
    }
    free(plan->placements);
    free(plan->directory);
    memset(plan, 0, sizeof *plan);
}
