#include "marketplace_memory.h"
#include "allocation.h"

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
#define STORE_FORMAT 2
#define MESSAGE_SIZE 2048

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

static int is_text(const char *text) {
    return text != NULL && text[0] != '\0';
}

static int are_places(const char *const *at, size_t count) {
    if (at == NULL || count == 0) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_text(at[index]) || strchr(at[index], ',') != NULL) {
            return 0;
        }
    }
    return 1;
}

static int is_usable_request(const runtime_marketplace_remember_request *request, cJSON **refusal) {
    if (!is_text(request->state) || !runtime_is_usable_home(request->state) || !is_text(request->name) ||
        !is_text(request->id) || !are_places(request->at, request->at_count) ||
        (request->marketplace != NULL && !is_text(request->marketplace)) ||
        (request->accept != NULL && !is_text(request->accept)) ||
        (request->accept_at != NULL && !is_text(request->accept_at))) {
        refuse(refusal, "pass state, name, id and at and, when they are known, marketplace, accept and acceptAt",
               "the call carries no usable state, name, id or at, or a marketplace, accept or acceptAt that is not usable");
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

static int is_place_list(const cJSON *at) {
    if (!cJSON_IsArray(at) || at->child == NULL) {
        return 0;
    }
    for (const cJSON *place = at->child; place != NULL; place = place->next) {
        if (!cJSON_IsString(place) || place->valuestring[0] == '\0') {
            return 0;
        }
    }
    return 1;
}

static int is_store(const cJSON *store) {
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(store, "format");
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(store, "names");
    if (!cJSON_IsObject(store) || !cJSON_IsNumber(format) || format->valuedouble != STORE_FORMAT || !cJSON_IsObject(names)) {
        return 0;
    }
    for (const cJSON *entry = names->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsObject(entry) || text_of(entry, "id") == NULL ||
            !is_place_list(cJSON_GetObjectItemCaseSensitive(entry, "at"))) {
            return 0;
        }
    }
    return 1;
}

/** The store at path, empty when there is no file; NULL with *refusal set for one that cannot be read or is no store. */
static cJSON *loaded_store(const char *path, cJSON **refusal) {
    spisor_file_presence presence = spisor_probe_file(path);
    if (presence == SPISOR_FILE_OUT_OF_MEMORY) {
        return NULL;
    }
    if (presence != SPISOR_FILE_PRESENT) {
        return empty_store();
    }
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    int out_of_memory = 0;
    cJSON *store = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, &out_of_memory);
    spisor_memory_release(bytes);
    if (outcome == SPISOR_READ_OUT_OF_MEMORY || out_of_memory) {
        return NULL;
    }
    if (!is_store(store)) {
        cJSON_Delete(store);
        return refuse(refusal,
                      "remove the file, whose names are then remembered again on their next install, or repair it; it "
                      "is left as it was",
                      "%s is not a record of marketplace names: it is not JSON of {format: 2, names: {name: {id, at}}}",
                      path);
    }
    return store;
}

static spisor_status writes_store(const cJSON *store, const char *state, const char *path) {
    char *pretty = spisor_json_stringify_pretty(store);
    size_t length = pretty == NULL ? 0 : strlen(pretty);
    char *text = pretty == NULL ? NULL : spisor_memory_reallocate(pretty, length + 2);
    spisor_status written = SPISOR_OUT_OF_MEMORY;
    if (text != NULL) {
        text[length] = '\n';
        text[length + 1] = '\0';
        written = spisor_make_directories(state);
    }
    if (written == SPISOR_OK) {
        written = spisor_replace_file(path, (const uint8_t *)text, length + 1);
    }
    spisor_memory_release(text == NULL ? pretty : text);
    return written;
}

static char *qualified_name(const runtime_marketplace_remember_request *request) {
    if (request->marketplace == NULL) {
        size_t bare_length = strlen(request->name) + 1;
        char *bare = spisor_memory_allocate(bare_length);
        if (bare != NULL) {
            memcpy(bare, request->name, bare_length);
        }
        return bare;
    }
    size_t length = strlen(request->marketplace) + strlen(request->name) + 2;
    char *key = spisor_memory_allocate(length);
    if (key != NULL) {
        snprintf(key, length, "%s/%s", request->marketplace, request->name);
    }
    return key;
}

static cJSON *flag_answer(const char *flag) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, flag) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *needs_acceptance_answer(const char *was, const char *now) {
    cJSON *answer = flag_answer("needsUserAcceptance");
    if (answer != NULL &&
        (cJSON_AddStringToObject(answer, "was", was) == NULL || cJSON_AddStringToObject(answer, "now", now) == NULL)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *places_json(const char *const *at, size_t count) {
    cJSON *places = cJSON_CreateArray();
    for (size_t index = 0; places != NULL && index < count; index++) {
        cJSON *place = cJSON_CreateString(at[index]);
        if (place == NULL || !cJSON_AddItemToArray(places, place)) {
            cJSON_Delete(place);
            cJSON_Delete(places);
            return NULL;
        }
    }
    return places;
}

static cJSON *needs_at_acceptance_answer(const cJSON *was, const runtime_marketplace_remember_request *request) {
    cJSON *answer = flag_answer("needsAtAcceptance");
    cJSON *was_copy = answer == NULL ? NULL : cJSON_Duplicate(was, 1);
    cJSON *now = was_copy == NULL ? NULL : places_json(request->at, request->at_count);
    if (now == NULL || !cJSON_AddItemToObject(answer, "wasAt", was_copy)) {
        cJSON_Delete(now);
        cJSON_Delete(was_copy);
        cJSON_Delete(answer);
        return NULL;
    }
    if (!cJSON_AddItemToObject(answer, "nowAt", now)) {
        cJSON_Delete(now);
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int is_same_at(const cJSON *remembered, const char *const *at, size_t count) {
    size_t index = 0;
    for (const cJSON *place = remembered->child; place != NULL; place = place->next, index++) {
        if (index == count || strcmp(place->valuestring, at[index]) != 0) {
            return 0;
        }
    }
    return index == count;
}

/** Whether `accepted`, places joined by commas, is the offered `at` exactly. */
static int is_accepted_at(const char *accepted, const char *const *at, size_t count) {
    const char *cursor = accepted;
    for (size_t index = 0; index < count; index++) {
        size_t length = strlen(at[index]);
        if (strncmp(cursor, at[index], length) != 0 || (cursor[length] != ',' && cursor[length] != '\0')) {
            return 0;
        }
        cursor += length;
        if (*cursor == ',') {
            cursor++;
        } else if (index + 1 < count) {
            return 0;
        }
    }
    return *cursor == '\0';
}

static int records_entry(cJSON *store, const char *key, const runtime_marketplace_remember_request *request) {
    cJSON *names = cJSON_GetObjectItemCaseSensitive(store, "names");
    cJSON_DeleteItemFromObjectCaseSensitive(names, key);
    cJSON *entry = cJSON_AddObjectToObject(names, key);
    cJSON *at = entry == NULL || cJSON_AddStringToObject(entry, "id", request->id) == NULL
                    ? NULL
                    : places_json(request->at, request->at_count);
    if (at == NULL || !cJSON_AddItemToObject(entry, "at", at)) {
        cJSON_Delete(at);
        return 0;
    }
    return 1;
}

/** NULL when the remembered entry settles the request; otherwise the answer asking the user, or NULL with *asked. */
static cJSON *question_for(const cJSON *remembered, const runtime_marketplace_remember_request *request, int *asked,
                           int *changed) {
    *asked = 0;
    *changed = remembered == NULL;
    if (remembered == NULL) {
        return NULL;
    }
    const char *was = text_of(remembered, "id");
    const cJSON *was_at = cJSON_GetObjectItemCaseSensitive(remembered, "at");
    if (strcmp(was, request->id) != 0) {
        if (request->accept != NULL && strcmp(request->accept, request->id) == 0) {
            *changed = 1;
            return NULL;
        }
        *asked = 1;
        return needs_acceptance_answer(was, request->id);
    }
    if (!is_same_at(was_at, request->at, request->at_count)) {
        if (request->accept_at != NULL && is_accepted_at(request->accept_at, request->at, request->at_count)) {
            *changed = 1;
            return NULL;
        }
        *asked = 1;
        return needs_at_acceptance_answer(was_at, request);
    }
    return NULL;
}

static cJSON *settled_answer(cJSON *store, const runtime_marketplace_remember_request *request, const char *key,
                             const char *path, cJSON **refusal) {
    const cJSON *remembered = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(store, "names"), key);
    int asked = 0;
    int changed = 0;
    cJSON *question = question_for(remembered, request, &asked, &changed);
    if (asked) {
        return question;
    }
    if (!request->is_recording) {
        return flag_answer("settled");
    }
    cJSON *answer = flag_answer("remembered");
    if (answer == NULL || !changed) {
        return answer;
    }
    if (!records_entry(store, key, request)) {
        cJSON_Delete(answer);
        return NULL;
    }
    spisor_status written = writes_store(store, request->state, path);
    if (written != SPISOR_OK) {
        cJSON_Delete(answer);
        return written == SPISOR_OUT_OF_MEMORY ? NULL : refuse(refusal, "check that the state place can be written",
                      "the record of marketplace names could not be written to %s", path);
    }
    return answer;
}

cJSON *runtime_marketplace_remember(const runtime_marketplace_remember_request *request, cJSON **refusal) {
    *refusal = NULL;
    if (!is_usable_request(request, refusal)) {
        return NULL;
    }
    char *path = store_path(request->state);
    char *key = qualified_name(request);
    cJSON *store = path == NULL || key == NULL ? NULL : loaded_store(path, refusal);
    cJSON *answer = store == NULL ? NULL : settled_answer(store, request, key, path, refusal);
    cJSON_Delete(store);
    spisor_memory_release(key);
    spisor_memory_release(path);
    return answer;
}
