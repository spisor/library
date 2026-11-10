#include "marketplace_memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_service.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define STORE_FILE "marketplace-names.json"
#define STORE_FORMAT 1
#define MESSAGE_SIZE 2048

typedef struct remembered_name {
    const char *state;
    const char *marketplace;
    const char *name;
    const char *id;
    const char *accept;
    int is_recording;
} remembered_name;

static cJSON *refuse(cJSON **refusal, const char *fix, const char *format, ...) {
    char message[MESSAGE_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) && value->valuestring[0] != '\0' ? value->valuestring : NULL;
}

static int reads_request(const cJSON *argument, remembered_name *request, cJSON **refusal) {
    request->state = text_of(argument, "state");
    request->marketplace = text_of(argument, "marketplace");
    request->name = text_of(argument, "name");
    request->id = text_of(argument, "id");
    request->accept = text_of(argument, "accept");
    const cJSON *recording = cJSON_GetObjectItemCaseSensitive(argument, "record");
    request->is_recording = recording == NULL || cJSON_IsTrue(recording);
    if (request->state == NULL || !runtime_is_usable_home(request->state) || request->name == NULL || request->id == NULL ||
        (cJSON_GetObjectItemCaseSensitive(argument, "marketplace") != NULL && request->marketplace == NULL) ||
        (cJSON_GetObjectItemCaseSensitive(argument, "accept") != NULL && request->accept == NULL) ||
        (recording != NULL && !cJSON_IsBool(recording))) {
        refuse(refusal, "send {state, name, id} and, when they are known, marketplace, accept and record",
               "the argument carries no usable state, name or id, or a marketplace, accept or record that is not usable");
        return 0;
    }
    return 1;
}

static char *store_path(const char *state) {
    const char *segments[2] = {state, STORE_FILE};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static cJSON *empty_store(void) {
    cJSON *store = cJSON_CreateObject();
    if (store == NULL || cJSON_AddNumberToObject(store, "format", STORE_FORMAT) == NULL ||
        cJSON_AddObjectToObject(store, "names") == NULL) {
        cJSON_Delete(store);
        return NULL;
    }
    return store;
}

static int is_store(const cJSON *store) {
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(store, "format");
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(store, "names");
    if (!cJSON_IsObject(store) || !cJSON_IsNumber(format) || format->valuedouble != STORE_FORMAT || !cJSON_IsObject(names)) {
        return 0;
    }
    for (const cJSON *entry = names->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsString(entry)) {
            return 0;
        }
    }
    return 1;
}

/** The store at path, empty when there is no file; NULL with *refusal set for one that cannot be read or is no store. */
static cJSON *loaded_store(const char *path, cJSON **refusal) {
    if (!spisor_file_exists(path)) {
        return empty_store();
    }
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    cJSON *store = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    free(bytes);
    if (!is_store(store)) {
        cJSON_Delete(store);
        return refuse(refusal, "repair the file or move it away, then run the command again; it is left as it was",
                      "%s is not a record of marketplace names: it is not JSON of {format: 1, names: {name: id}}", path);
    }
    return store;
}

static int writes_store(const cJSON *store, const char *state, const char *path) {
    char *pretty = spisor_json_stringify_pretty(store);
    size_t length = pretty == NULL ? 0 : strlen(pretty);
    char *text = pretty == NULL ? NULL : realloc(pretty, length + 2);
    int written = 0;
    if (text != NULL) {
        text[length] = '\n';
        text[length + 1] = '\0';
        written = spisor_make_directories(state) == 1 && spisor_replace_file(path, (const uint8_t *)text, length + 1) == 1;
    }
    free(text == NULL ? pretty : text);
    return written;
}

static char *qualified_name(const remembered_name *request) {
    if (request->marketplace == NULL) {
        size_t bare_length = strlen(request->name) + 1;
        char *bare = malloc(bare_length);
        if (bare != NULL) {
            memcpy(bare, request->name, bare_length);
        }
        return bare;
    }
    size_t length = strlen(request->marketplace) + strlen(request->name) + 2;
    char *key = malloc(length);
    if (key != NULL) {
        snprintf(key, length, "%s/%s", request->marketplace, request->name);
    }
    return key;
}

static cJSON *remembered_answer(void) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, "remembered") == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *settled_check_answer(void) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, "settled") == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *needs_acceptance_answer(const char *was, const char *now) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, "needsUserAcceptance") == NULL ||
        cJSON_AddStringToObject(answer, "was", was) == NULL || cJSON_AddStringToObject(answer, "now", now) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int records_id(cJSON *store, const char *key, const char *id) {
    cJSON *names = cJSON_GetObjectItemCaseSensitive(store, "names");
    cJSON_DeleteItemFromObjectCaseSensitive(names, key);
    return cJSON_AddStringToObject(names, key, id) != NULL;
}

static cJSON *settled_answer(cJSON *store, const remembered_name *request, const char *key, const char *path, cJSON **refusal) {
    const char *was = text_of(cJSON_GetObjectItemCaseSensitive(store, "names"), key);
    int is_remembered = was != NULL && strcmp(was, request->id) == 0;
    int is_accepted = was != NULL && request->accept != NULL && strcmp(request->accept, request->id) == 0;
    if (was != NULL && !is_remembered && !is_accepted) {
        return needs_acceptance_answer(was, request->id);
    }
    if (!request->is_recording) {
        return settled_check_answer();
    }
    if (!is_remembered && !(records_id(store, key, request->id) && writes_store(store, request->state, path))) {
        return refuse(refusal, "check that the state place can be written", "the record of marketplace names could not be written to %s", path);
    }
    return remembered_answer();
}

static cJSON *remember(const cJSON *argument, cJSON **refusal) {
    remembered_name request;
    if (!reads_request(argument, &request, refusal)) {
        return NULL;
    }
    char *path = store_path(request.state);
    char *key = qualified_name(&request);
    cJSON *store = path == NULL || key == NULL ? NULL : loaded_store(path, refusal);
    cJSON *answer = store == NULL ? NULL : settled_answer(store, &request, key, path, refusal);
    cJSON_Delete(store);
    free(key);
    free(path);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_marketplace_remember(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, remember);
}
