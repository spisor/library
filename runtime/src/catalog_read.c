#include "catalog_read.h"
#include "allocation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_check.h"
#include "json_parse.h"
#include "catalog_fold.h"
#include "spisor/json_write.h"
#include "catalog_text.h"
#include "catalog_url.h"
#include "spisor/json_kind.h"

#define NOT_JSON_DETAIL "the catalog index is not valid JSON: it is not one JSON document in UTF-8"
#define NOT_JSON_FIX "serve the index as a JSON document"
#define LATEST_FIX "set latest to one of the item's versions"
#define CASE_FIX "give every file of a version a name that differs from the others in more than case"
#define URL_FIX "use an https url under an https index, or a file url inside the index's own directory under a file index"
#define DEFAULT_PLUGIN_RUNTIME "javascript"

spisor_status runtime_catalog_refuse(runtime_catalog_refusal *refusal, char *detail, const char *fix, char *path) {
    refusal->detail = detail;
    refusal->fix = detail == NULL ? NULL : runtime_catalog_copy(fix);
    refusal->path = path;
    if (refusal->fix == NULL) {
        runtime_catalog_free_refusal(refusal);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_REFUSED;
}

void runtime_catalog_free_refusal(runtime_catalog_refusal *refusal) {
    spisor_memory_release(refusal->detail);
    spisor_memory_release(refusal->fix);
    spisor_memory_release(refusal->path);
    memset(refusal, 0, sizeof *refusal);
}

char *runtime_catalog_refusal_message(const runtime_catalog_refusal *refusal) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, refusal->detail);
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

static spisor_status check_latest(const cJSON *items, runtime_catalog_refusal *refusal) {
    size_t count;
    const cJSON **ordered = spisor_json_members_in_javascript_order(items, &count);
    if (ordered == NULL && count > 0) {
        return SPISOR_OUT_OF_MEMORY;
    }
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < count; index++) {
        const cJSON *item = ordered[index];
        const char *latest = member(item, "latest")->valuestring;
        if (member(member(item, "versions"), latest) == NULL) {
            const char *detail[] = {"item ", item->string, " names latest version ", latest, ", which it does not list", NULL};
            const char *path[] = {"/items/", item->string, "/latest", NULL};
            status = runtime_catalog_refuse(refusal, joined(detail), LATEST_FIX, joined(path));
        }
    }
    spisor_memory_release(ordered);
    return status;
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
    const char **grown = spisor_memory_reallocate(set->keys, (set->count + count + 1) * sizeof *grown);
    if (grown == NULL) {
        spisor_memory_release(members);
        return 0;
    }
    set->keys = grown;
    for (size_t index = 0; index < count; index++) {
        set->keys[set->count++] = members[index]->string;
    }
    spisor_memory_release(members);
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

static spisor_status refuse_collision(const char *id, const char *version, const key_set *set, size_t first, size_t second,
                            runtime_catalog_refusal *refusal) {
    const char *detail[] = {"files ", set->keys[first], " and ", set->keys[second], " of ", id, "@", version,
                            " differ only by case, so Windows and macOS would install both into one file", NULL};
    return runtime_catalog_refuse(refusal, joined(detail), CASE_FIX, version_path(id, version));
}

static spisor_status check_version_collisions(const char *id, const cJSON *version, runtime_catalog_refusal *refusal) {
    const cJSON *shared = member(version, "files");
    size_t platform_count;
    const cJSON **platforms = spisor_json_members_in_javascript_order(member(version, "platforms"), &platform_count);
    if (platforms == NULL && platform_count > 0) {
        return SPISOR_OUT_OF_MEMORY;
    }
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < (platform_count == 0 ? 1 : platform_count); index++) {
        const cJSON *platform_files = platform_count == 0 ? NULL : member(platforms[index], "files");
        key_set set;
        size_t first = 0;
        size_t second = 0;
        int collides = key_set_collides(shared, platform_files, &first, &second, &set);
        if (collides != 0) {
            status = collides < 0 ? SPISOR_OUT_OF_MEMORY
                                  : refuse_collision(id, version->string, &set, first, second, refusal);
        }
        spisor_memory_release(set.keys);
    }
    spisor_memory_release(platforms);
    return status;
}

typedef spisor_status (*version_rule)(const char *id, cJSON *version, const char *origin,
                                      runtime_catalog_refusal *refusal);

static spisor_status each_version(cJSON *items, const char *origin, version_rule rule,
                                  runtime_catalog_refusal *refusal) {
    size_t item_count;
    const cJSON **ordered_items = spisor_json_members_in_javascript_order(items, &item_count);
    if (ordered_items == NULL && item_count > 0) {
        return SPISOR_OUT_OF_MEMORY;
    }
    spisor_status status = SPISOR_OK;
    for (size_t item_index = 0; status == SPISOR_OK && item_index < item_count; item_index++) {
        size_t version_count;
        const cJSON **versions = spisor_json_members_in_javascript_order(member(ordered_items[item_index], "versions"),
                                                                 &version_count);
        if (versions == NULL && version_count > 0) {
            status = SPISOR_OUT_OF_MEMORY;
        }
        for (size_t index = 0; status == SPISOR_OK && index < version_count; index++) {
            status = rule(ordered_items[item_index]->string, (cJSON *)versions[index], origin, refusal);
        }
        spisor_memory_release(versions);
    }
    spisor_memory_release(ordered_items);
    return status;
}

static spisor_status collisions_rule(const char *id, cJSON *version, const char *origin, runtime_catalog_refusal *refusal) {
    (void)origin;
    return check_version_collisions(id, version, refusal);
}

static spisor_status refuse_url(const char *id, const char *version, const char *key, const char *origin,
                      runtime_catalog_refusal *refusal) {
    const char *detail[] = {"the url of file ", key, " in ", id, "@", version, " leaves its source (resolved against ",
                            origin, ")", NULL};
    return runtime_catalog_refuse(refusal, joined(detail), URL_FIX, version_path(id, version));
}

static spisor_status resolve_files(const char *id, const cJSON *version, cJSON *files, const char *origin,
                                   runtime_catalog_refusal *refusal) {
    size_t count;
    const cJSON **references = spisor_json_members_in_javascript_order(files, &count);
    if (references == NULL && count > 0) {
        return SPISOR_OUT_OF_MEMORY;
    }
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < count; index++) {
        cJSON *reference = (cJSON *)references[index];
        char *resolved = runtime_catalog_resolve_within_origin(member(reference, "url")->valuestring, origin);
        if (resolved == NULL) {
            status = refuse_url(id, version->string, reference->string, origin, refusal);
        } else {
            cJSON *replacement = cJSON_CreateString(resolved);
            if (replacement == NULL || !cJSON_ReplaceItemInObjectCaseSensitive(reference, "url", replacement)) {
                cJSON_Delete(replacement);
                status = SPISOR_OUT_OF_MEMORY;
            }
            spisor_memory_release(resolved);
        }
    }
    spisor_memory_release(references);
    return status;
}

static spisor_status urls_rule(const char *id, cJSON *version, const char *origin, runtime_catalog_refusal *refusal) {
    spisor_status status = resolve_files(id, version, cJSON_GetObjectItemCaseSensitive(version, "files"), origin, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    size_t count;
    const cJSON **platforms =
        spisor_json_members_in_javascript_order(cJSON_GetObjectItemCaseSensitive(version, "platforms"), &count);
    if (platforms == NULL && count > 0) {
        return SPISOR_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == SPISOR_OK && index < count; index++) {
        status = resolve_files(id, version, cJSON_GetObjectItemCaseSensitive(platforms[index], "files"), origin,
                               refusal);
    }
    spisor_memory_release(platforms);
    return status;
}

static spisor_status add_default_runtimes(cJSON *items) {
    for (cJSON *item = items->child; item != NULL; item = item->next) {
        int plugin = strcmp(member(item, "kind")->valuestring, "plugin") == 0;
        if (plugin && member(item, "runtime") == NULL &&
            cJSON_AddStringToObject(item, "runtime", DEFAULT_PLUGIN_RUNTIME) == NULL) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static size_t byte_order_mark_length(const char *text, size_t length) {
    return length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
                   (unsigned char)text[2] == 0xBF
               ? 3
               : 0;
}

static spisor_status check_rules(cJSON *document, const char *origin, runtime_catalog_refusal *refusal) {
    cJSON *items = cJSON_GetObjectItemCaseSensitive(document, "items");
    spisor_status status = runtime_catalog_check_schema(document, refusal);
    status = status == SPISOR_OK ? check_latest(items, refusal) : status;
    status = status == SPISOR_OK ? each_version(items, origin, collisions_rule, refusal) : status;
    status = status == SPISOR_OK ? each_version(items, origin, urls_rule, refusal) : status;
    return status == SPISOR_OK ? add_default_runtimes(items) : status;
}

spisor_status runtime_catalog_read_text(const char *text, size_t length, const char *origin, cJSON **catalog,
                                        runtime_catalog_refusal *refusal) {
    memset(refusal, 0, sizeof *refusal);
    *catalog = NULL;
    size_t skipped = byte_order_mark_length(text, length);
    int out_of_memory = 0;
    cJSON *document = spisor_parse_json_reporting((const uint8_t *)text + skipped, length - skipped, &out_of_memory);
    if (document == NULL) {
        return out_of_memory ? SPISOR_OUT_OF_MEMORY
                             : runtime_catalog_refuse(refusal, runtime_catalog_copy(NOT_JSON_DETAIL), NOT_JSON_FIX, NULL);
    }
    spisor_status status = check_rules(document, origin, refusal);
    if (status != SPISOR_OK) {
        cJSON_Delete(document);
        return status;
    }
    *catalog = document;
    return SPISOR_OK;
}
