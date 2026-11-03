#ifndef RUNTIME_JSON_EDIT_H
#define RUNTIME_JSON_EDIT_H

#include <stddef.h>

#include "cJSON.h"
#include "json_span.h"

/** Where an edit changed the text: replaced_length bytes at at became written_length bytes. */
typedef struct json_edit_splice {
    size_t at;
    size_t replaced_length;
    size_t written_length;
} json_edit_splice;

/** Bytes an edit produced, freed with free; NUL terminated one past length, splice being the last edit made. */
typedef struct json_edit_text {
    char *bytes;
    size_t length;
    json_edit_splice splice;
} json_edit_text;

/**
 * text with the member at key (an array of strings) set to value, every other byte kept: a present
 * member has only its value's bytes replaced; an absent one is added as the last member of its
 * object, inside any missing objects on the way, in the object's own style (json_style.h). 0 when
 * the text is malformed, a key on the way is not an object, or memory runs out.
 *
 * @implNote A key the text holds twice is the LAST one, the member JSON.parse keeps.
 */
int json_edit_set(const char *text, size_t length, const cJSON *key, const cJSON *value, json_edit_text *edited);

/**
 * text without the member at key, then without each object from key's depth created_from on that is
 * left empty, whether or not the member was still there, every other byte kept. A removed only member
 * takes the blanks before it, and the braces are emptied when nothing but blanks would stay.
 */
int json_edit_remove(const char *text, size_t length, const cJSON *key, int created_from, json_edit_text *edited);

/** text with the bytes of the value at key replaced by value_text; 0 when the member is absent. */
int json_edit_replace_value(const char *text, size_t length, const cJSON *key, const char *value_text,
                            size_t value_length, json_edit_text *edited);

/** The span of the value at key; 0 when it is absent or the text is malformed. */
int json_edit_find_value(const char *text, size_t length, const cJSON *key, json_span *value);

/** Whether text is an object with no member. */
int json_edit_is_empty_object(const char *text, size_t length);

/** text with removed_length bytes at at replaced by inserted; 0 when that is past the end or memory runs out. */
int json_edit_splice_text(const char *text, size_t length, size_t at, size_t removed_length, const char *inserted,
                          size_t inserted_length, json_edit_text *edited);

/** A new file's bytes holding only key set to value: JSON.stringify(document, null, 2) and a LF. */
int json_edit_new_document(const cJSON *key, const cJSON *value, json_edit_text *created);

#endif
