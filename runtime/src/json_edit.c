#include "json_edit.h"

#include <stdlib.h>
#include <string.h>

#include "json_style.h"
#include "spisor/json_write.h"

/** Where the member named name stands in its object: the last one so named, and its neighbours. */
typedef struct member_place {
    json_member found;
    json_member previous;
    json_member next;
    int has_previous;
    int has_next;
} member_place;

typedef enum place_answer { PLACE_FOUND, PLACE_ABSENT, PLACE_MALFORMED } place_answer;

static const char *segment_at(const cJSON *key, int depth) {
    return cJSON_GetArrayItem(key, depth)->valuestring;
}

static int last_depth(const cJSON *key) {
    return cJSON_GetArraySize(key) - 1;
}

static int document_root(const char *text, size_t length, json_span *root) {
    return json_span_value(text, length, 0, root) && text[root->start] == '{' &&
           json_span_skip_space(text, length, root->end) == length;
}

static int is_object(const char *text, json_span value) {
    return text[value.start] == '{';
}

static place_answer locate_member(const char *text, json_span object, const char *name, member_place *place) {
    json_members walk;
    json_members_start(&walk, text, object.end, object);
    json_member member;
    json_member before = {{0, 0}, {0, 0}};
    int seen = 0;
    int found = 0;
    int stepped;
    while ((stepped = json_members_next(&walk, &member)) == 1) {
        if (found && !place->has_next) {
            place->next = member;
            place->has_next = 1;
        }
        int named = json_span_key_is(text, member.key, name);
        if (named < 0) {
            return PLACE_MALFORMED;
        }
        if (named) {
            place->found = member;
            place->previous = before;
            place->has_previous = seen;
            place->has_next = 0;
            found = 1;
        }
        before = member;
        seen = 1;
    }
    if (stepped < 0) {
        return PLACE_MALFORMED;
    }
    return found ? PLACE_FOUND : PLACE_ABSENT;
}

/** The object holding the member at key's depth: walked from root through key's first depth segments. */
static place_answer locate_object(const char *text, json_span root, const cJSON *key, int depth, json_span *object) {
    *object = root;
    for (int index = 0; index < depth; index++) {
        member_place place = {0};
        place_answer answer = locate_member(text, *object, segment_at(key, index), &place);
        if (answer != PLACE_FOUND) {
            return answer;
        }
        if (!is_object(text, place.found.value)) {
            return PLACE_MALFORMED;
        }
        *object = place.found.value;
    }
    return PLACE_FOUND;
}

static int splice(const char *text, size_t length, json_span replaced, const char *inserted, size_t inserted_length,
                  json_edit_text *edited) {
    size_t total = length - (replaced.end - replaced.start) + inserted_length;
    char *bytes = malloc(total + 1);
    if (bytes == NULL) {
        return 0;
    }
    memcpy(bytes, text, replaced.start);
    memcpy(bytes + replaced.start, inserted, inserted_length);
    memcpy(bytes + replaced.start + inserted_length, text + replaced.end, length - replaced.end);
    bytes[total] = '\0';
    edited->bytes = bytes;
    edited->length = total;
    return 1;
}

static int copy_of(const char *text, size_t length, json_edit_text *edited) {
    json_span nothing = {0, 0};
    return splice(text, length, nothing, "", 0, edited);
}

/** value inside one object per key segment after depth, the innermost holding value. */
static cJSON *nested_value(const cJSON *key, int depth, const cJSON *value) {
    cJSON *nested = cJSON_Duplicate(value, 1);
    for (int index = last_depth(key); nested != NULL && index > depth; index--) {
        cJSON *holder = cJSON_CreateObject();
        if (holder == NULL || !cJSON_AddItemToObject(holder, segment_at(key, index), nested)) {
            cJSON_Delete(holder);
            cJSON_Delete(nested);
            return NULL;
        }
        nested = holder;
    }
    return nested;
}

static json_span interior_of(json_span object) {
    json_span interior = {object.start + 1, object.end - 1};
    return interior;
}

/** The empty span just past the value of object's last member, or no member at all. */
static int after_last_member(const char *text, json_span object, json_span *point) {
    json_members walk;
    json_members_start(&walk, text, object.end, object);
    json_member member;
    int stepped;
    int any = 0;
    while ((stepped = json_members_next(&walk, &member)) == 1) {
        point->start = member.value.end;
        point->end = member.value.end;
        any = 1;
    }
    return stepped == 0 && any;
}

static int insert_member(const char *text, size_t length, json_span root, json_span object, const char *name,
                         const cJSON *value, json_edit_text *edited) {
    json_span point;
    int into_empty = !after_last_member(text, object, &point);
    if (into_empty) {
        point = interior_of(object);
    }
    json_style style = json_style_of(text, root, object);
    size_t member_length = 0;
    char *member = json_style_member_text(&style, into_empty, name, value, &member_length);
    int done = member != NULL && splice(text, length, point, member, member_length, edited);
    free(member);
    return done;
}

static int replace_with_value(const char *text, size_t length, json_span root, json_span object, json_span replaced,
                              const cJSON *value, json_edit_text *edited) {
    json_style style = json_style_of(text, root, object);
    size_t value_length = 0;
    char *written = json_style_value_text(&style, value, &value_length);
    int done = written != NULL && splice(text, length, replaced, written, value_length, edited);
    free(written);
    return done;
}

int json_edit_set(const char *text, size_t length, const cJSON *key, const cJSON *value, json_edit_text *edited) {
    json_span root;
    if (!document_root(text, length, &root)) {
        return 0;
    }
    json_span object = root;
    for (int depth = 0; depth <= last_depth(key); depth++) {
        member_place place = {0};
        place_answer answer = locate_member(text, object, segment_at(key, depth), &place);
        if (answer == PLACE_MALFORMED) {
            return 0;
        }
        if (answer == PLACE_ABSENT) {
            cJSON *nested = nested_value(key, depth, value);
            int done = nested != NULL && insert_member(text, length, root, object, segment_at(key, depth), nested,
                                                       edited);
            cJSON_Delete(nested);
            return done;
        }
        if (depth == last_depth(key)) {
            return replace_with_value(text, length, root, object, place.found.value, value, edited);
        }
        if (!is_object(text, place.found.value)) {
            return 0;
        }
        object = place.found.value;
    }
    return 0;
}

/** The bytes a removed member takes with it: the comma before it, or after it, or the whole interior. */
static json_span removed_span(json_span object, const member_place *place) {
    json_span removed;
    if (place->has_previous) {
        removed.start = place->previous.value.end;
        removed.end = place->found.value.end;
    } else if (place->has_next) {
        removed.start = place->found.key.start;
        removed.end = place->next.key.start;
    } else {
        removed = interior_of(object);
    }
    return removed;
}

/** 1 with edited when the member at depth was removed, 0 when nothing more is to go, -1 on failure. */
static int remove_one(const json_edit_text *current, const cJSON *key, int depth, int only_if_empty,
                      json_edit_text *edited) {
    json_span root;
    json_span object;
    if (!document_root(current->bytes, current->length, &root)) {
        return -1;
    }
    place_answer reached = locate_object(current->bytes, root, key, depth, &object);
    member_place place = {0};
    place_answer answer =
        reached == PLACE_FOUND ? locate_member(current->bytes, object, segment_at(key, depth), &place) : reached;
    if (answer == PLACE_MALFORMED) {
        return -1;
    }
    if (answer == PLACE_ABSENT ||
        (only_if_empty && !json_edit_is_empty_object(current->bytes + place.found.value.start,
                                                     place.found.value.end - place.found.value.start))) {
        return 0;
    }
    return splice(current->bytes, current->length, removed_span(object, &place), "", 0, edited) ? 1 : -1;
}

int json_edit_remove(const char *text, size_t length, const cJSON *key, int created_from, json_edit_text *edited) {
    json_edit_text current = {NULL, 0};
    if (!copy_of(text, length, &current)) {
        return 0;
    }
    for (int depth = last_depth(key); depth >= created_from && depth >= 0; depth--) {
        json_edit_text next = {NULL, 0};
        int removed = remove_one(&current, key, depth, depth != last_depth(key), &next);
        if (removed < 0) {
            free(current.bytes);
            return 0;
        }
        if (removed == 0) {
            break;
        }
        free(current.bytes);
        current = next;
    }
    *edited = current;
    return 1;
}

int json_edit_find_value(const char *text, size_t length, const cJSON *key, json_span *value) {
    json_span root;
    json_span object;
    member_place place = {0};
    if (!document_root(text, length, &root) ||
        locate_object(text, root, key, last_depth(key), &object) != PLACE_FOUND ||
        locate_member(text, object, segment_at(key, last_depth(key)), &place) != PLACE_FOUND) {
        return 0;
    }
    *value = place.found.value;
    return 1;
}

int json_edit_replace_value(const char *text, size_t length, const cJSON *key, const char *value_text,
                            size_t value_length, json_edit_text *edited) {
    json_span value;
    return json_edit_find_value(text, length, key, &value) &&
           splice(text, length, value, value_text, value_length, edited);
}

int json_edit_is_empty_object(const char *text, size_t length) {
    json_span root;
    json_member first;
    json_members walk;
    if (!document_root(text, length, &root)) {
        return 0;
    }
    json_members_start(&walk, text, length, root);
    return json_members_next(&walk, &first) == 0;
}

int json_edit_new_document(const cJSON *key, const cJSON *value, json_edit_text *created) {
    cJSON *document = nested_value(key, -1, value);
    char *pretty = document == NULL ? NULL : spisor_json_stringify_pretty(document);
    cJSON_Delete(document);
    if (pretty == NULL) {
        return 0;
    }
    json_span end = {strlen(pretty), strlen(pretty)};
    int done = splice(pretty, strlen(pretty), end, "\n", 1, created);
    free(pretty);
    return done;
}
