#include "json_key_change.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "json_edit.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/javascript.h"
#include "spisor/json_file.h"
#include "spisor/path.h"

static void write_note(runtime_change_note *note, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(note->detail, sizeof note->detail, format, arguments);
    va_end(arguments);
}

static runtime_change_step refuse(runtime_change_note *note, const char *reason, const char *path) {
    write_note(note, "%s: %s", reason, path);
    return RUNTIME_CHANGE_REFUSED;
}

static runtime_change_step fail(runtime_change_note *note, const char *reason, const char *path) {
    write_note(note, "%s: %s", reason, path);
    return RUNTIME_CHANGE_FAILED;
}

static runtime_change_step out_of_memory(runtime_change_note *note) {
    write_note(note, "memory ran out");
    return RUNTIME_CHANGE_FAILED;
}

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static const cJSON *key_of(const cJSON *change) {
    return cJSON_GetObjectItemCaseSensitive(change, "key");
}

static int is_string_list(const cJSON *key) {
    if (!cJSON_IsArray(key) || cJSON_GetArraySize(key) == 0) {
        return 0;
    }
    const cJSON *segment;
    cJSON_ArrayForEach(segment, key) {
        if (!cJSON_IsString(segment)) {
            return 0;
        }
    }
    return 1;
}

const char *runtime_json_key_defect(const cJSON *change) {
    const char *file = text_of(change, "file");
    const cJSON *create = cJSON_GetObjectItemCaseSensitive(change, "create");
    if (file == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, file)) {
        return "a json-key change needs an absolute file";
    }
    if (!is_string_list(key_of(change))) {
        return "a json-key change needs its key as a non-empty array of strings";
    }
    if (create != NULL && !cJSON_IsBool(create)) {
        return "a json-key change's create is not a boolean";
    }
    return cJSON_GetObjectItemCaseSensitive(change, "value") != NULL ? NULL : "a json-key change needs a value";
}

static cJSON *child_object(cJSON *object, const cJSON *segment) {
    return cJSON_GetObjectItemCaseSensitive(object, segment->valuestring);
}

/** The object reached by walking the first depth segments of key, or NULL when one is missing or no object. */
static cJSON *object_at(cJSON *root, const cJSON *key, int depth) {
    cJSON *node = root;
    for (int index = 0; node != NULL && index < depth; index++) {
        node = child_object(node, cJSON_GetArrayItem(key, index));
        node = cJSON_IsObject(node) ? node : NULL;
    }
    return node;
}

static cJSON *value_at(cJSON *root, const cJSON *key) {
    int last = cJSON_GetArraySize(key) - 1;
    cJSON *parent = object_at(root, key, last);
    return parent == NULL ? NULL : child_object(parent, cJSON_GetArrayItem(key, last));
}

static cJSON *read_json_object(const char *path, runtime_change_note *note, runtime_change_step *step) {
    spisor_json_file_state state;
    cJSON *root = spisor_read_json_file(path, &state);
    if (state == SPISOR_JSON_FILE_ABSENT) {
        *step = refuse(note, "the JSON file does not exist", path);
    } else if (state != SPISOR_JSON_FILE_PARSED || !cJSON_IsObject(root)) {
        *step = refuse(note, "the file is not a readable JSON object; it was left unchanged", path);
        cJSON_Delete(root);
        root = NULL;
    }
    return root;
}

/** The depth of the first segment missing on the way to key, -1 when key is present, -2 when blocked. */
static int first_missing_depth(cJSON *root, const cJSON *key, runtime_change_note *note, const char *path) {
    int last = cJSON_GetArraySize(key) - 1;
    cJSON *node = root;
    for (int index = 0; index < last; index++) {
        cJSON *child = child_object(node, cJSON_GetArrayItem(key, index));
        if (child == NULL) {
            return index;
        }
        if (!cJSON_IsObject(child)) {
            write_note(note, "a key on the path is not an object, so the key cannot be set: %s", path);
            return -2;
        }
        node = child;
    }
    return child_object(node, cJSON_GetArrayItem(key, last)) == NULL ? last : -1;
}

static int holds_value_already(cJSON *root, const cJSON *change) {
    const cJSON *current = value_at(root, key_of(change));
    return current != NULL && cJSON_Compare(current, cJSON_GetObjectItemCaseSensitive(change, "value"), 1);
}

typedef struct file_bytes {
    char *bytes;
    size_t length;
} file_bytes;

static file_bytes read_bytes(const char *path) {
    file_bytes read = {NULL, 0};
    read.bytes = (char *)spisor_read_file(path, &read.length);
    return read;
}

static int add_or_replace(cJSON *record, const char *member, cJSON *value) {
    int set = value != NULL && (cJSON_GetObjectItemCaseSensitive(record, member) != NULL
                                    ? cJSON_ReplaceItemInObjectCaseSensitive(record, member, value)
                                    : cJSON_AddItemToObject(record, member, value));
    if (value != NULL && !set) {
        cJSON_Delete(value);
    }
    return set;
}

static int set_after(cJSON *record, const json_edit_text *written) {
    char digest[RUNTIME_CHANGE_DIGEST_SIZE];
    runtime_change_digest((const uint8_t *)written->bytes, written->length, digest);
    return add_or_replace(record, "after", cJSON_CreateString(digest));
}

/**
 * @implNote A registration is JSON, so the bytes go in as a string only when they are UTF-8 without a
 * NUL; any other file is reversed member by member, which is exact for every byte but a value's.
 */
static int is_storable_text(const file_bytes *bytes) {
    if (memchr(bytes->bytes, '\0', bytes->length) != NULL) {
        return 0;
    }
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy((const uint8_t *)bytes->bytes, bytes->length, &decoded_length);
    int same = decoded != NULL && decoded_length == bytes->length && memcmp(decoded, bytes->bytes, decoded_length) == 0;
    free(decoded);
    return same;
}

static int add_before(cJSON *record, const file_bytes *before) {
    return !is_storable_text(before) || add_or_replace(record, "before", cJSON_CreateString(before->bytes));
}

static int add_what_was_there(cJSON *record, cJSON *root, const cJSON *change, int missing_from) {
    if (missing_from >= 0) {
        return cJSON_AddNumberToObject(record, "createdFrom", missing_from) != NULL;
    }
    return add_or_replace(record, "previous", cJSON_Duplicate(value_at(root, key_of(change)), 1));
}

static runtime_change_step finish_record(cJSON **record, int complete, runtime_change_note *note) {
    if (complete) {
        return RUNTIME_CHANGE_DONE;
    }
    cJSON_Delete(*record);
    *record = NULL;
    return out_of_memory(note);
}

static const char *const recorded_members[] = {"file", "key", "value"};

static runtime_change_step record_edit(const cJSON *change, cJSON *root, int missing_from, const file_bytes *before,
                                       cJSON **record, runtime_change_note *note) {
    json_edit_text edited = {NULL, 0};
    if (!json_edit_set(before->bytes, before->length, key_of(change), cJSON_GetObjectItemCaseSensitive(change, "value"),
                       &edited)) {
        return refuse(note, "the file is not a readable JSON object; it was left unchanged", text_of(change, "file"));
    }
    *record = runtime_change_record_with(change, recorded_members, 3);
    int complete = *record != NULL && add_what_was_there(*record, root, change, missing_from) &&
                   add_before(*record, before) && set_after(*record, &edited);
    free(edited.bytes);
    return finish_record(record, complete, note);
}

static int may_create(const cJSON *change, const char *path) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(change, "create")) &&
           runtime_probe_entry(path) == RUNTIME_ENTRY_MISSING;
}

static runtime_change_step record_created(const cJSON *change, cJSON **record, runtime_change_note *note) {
    json_edit_text created = {NULL, 0};
    if (!json_edit_new_document(key_of(change), cJSON_GetObjectItemCaseSensitive(change, "value"), &created)) {
        return out_of_memory(note);
    }
    *record = runtime_change_record_with(change, recorded_members, 3);
    int complete = *record != NULL && cJSON_AddNumberToObject(*record, "createdFrom", 0) != NULL &&
                   cJSON_AddTrueToObject(*record, "created") != NULL && set_after(*record, &created);
    free(created.bytes);
    return finish_record(record, complete, note);
}

static runtime_change_step prepare_existing(const cJSON *change, const file_bytes *before, cJSON **record,
                                            runtime_change_note *note) {
    const char *path = text_of(change, "file");
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    cJSON *root = read_json_object(path, note, &step);
    if (root == NULL) {
        return step;
    }
    int missing_from = first_missing_depth(root, key_of(change), note, path);
    if (missing_from == -2) {
        step = RUNTIME_CHANGE_REFUSED;
    } else if (missing_from == -1 && holds_value_already(root, change)) {
        step = RUNTIME_CHANGE_UNNEEDED;
    } else {
        step = record_edit(change, root, missing_from, before, record, note);
    }
    cJSON_Delete(root);
    return step;
}

runtime_change_step runtime_json_key_prepare(const cJSON *change, cJSON **record, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    file_bytes before = read_bytes(path);
    if (before.bytes == NULL && may_create(change, path)) {
        return record_created(change, record, note);
    }
    if (before.bytes == NULL) {
        return runtime_probe_entry(path) == RUNTIME_ENTRY_MISSING
                   ? refuse(note, "the JSON file does not exist", path)
                   : refuse(note, "the file is not a readable JSON object; it was left unchanged", path);
    }
    runtime_change_step step = prepare_existing(change, &before, record, note);
    free(before.bytes);
    return step;
}

static runtime_change_step perform_created(const cJSON *change, cJSON *record, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    json_edit_text created = {NULL, 0};
    if (!json_edit_new_document(key_of(change), cJSON_GetObjectItemCaseSensitive(change, "value"), &created)) {
        return out_of_memory(note);
    }
    runtime_entry_outcome made = runtime_create_new_file(path, (const uint8_t *)created.bytes, created.length, 0);
    int recorded = made == RUNTIME_ENTRY_DONE && set_after(record, &created);
    free(created.bytes);
    if (made == RUNTIME_ENTRY_EXISTS) {
        return refuse(note, "something already exists where the JSON file would be created; it was left unchanged",
                      path);
    }
    if (made != RUNTIME_ENTRY_DONE) {
        return fail(note, "the JSON file could not be created (does its directory exist?)", path);
    }
    return recorded ? RUNTIME_CHANGE_DONE : out_of_memory(note);
}

static runtime_change_step perform_edit(const cJSON *change, cJSON *record, const file_bytes *current,
                                        runtime_change_note *note) {
    const char *path = text_of(change, "file");
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    cJSON *root = read_json_object(path, note, &step);
    cJSON_Delete(root);
    if (root == NULL) {
        return step;
    }
    json_edit_text edited = {NULL, 0};
    if (!json_edit_set(current->bytes, current->length, key_of(change),
                       cJSON_GetObjectItemCaseSensitive(change, "value"), &edited)) {
        return refuse(note, "the file is not a readable JSON object; it was left unchanged", path);
    }
    int written = spisor_replace_file(path, (const uint8_t *)edited.bytes, edited.length);
    int recorded = written && set_after(record, &edited);
    free(edited.bytes);
    if (!written) {
        return fail(note, "the key could not be written", path);
    }
    return recorded ? RUNTIME_CHANGE_DONE : out_of_memory(note);
}

runtime_change_step runtime_json_key_perform(const cJSON *change, cJSON *record, runtime_change_note *note) {
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "created"))) {
        return perform_created(change, record, note);
    }
    file_bytes current = read_bytes(text_of(change, "file"));
    if (current.bytes == NULL) {
        return fail(note, "the JSON file could not be read", text_of(change, "file"));
    }
    runtime_change_step step = perform_edit(change, record, &current, note);
    free(current.bytes);
    return step;
}

int runtime_json_key_is_on_disk(const cJSON *record) {
    spisor_json_file_state state;
    cJSON *root = spisor_read_json_file(text_of(record, "file"), &state);
    const cJSON *current = root == NULL ? NULL : value_at(root, key_of(record));
    int on_disk = current != NULL && cJSON_Compare(current, cJSON_GetObjectItemCaseSensitive(record, "value"), 1);
    cJSON_Delete(root);
    return on_disk;
}

static void key_text(const cJSON *key, char *text, size_t size) {
    size_t used = 0;
    text[0] = '\0';
    const cJSON *segment;
    cJSON_ArrayForEach(segment, key) {
        int wrote = snprintf(text + used, size - used, "%s%s", used == 0 ? "" : ".", segment->valuestring);
        if (wrote < 0 || (size_t)wrote >= size - used) {
            return;
        }
        used += (size_t)wrote;
    }
}

static runtime_change_reversal drifted_key(const cJSON *record, const char *why, runtime_change_note *note) {
    char key[512];
    key_text(key_of(record), key, sizeof key);
    write_note(note, "the key %s in %s %s; it was left", key, text_of(record, "file"), why);
    return RUNTIME_CHANGE_DRIFTED;
}

static runtime_change_reversal not_reversed(const cJSON *record, runtime_change_note *note) {
    write_note(note, "the key's previous state could not be written back to %s", text_of(record, "file"));
    return RUNTIME_CHANGE_NOT_REVERSED;
}

static int is_created(const cJSON *record) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "created"));
}

static int holds_what_was_written(const cJSON *record, const file_bytes *current) {
    char digest[RUNTIME_CHANGE_DIGEST_SIZE];
    runtime_change_digest((const uint8_t *)current->bytes, current->length, digest);
    const char *after = text_of(record, "after");
    return after != NULL && strcmp(after, digest) == 0;
}

static runtime_change_reversal remove_created_file(const cJSON *record, runtime_change_note *note) {
    return runtime_remove_file_entry(text_of(record, "file")) == RUNTIME_ENTRY_DONE ? RUNTIME_CHANGE_REVERSED
                                                                                     : not_reversed(record, note);
}

static runtime_change_reversal write_back(const cJSON *record, const char *bytes, size_t length,
                                          runtime_change_note *note) {
    return spisor_replace_file(text_of(record, "file"), (const uint8_t *)bytes, length) ? RUNTIME_CHANGE_REVERSED
                                                                                        : not_reversed(record, note);
}

/** The previous value's own bytes as the file held them, or failing that the value written in place. */
static int restore_previous(const cJSON *record, const file_bytes *current, json_edit_text *edited) {
    const char *before = text_of(record, "before");
    json_span previous;
    if (before != NULL && json_edit_find_value(before, strlen(before), key_of(record), &previous)) {
        return json_edit_replace_value(current->bytes, current->length, key_of(record), before + previous.start,
                                       previous.end - previous.start, edited);
    }
    return json_edit_set(current->bytes, current->length, key_of(record),
                         cJSON_GetObjectItemCaseSensitive(record, "previous"), edited);
}

static int created_from(const cJSON *record) {
    const cJSON *depth = cJSON_GetObjectItemCaseSensitive(record, "createdFrom");
    return cJSON_IsNumber(depth) ? depth->valueint : cJSON_GetArraySize(key_of(record)) - 1;
}

static runtime_change_reversal undo_member(const cJSON *record, const file_bytes *current, runtime_change_note *note) {
    json_edit_text edited = {NULL, 0};
    int done = cJSON_GetObjectItemCaseSensitive(record, "previous") != NULL
                   ? restore_previous(record, current, &edited)
                   : json_edit_remove(current->bytes, current->length, key_of(record), created_from(record), &edited);
    if (!done) {
        return not_reversed(record, note);
    }
    runtime_change_reversal reversal = is_created(record) && json_edit_is_empty_object(edited.bytes, edited.length)
                                           ? remove_created_file(record, note)
                                           : write_back(record, edited.bytes, edited.length, note);
    free(edited.bytes);
    return reversal;
}

static int is_already_restored(const cJSON *current, const cJSON *record) {
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(record, "previous");
    return previous == NULL ? current == NULL : current != NULL && cJSON_Compare(current, previous, 1);
}

static runtime_change_reversal reverse_drifted_file(const cJSON *record, const file_bytes *current,
                                                    runtime_change_note *note) {
    spisor_json_file_state state;
    cJSON *root = spisor_read_json_file(text_of(record, "file"), &state);
    if (state != SPISOR_JSON_FILE_PARSED || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return drifted_key(record, "is in a file that is no longer readable JSON", note);
    }
    const cJSON *now = value_at(root, key_of(record));
    runtime_change_reversal reversal;
    if (now != NULL && cJSON_Compare(now, cJSON_GetObjectItemCaseSensitive(record, "value"), 1)) {
        reversal = undo_member(record, current, note);
    } else if (is_already_restored(now, record)) {
        reversal = RUNTIME_CHANGE_REVERSED;
    } else {
        reversal = drifted_key(record, "no longer holds the value the host set", note);
    }
    cJSON_Delete(root);
    return reversal;
}

static runtime_change_reversal reverse_missing_file(const cJSON *record, runtime_change_note *note) {
    if (runtime_probe_entry(text_of(record, "file")) != RUNTIME_ENTRY_MISSING) {
        return drifted_key(record, "is in a file that is no longer readable JSON", note);
    }
    return is_created(record) ? RUNTIME_CHANGE_REVERSED : drifted_key(record, "is gone with its file", note);
}

runtime_change_reversal runtime_json_key_reverse(const cJSON *record, runtime_change_note *note) {
    file_bytes current = read_bytes(text_of(record, "file"));
    if (current.bytes == NULL) {
        return reverse_missing_file(record, note);
    }
    const char *before = text_of(record, "before");
    runtime_change_reversal reversal;
    if (holds_what_was_written(record, &current) && is_created(record)) {
        reversal = remove_created_file(record, note);
    } else if (holds_what_was_written(record, &current) && before != NULL) {
        reversal = write_back(record, before, strlen(before), note);
    } else {
        reversal = reverse_drifted_file(record, &current, note);
    }
    free(current.bytes);
    return reversal;
}
