#include "catalog_read.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_check.h"
#include "catalog_fold.h"
#include "spisor/json_write.h"
#include "catalog_text.h"
#include "catalog_url.h"
#include "spisor/json_kind.h"

#define OUT_OF_MEMORY_DETAIL "the catalog index could not be read: the process ran out of memory"
#define NOT_JSON_DETAIL "the catalog index is not valid JSON: it is not one JSON document in UTF-8"
#define NOT_JSON_FIX "serve the index as a JSON document"
#define LATEST_FIX "set latest to one of the item's versions"
#define CASE_FIX "give every file of a version a name that differs from the others in more than case"
#define URL_FIX "use an https url under an https index, or a file url inside the index's own directory under a file index"
#define DEFAULT_PLUGIN_RUNTIME "javascript"

int runtime_catalog_refuse(runtime_catalog_refusal *refusal, char *detail, const char *fix, char *path) {
    refusal->detail = detail != NULL ? detail : runtime_catalog_copy(OUT_OF_MEMORY_DETAIL);
    refusal->fix = runtime_catalog_copy(fix);
    refusal->path = path;
    return 0;
}

void runtime_catalog_free_refusal(runtime_catalog_refusal *refusal) {
    free(refusal->detail);
    free(refusal->fix);
    free(refusal->path);
    memset(refusal, 0, sizeof *refusal);
}

char *runtime_catalog_refusal_message(const runtime_catalog_refusal *refusal) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, refusal->detail == NULL ? OUT_OF_MEMORY_DETAIL : refusal->detail);
    if (refusal->path != NULL && refusal->path[0] != 0) {
        runtime_catalog_text_add(&text, " at ");
        runtime_catalog_text_add(&text, refusal->path);
    }
    return runtime_catalog_text_finish(&text);
}

static char *joined(const char *const *parts) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, "");
    for (size_t index = 0; parts[index] != NULL; index++) {
        runtime_catalog_text_add(&text, parts[index]);
    }
    return runtime_catalog_text_finish(&text);
}

static char *version_path(const char *id, const char *version) {
    const char *parts[] = {"/items/", id, "/versions/", version, NULL};
    return joined(parts);
}

static const cJSON *member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static int check_latest(const cJSON *items, runtime_catalog_refusal *refusal) {
    size_t count;
    const cJSON **ordered = spisor_json_members_in_javascript_order(items, &count);
    if (ordered == NULL && count > 0) {
        return runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
    }
    int valid = 1;
    for (size_t index = 0; valid && index < count; index++) {
        const cJSON *item = ordered[index];
        const char *latest = member(item, "latest")->valuestring;
        if (member(member(item, "versions"), latest) == NULL) {
            const char *detail[] = {"item ", item->string, " names latest version ", latest, ", which it does not list", NULL};
            const char *path[] = {"/items/", item->string, "/latest", NULL};
            valid = runtime_catalog_refuse(refusal, joined(detail), LATEST_FIX, joined(path));
        }
    }
    free(ordered);
    return valid;
}

typedef struct key_set {
    const char **keys;
    size_t count;
} key_set;

static int add_keys(key_set *set, const cJSON *files) {
    size_t count;
    const cJSON **members = spisor_json_members_in_javascript_order(files, &count);
    if (members == NULL && count > 0) {
        return 0;
    }
    const char **grown = realloc(set->keys, (set->count + count + 1) * sizeof *grown);
    if (grown == NULL) {
        free(members);
        return 0;
    }
    set->keys = grown;
    for (size_t index = 0; index < count; index++) {
        set->keys[set->count++] = members[index]->string;
    }
    free(members);
    return 1;
}

/** -1 out of memory, 1 when the keys one install writes into a directory hold a folded pair. */
static int key_set_collides(const cJSON *shared, const cJSON *platform_files, size_t *first, size_t *second,
                            key_set *set) {
    set->keys = NULL;
    set->count = 0;
    if (!add_keys(set, shared) || !add_keys(set, platform_files)) {
        return -1;
    }
    return runtime_catalog_folded_duplicate(set->keys, set->count, first, second);
}

static int refuse_collision(const char *id, const char *version, const key_set *set, size_t first, size_t second,
                            runtime_catalog_refusal *refusal) {
    const char *detail[] = {"files ", set->keys[first], " and ", set->keys[second], " of ", id, "@", version,
                            " differ only by case, so Windows and macOS would install both into one file", NULL};
    return runtime_catalog_refuse(refusal, joined(detail), CASE_FIX, version_path(id, version));
}

static int check_version_collisions(const char *id, const cJSON *version, runtime_catalog_refusal *refusal) {
    const cJSON *shared = member(version, "files");
    size_t platform_count;
    const cJSON **platforms = spisor_json_members_in_javascript_order(member(version, "platforms"), &platform_count);
    if (platforms == NULL && platform_count > 0) {
        return runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
    }
    int valid = 1;
    for (size_t index = 0; valid && index < (platform_count == 0 ? 1 : platform_count); index++) {
        const cJSON *platform_files = platform_count == 0 ? NULL : member(platforms[index], "files");
        key_set set;
        size_t first = 0;
        size_t second = 0;
        int collides = key_set_collides(shared, platform_files, &first, &second, &set);
        if (collides != 0) {
            valid = collides < 0 ? runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL)
                                 : refuse_collision(id, version->string, &set, first, second, refusal);
        }
        free(set.keys);
    }
    free(platforms);
    return valid;
}

typedef int (*version_rule)(const char *id, cJSON *version, const char *origin, runtime_catalog_refusal *refusal);

static int each_version(cJSON *items, const char *origin, version_rule rule, runtime_catalog_refusal *refusal) {
    size_t item_count;
    const cJSON **ordered_items = spisor_json_members_in_javascript_order(items, &item_count);
    if (ordered_items == NULL && item_count > 0) {
        return runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
    }
    int valid = 1;
    for (size_t item_index = 0; valid && item_index < item_count; item_index++) {
        size_t version_count;
        const cJSON **versions = spisor_json_members_in_javascript_order(member(ordered_items[item_index], "versions"),
                                                                 &version_count);
        if (versions == NULL && version_count > 0) {
            valid = runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
        }
        for (size_t index = 0; valid && index < version_count; index++) {
            valid = rule(ordered_items[item_index]->string, (cJSON *)versions[index], origin, refusal);
        }
        free(versions);
    }
    free(ordered_items);
    return valid;
}

static int collisions_rule(const char *id, cJSON *version, const char *origin, runtime_catalog_refusal *refusal) {
    (void)origin;
    return check_version_collisions(id, version, refusal);
}

static int refuse_url(const char *id, const char *version, const char *key, const char *origin,
                      runtime_catalog_refusal *refusal) {
    const char *detail[] = {"the url of file ", key, " in ", id, "@", version, " leaves its source (resolved against ",
                            origin, ")", NULL};
    return runtime_catalog_refuse(refusal, joined(detail), URL_FIX, version_path(id, version));
}

static int resolve_files(const char *id, const cJSON *version, cJSON *files, const char *origin,
                         runtime_catalog_refusal *refusal) {
    size_t count;
    const cJSON **references = spisor_json_members_in_javascript_order(files, &count);
    if (references == NULL && count > 0) {
        return runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
    }
    int valid = 1;
    for (size_t index = 0; valid && index < count; index++) {
        cJSON *reference = (cJSON *)references[index];
        char *resolved = runtime_catalog_resolve_within_origin(member(reference, "url")->valuestring, origin);
        if (resolved == NULL) {
            valid = refuse_url(id, version->string, reference->string, origin, refusal);
        } else {
            cJSON *replacement = cJSON_CreateString(resolved);
            valid = replacement != NULL && cJSON_ReplaceItemInObjectCaseSensitive(reference, "url", replacement);
            if (!valid) {
                cJSON_Delete(replacement);
                runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
            }
            free(resolved);
        }
    }
    free(references);
    return valid;
}

static int urls_rule(const char *id, cJSON *version, const char *origin, runtime_catalog_refusal *refusal) {
    if (!resolve_files(id, version, cJSON_GetObjectItemCaseSensitive(version, "files"), origin, refusal)) {
        return 0;
    }
    size_t count;
    const cJSON **platforms =
        spisor_json_members_in_javascript_order(cJSON_GetObjectItemCaseSensitive(version, "platforms"), &count);
    if (platforms == NULL && count > 0) {
        return runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
    }
    int valid = 1;
    for (size_t index = 0; valid && index < count; index++) {
        valid = resolve_files(id, version, cJSON_GetObjectItemCaseSensitive(platforms[index], "files"), origin,
                              refusal);
    }
    free(platforms);
    return valid;
}

static int add_default_runtimes(cJSON *items, runtime_catalog_refusal *refusal) {
    for (cJSON *item = items->child; item != NULL; item = item->next) {
        int plugin = strcmp(member(item, "kind")->valuestring, "plugin") == 0;
        if (plugin && member(item, "runtime") == NULL &&
            cJSON_AddStringToObject(item, "runtime", DEFAULT_PLUGIN_RUNTIME) == NULL) {
            return runtime_catalog_refuse(refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
        }
    }
    return 1;
}

static size_t byte_order_mark_length(const char *text, size_t length) {
    return length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
                   (unsigned char)text[2] == 0xBF
               ? 3
               : 0;
}

cJSON *runtime_catalog_read_text(const char *text, size_t length, const char *origin,
                                 runtime_catalog_refusal *refusal) {
    memset(refusal, 0, sizeof *refusal);
    size_t skipped = byte_order_mark_length(text, length);
    cJSON *document = spisor_parse_json((const uint8_t *)text + skipped, length - skipped);
    if (document == NULL) {
        runtime_catalog_refuse(refusal, runtime_catalog_copy(NOT_JSON_DETAIL), NOT_JSON_FIX, NULL);
        return NULL;
    }
    cJSON *items = cJSON_GetObjectItemCaseSensitive(document, "items");
    int valid = runtime_catalog_check_schema(document, refusal) && check_latest(items, refusal) &&
                each_version(items, origin, collisions_rule, refusal) && each_version(items, origin, urls_rule, refusal) &&
                add_default_runtimes(items, refusal);
    if (!valid) {
        cJSON_Delete(document);
        return NULL;
    }
    return document;
}
