#include "identity_also_at.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "identity.h"
#include "identity_resolve.h"
#include "json_service.h"
#include "spisor/json_kind.h"

#define ALSO_AT_PATH ".weivel/also-at.json"
#define MAXIMUM_COPIES 16

/** Stands in a files map for a file that was read and does not parse, which is not the same as no file. */
static cJSON *unparseable_file(void) {
    return cJSON_CreateString("");
}

static int is_text(const char *text, runtime_identity_part part) {
    return strlen(text) == part.length && strncmp(text, part.start, part.length) == 0;
}

/** A repo id: valid, not local, and with no folder below the repo. */
static int is_repo_id(const char *text) {
    runtime_identity parsed;
    return runtime_identity_parse_id(text, &parsed) && !is_text("local", parsed.resolver) && parsed.path.length == 0;
}

static int is_repo_id_item(const cJSON *item) {
    return cJSON_IsString(item) && is_repo_id(item->valuestring);
}

static int has_only_repo_ids(const cJSON *ids) {
    if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) > MAXIMUM_COPIES) {
        return 0;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, ids) {
        if (!is_repo_id_item(item)) {
            return 0;
        }
    }
    return 1;
}

static int is_well_formed(const cJSON *file) {
    return cJSON_IsObject(file) && is_repo_id_item(cJSON_GetObjectItemCaseSensitive(file, "primary")) &&
           has_only_repo_ids(cJSON_GetObjectItemCaseSensitive(file, "ids"));
}

static const char *primary_of(const cJSON *file) {
    return cJSON_GetObjectItemCaseSensitive(file, "primary")->valuestring;
}

static int lists(const cJSON *file, const char *id) {
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(file, "ids")) {
        if (strcmp(item->valuestring, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int contains(const cJSON *ids, const char *id) {
    const cJSON *item;
    cJSON_ArrayForEach(item, ids) {
        if (strcmp(item->valuestring, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_counted_copy(const cJSON *file, const char *id) {
    return is_well_formed(file) && lists(file, id);
}

typedef enum start_file_state { START_FILE_USABLE, START_FILE_MALFORMED, START_FILE_UNAVAILABLE } start_file_state;

static cJSON *answer_of(const cJSON *ids, const char *primary, int primary_confirmed, start_file_state state) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddItemToObject(answer, "ids", cJSON_Duplicate(ids, 1)) == 0 ||
        cJSON_AddStringToObject(answer, "primary", primary) == NULL ||
        cJSON_AddBoolToObject(answer, "primaryConfirmed", primary_confirmed) == NULL ||
        (state == START_FILE_MALFORMED && cJSON_AddTrueToObject(answer, "startFileMalformed") == NULL) ||
        (state == START_FILE_UNAVAILABLE && cJSON_AddTrueToObject(answer, "startFileUnavailable") == NULL)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *only_this_copy(const char *id, start_file_state state) {
    cJSON *ids = cJSON_CreateArray();
    cJSON *answer = NULL;
    if (ids != NULL && cJSON_AddItemToArray(ids, cJSON_CreateString(id)) != 0) {
        answer = answer_of(ids, id, state != START_FILE_UNAVAILABLE, state);
    }
    cJSON_Delete(ids);
    return answer;
}

static cJSON *refuse_primaries(const char *first_id, const char *first_primary, const char *second_id,
                               const char *second_primary, cJSON **refusal) {
    char message[512];
    snprintf(message, sizeof message, RUNTIME_PRIMARIES_DISAGREE ": %s names %s and %s names %s", first_id,
             first_primary, second_id, second_primary);
    *refusal = runtime_refusal(message, "make every copy's also-at file name the same primary, then try again");
    return NULL;
}

static int adds_id(cJSON *ids, const char *id) {
    return cJSON_AddItemToArray(ids, cJSON_CreateString(id)) != 0;
}

/**
 * The ids the starting file lists whose own file lists the starting id back, in the starting file's order, after the
 * starting id. NULL with *refusal set when a counted copy names another primary, NULL with none when memory ran out.
 */
static cJSON *counted_ids(const char *id, const cJSON *start_file, const cJSON *files, cJSON **refusal) {
    cJSON *ids = cJSON_CreateArray();
    if (ids == NULL || !adds_id(ids, id)) {
        cJSON_Delete(ids);
        return NULL;
    }
    const cJSON *listed;
    cJSON_ArrayForEach(listed, cJSON_GetObjectItemCaseSensitive(start_file, "ids")) {
        const cJSON *file = cJSON_GetObjectItemCaseSensitive(files, listed->valuestring);
        if (contains(ids, listed->valuestring) || !is_counted_copy(file, id)) {
            continue;
        }
        if (strcmp(primary_of(file), primary_of(start_file)) != 0) {
            refuse_primaries(id, primary_of(start_file), listed->valuestring, primary_of(file), refusal);
            cJSON_Delete(ids);
            return NULL;
        }
        if (!adds_id(ids, listed->valuestring)) {
            cJSON_Delete(ids);
            return NULL;
        }
    }
    return ids;
}

static cJSON *agree_on(const char *id, const cJSON *files, cJSON **refusal) {
    const cJSON *start_file = cJSON_GetObjectItemCaseSensitive(files, id);
    if (start_file == NULL || cJSON_IsNull(start_file)) {
        return only_this_copy(id, START_FILE_USABLE);
    }
    if (cJSON_IsFalse(start_file)) {
        return only_this_copy(id, START_FILE_UNAVAILABLE);
    }
    if (!is_well_formed(start_file)) {
        return only_this_copy(id, START_FILE_MALFORMED);
    }
    cJSON *ids = counted_ids(id, start_file, files, refusal);
    cJSON *answer = ids == NULL ? NULL : answer_of(ids, primary_of(start_file), contains(ids, primary_of(start_file)), START_FILE_USABLE);
    cJSON_Delete(ids);
    return answer;
}

static const cJSON *repo_id_argument(const cJSON *argument, const char *member, cJSON **refusal) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(argument, member);
    if (!is_repo_id_item(id)) {
        char message[128];
        snprintf(message, sizeof message, "the %s is not the id of a repo", member);
        *refusal = runtime_refusal(message, "send a repo id such as github:<number>, with no folder and not local");
        return NULL;
    }
    return id;
}

static cJSON *agree(const cJSON *argument, cJSON **refusal) {
    const cJSON *id = repo_id_argument(argument, "id", refusal);
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(argument, "files");
    if (id == NULL) {
        return NULL;
    }
    if (!cJSON_IsObject(files)) {
        *refusal = runtime_refusal("the argument carries no files", "send {\"id\": \"<id>\", \"files\": {\"<id>\": <file or null>}}");
        return NULL;
    }
    return agree_on(id->valuestring, files, refusal);
}

static cJSON *parsed_file(const uint8_t *content, size_t length) {
    cJSON *file = spisor_parse_json(content, length);
    return file != NULL ? file : unparseable_file();
}

/**
 * The also-at file of one copy as a files map entry: its parsed content, a marker when it does not parse, null
 * when the host says it is absent, or false when the read failed any other way. NULL when memory ran out, or with *refusal set when the id could not be
 * resolved, which only the id we started from treats as more than an unreachable copy.
 */
static cJSON *read_copy(const char *id, const char *api_base, cJSON **refusal) {
    uint8_t *content = NULL;
    size_t length = 0;
    runtime_repo_file_status status = runtime_identity_read_file(id, api_base, ALSO_AT_PATH, &content, &length, refusal);
    cJSON *file = NULL;
    if (status == RUNTIME_REPO_FILE_READ) {
        file = parsed_file(content, length);
    } else if (status == RUNTIME_REPO_FILE_TOO_LARGE) {
        file = unparseable_file();
    } else if (status == RUNTIME_REPO_FILE_ABSENT) {
        file = cJSON_CreateNull();
    } else if (status == RUNTIME_REPO_FILE_UNAVAILABLE) {
        file = cJSON_CreateFalse();
    }
    free(content);
    return file;
}

static cJSON *read_unreachable_tolerant(const char *id, const char *api_base) {
    cJSON *refusal = NULL;
    cJSON *file = read_copy(id, api_base, &refusal);
    cJSON_Delete(refusal);
    return file;
}

static cJSON *read_start_copy(const char *id, const char *api_base, cJSON **refusal) {
    cJSON *file = read_copy(id, api_base, refusal);
    if (*refusal != NULL) {
        cJSON_Delete(file);
        return NULL;
    }
    return file;
}

static int adds_copy(cJSON *files, const char *id, cJSON *file) {
    return file != NULL && cJSON_AddItemToObject(files, id, file) != 0;
}

static int reads_listed_copies(cJSON *files, const cJSON *start_file, const char *api_base) {
    const cJSON *listed;
    cJSON_ArrayForEach(listed, cJSON_GetObjectItemCaseSensitive(start_file, "ids")) {
        if (cJSON_GetObjectItemCaseSensitive(files, listed->valuestring) == NULL &&
            !adds_copy(files, listed->valuestring, read_unreachable_tolerant(listed->valuestring, api_base))) {
            return 0;
        }
    }
    return 1;
}

static cJSON *agree_from_hosts(const char *id, const char *api_base, cJSON **refusal) {
    cJSON *files = cJSON_CreateObject();
    cJSON *start_file = files == NULL ? NULL : read_start_copy(id, api_base, refusal);
    if (!adds_copy(files, id, start_file)) {
        cJSON_Delete(files);
        return NULL;
    }
    cJSON *answer = NULL;
    if (!is_well_formed(start_file) || reads_listed_copies(files, start_file, api_base)) {
        answer = agree_on(id, files, refusal);
    }
    cJSON_Delete(files);
    return answer;
}

/** Whether apiBase is absent or text; *api_base is NULL when absent. */
static int read_api_base(const cJSON *argument, const char **api_base, cJSON **refusal) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "apiBase");
    if (item != NULL && !cJSON_IsString(item)) {
        *refusal = runtime_refusal("apiBase is not text", "leave apiBase out: the host's own API address is used");
        return 0;
    }
    *api_base = item == NULL ? NULL : item->valuestring;
    return 1;
}

static cJSON *also_at(const cJSON *argument, cJSON **refusal) {
    const cJSON *id = repo_id_argument(argument, "id", refusal);
    const char *api_base = NULL;
    if (id == NULL || !read_api_base(argument, &api_base, refusal)) {
        return NULL;
    }
    return agree_from_hosts(id->valuestring, api_base, refusal);
}

static cJSON *accepts_version(const cJSON *argument, cJSON **refusal) {
    const cJSON *recorded = repo_id_argument(argument, "recorded", refusal);
    const cJSON *primary = recorded == NULL ? NULL : repo_id_argument(argument, "primary", refusal);
    if (primary == NULL) {
        return NULL;
    }
    int accepted = strcmp(recorded->valuestring, primary->valuestring) == 0;
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddBoolToObject(answer, "accepted", accepted) == NULL ||
        cJSON_AddBoolToObject(answer, "needsUserAcceptance", !accepted) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

RUNTIME_EXPORT int32_t runtime_identity_agree(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, agree);
}

RUNTIME_EXPORT int32_t runtime_identity_also_at(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, also_at);
}

RUNTIME_EXPORT int32_t runtime_identity_accepts_version(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, accepts_version);
}
