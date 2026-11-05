#include "cache_locate.h"

#include <stdlib.h>
#include <string.h>

#include "cache_marker.h"
#include "cache_place.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "node_json_file.h"
#include "spisor/file.h"

#define MANIFEST_FILE "plugin.json"
#define VERSION_FIX "ask the catalog's publisher to give plugin.json the version the catalog lists it under"
#define OUTSIDE_FIX "give the plugin an id and version that are plain folder names"
#define REINSTALL_FIX "install it again"

typedef struct location {
    const char *id;
    const char *version;
    char *directory;
} location;

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) && value->valuestring[0] != 0 ? value->valuestring : NULL;
}

static int add_problem(cJSON *problems, const location *place, const char *detail, const char *fix, const char *file) {
    cJSON *problem = cJSON_CreateObject();
    int built = problem != NULL && cJSON_AddStringToObject(problem, "id", place->id) != NULL &&
                cJSON_AddStringToObject(problem, "version", place->version) != NULL &&
                cJSON_AddStringToObject(problem, "detail", detail) != NULL &&
                cJSON_AddStringToObject(problem, "fix", fix) != NULL &&
                (file == NULL || cJSON_AddStringToObject(problem, "file", file) != NULL);
    if (!built || !cJSON_AddItemToArray(problems, problem)) {
        cJSON_Delete(problem);
        return 0;
    }
    return 1;
}

static int add_outside(cJSON *problems, const location *place) {
    const char *parts[4] = {place->id, "@", place->version, " names no folder inside the cache's bundles"};
    char *detail = runtime_cache_text(parts, 4);
    int added = detail != NULL && add_problem(problems, place, detail, OUTSIDE_FIX, NULL);
    free(detail);
    return added;
}

static int add_damaged(cJSON *problems, const location *place, const cJSON *check) {
    const char *parts[2] = {place->directory, " is damaged"};
    char *fallback = text_of(check, "detail") == NULL ? runtime_cache_text(parts, 2) : NULL;
    const char *detail = fallback != NULL ? fallback : text_of(check, "detail");
    const char *fix = text_of(check, "fix") != NULL ? text_of(check, "fix") : REINSTALL_FIX;
    int added = detail != NULL && add_problem(problems, place, detail, fix, text_of(check, "file"));
    free(fallback);
    return added;
}

static int add_other_version(cJSON *problems, const location *place, const char *named) {
    const char *parts[6] = {MANIFEST_FILE " in ", place->directory, " names version ", named == NULL ? "(none)" : named,
                            ", not ", place->version};
    char *detail = runtime_cache_text(parts, 6);
    int added = detail != NULL && add_problem(problems, place, detail, VERSION_FIX, MANIFEST_FILE);
    free(detail);
    return added;
}

/** The entry manifest names inside directory when it is there, else NULL; *failed is set when memory runs out. */
static char *entry_of(const char *directory, const cJSON *manifest, int *failed) {
    const char *entry = text_of(manifest, "entry");
    char *path = entry == NULL ? NULL : runtime_cache_resolve(directory, &entry, 1);
    *failed = entry != NULL && path == NULL;
    if (path != NULL && (!runtime_cache_is_strictly_inside(directory, path) || !spisor_file_exists(path))) {
        free(path);
        path = NULL;
    }
    return path;
}

static int add_located(cJSON *located, const location *place, const char *manifest_path, const char *entry) {
    cJSON *found = cJSON_CreateObject();
    int built = found != NULL && cJSON_AddStringToObject(found, "id", place->id) != NULL &&
                cJSON_AddStringToObject(found, "version", place->version) != NULL &&
                cJSON_AddStringToObject(found, "directory", place->directory) != NULL &&
                cJSON_AddStringToObject(found, "manifest", manifest_path) != NULL &&
                (entry == NULL || cJSON_AddStringToObject(found, "entry", entry) != NULL);
    if (!built || !cJSON_AddItemToArray(located, found)) {
        cJSON_Delete(found);
        return 0;
    }
    return 1;
}

/** Reads plugin.json of an intact version: located when it names the version, a problem when it names another. */
static int add_intact(cJSON *located, cJSON *problems, const location *place) {
    const char *name = MANIFEST_FILE;
    char *manifest_path = runtime_cache_resolve(place->directory, &name, 1);
    if (manifest_path == NULL) {
        return 0;
    }
    int added = 1;
    if (spisor_file_exists(manifest_path)) {
        cJSON *manifest = runtime_read_node_json(manifest_path);
        const char *named = cJSON_IsObject(manifest) ? text_of(manifest, "version") : NULL;
        int failed = 0;
        char *entry = named != NULL && strcmp(named, place->version) == 0 ? entry_of(place->directory, manifest, &failed) : NULL;
        if (named == NULL || strcmp(named, place->version) != 0) {
            added = add_other_version(problems, place, named);
        } else {
            added = !failed && add_located(located, place, manifest_path, entry);
        }
        free(entry);
        cJSON_Delete(manifest);
    }
    free(manifest_path);
    return added;
}

static int locate_one(const char *cache, const char *id, const char *version, cJSON *located, cJSON *problems) {
    location place = {id, version, runtime_cache_bundle_directory(cache, id, version)};
    if (place.directory == NULL) {
        return add_outside(problems, &place);
    }
    cJSON *check = runtime_cache_check_version_sizes(place.directory);
    const char *kind = text_of(check, "kind");
    int added = kind != NULL;
    if (kind != NULL && strcmp(kind, "damaged") == 0) {
        added = add_damaged(problems, &place, check);
    } else if (kind != NULL && strcmp(kind, "intact") == 0) {
        added = add_intact(located, problems, &place);
    }
    cJSON_Delete(check);
    free(place.directory);
    return added;
}

static int compare_ids(const void *left, const void *right) {
    return strcmp((*(const cJSON *const *)left)->string, (*(const cJSON *const *)right)->string);
}

/** The string members of plugins sorted by id, as an array of borrowed pointers freed with free. */
static const cJSON **sorted_members(const cJSON *plugins, size_t *count) {
    *count = 0;
    for (const cJSON *entry = plugins->child; entry != NULL; entry = entry->next) {
        *count += cJSON_IsString(entry) ? 1 : 0;
    }
    const cJSON **members = malloc((*count == 0 ? 1 : *count) * sizeof *members);
    size_t index = 0;
    for (const cJSON *entry = plugins->child; members != NULL && entry != NULL; entry = entry->next) {
        if (cJSON_IsString(entry)) {
            members[index++] = entry;
        }
    }
    if (members != NULL) {
        qsort(members, *count, sizeof *members, compare_ids);
    }
    return members;
}

cJSON *runtime_cache_locate_plugins(const char *cache, const cJSON *plugins) {
    size_t count = 0;
    const cJSON **members = sorted_members(plugins, &count);
    cJSON *answer = cJSON_CreateObject();
    cJSON *located = cJSON_CreateArray();
    cJSON *problems = cJSON_CreateArray();
    int done = members != NULL && answer != NULL && located != NULL && problems != NULL;
    for (size_t index = 0; done && index < count; index++) {
        done = locate_one(cache, members[index]->string, members[index]->valuestring, located, problems);
    }
    free(members);
    if (!done) {
        cJSON_Delete(located);
        cJSON_Delete(problems);
        cJSON_Delete(answer);
        return NULL;
    }
    int attached = runtime_catalog_attach(answer, "located", located);
    if (!attached) {
        cJSON_Delete(problems);
    }
    if (!attached || !runtime_catalog_attach(answer, "problems", problems)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}
