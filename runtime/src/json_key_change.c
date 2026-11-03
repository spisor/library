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

static const char *const unreadable_object = "the file is not a readable JSON object; it was left unchanged";

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

static const cJSON *value_of(const cJSON *change) {
    return cJSON_GetObjectItemCaseSensitive(change, "value");
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
    return value_of(change) != NULL ? NULL : "a json-key change needs a value";
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

/** The depth of the first segment missing on the way to key, -1 when key is present, -2 when blocked. */
static int first_missing_depth(cJSON *root, const cJSON *key) {
    int last = cJSON_GetArraySize(key) - 1;
    cJSON *node = root;
    for (int index = 0; index < last; index++) {
        cJSON *child = child_object(node, cJSON_GetArrayItem(key, index));
        if (child == NULL) {
            return index;
        }
        if (!cJSON_IsObject(child)) {
            return -2;
        }
        node = child;
    }
    return child_object(node, cJSON_GetArrayItem(key, last)) == NULL ? last : -1;
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

/** The JSON object the bytes hold, read as spisor_read_json_file reads a file; NULL when they hold none. */
static cJSON *object_in(const file_bytes *bytes) {
    cJSON *root = spisor_parse_json_text((const uint8_t *)bytes->bytes, bytes->length);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
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

static int set_digest(cJSON *record, const char *member, const char *bytes, size_t length) {
    char digest[RUNTIME_CHANGE_DIGEST_SIZE];
    runtime_change_digest((const uint8_t *)bytes, length, digest);
    return add_or_replace(record, member, cJSON_CreateString(digest));
}

static int digest_is(const cJSON *record, const char *member, const char *bytes, size_t length) {
    char digest[RUNTIME_CHANGE_DIGEST_SIZE];
    runtime_change_digest((const uint8_t *)bytes, length, digest);
    const char *recorded = text_of(record, member);
    return recorded != NULL && strcmp(recorded, digest) == 0;
}

/**
 * @implNote A registration is JSON, so bytes go in as a string only when they are UTF-8 without a NUL;
 * a replaced value that is not is undone through the drift path, which writes it in the file's style.
 */
static int is_storable_text(const char *bytes, size_t length) {
    if (memchr(bytes, '\0', length) != NULL) {
        return 0;
    }
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy((const uint8_t *)bytes, length, &decoded_length);
    int same = decoded != NULL && decoded_length == length && memcmp(decoded, bytes, length) == 0;
    free(decoded);
    return same;
}

static int set_replaced(cJSON *record, const file_bytes *before, const json_edit_splice *splice) {
    const char *replaced = before->bytes + splice->at;
    cJSON_DeleteItemFromObjectCaseSensitive(record, "replaced");
    if (!is_storable_text(replaced, splice->replaced_length)) {
        return 1;
    }
    char *copy = malloc(splice->replaced_length + 1);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, replaced, splice->replaced_length);
    copy[splice->replaced_length] = '\0';
    int set = add_or_replace(record, "replaced", cJSON_CreateString(copy));
    free(copy);
    return set;
}

static int set_number(cJSON *record, const char *member, double number) {
    return add_or_replace(record, member, cJSON_CreateNumber(number));
}

/** The inverse of the edit: where it was, the bytes it replaced, how many it wrote, and both digests. */
static int set_inverse(cJSON *record, const file_bytes *before, const json_edit_text *edited) {
    return set_number(record, "spliceAt", (double)edited->splice.at) &&
           set_number(record, "writtenLength", (double)edited->splice.written_length) &&
           set_replaced(record, before, &edited->splice) &&
           set_digest(record, "beforeDigest", before->bytes, before->length) &&
           set_digest(record, "after", edited->bytes, edited->length);
}

static int set_what_was_there(cJSON *record, cJSON *root, const cJSON *change, int missing_from) {
    cJSON_DeleteItemFromObjectCaseSensitive(record, "createdFrom");
    cJSON_DeleteItemFromObjectCaseSensitive(record, "previous");
    if (missing_from >= 0) {
        return set_number(record, "createdFrom", missing_from);
    }
    return add_or_replace(record, "previous", cJSON_Duplicate(value_at(root, key_of(change)), 1));
}

/**
 * Edits the bytes and makes record describe exactly that edit, so a reversal undoes what was written
 * whatever the file held when the change was prepared. edited is freed by the caller.
 */
static runtime_change_step describe_edit(const cJSON *change, cJSON *root, const file_bytes *before, cJSON *record,
                                         json_edit_text *edited, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    int missing_from = first_missing_depth(root, key_of(change));
    if (missing_from == -2) {
        return refuse(note, "a key on the path is not an object, so the key cannot be set", path);
    }
    if (!json_edit_set(before->bytes, before->length, key_of(change), value_of(change), edited)) {
        return refuse(note, unreadable_object, path);
    }
    return set_what_was_there(record, root, change, missing_from) && set_inverse(record, before, edited)
               ? RUNTIME_CHANGE_DONE
               : out_of_memory(note);
}

static const char *const recorded_members[] = {"file", "key", "value"};

static int may_create(const cJSON *change, const char *path) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(change, "create")) &&
           runtime_probe_entry(path) == RUNTIME_ENTRY_MISSING;
}

static runtime_change_step record_created(const cJSON *change, cJSON **record, runtime_change_note *note) {
    json_edit_text created = {NULL, 0, {0, 0, 0}};
    if (!json_edit_new_document(key_of(change), value_of(change), &created)) {
        return out_of_memory(note);
    }
    *record = runtime_change_record_with(change, recorded_members, 3);
    int complete = *record != NULL && set_number(*record, "createdFrom", 0) &&
                   cJSON_AddTrueToObject(*record, "created") != NULL &&
                   set_digest(*record, "after", created.bytes, created.length);
    free(created.bytes);
    if (!complete) {
        cJSON_Delete(*record);
        *record = NULL;
        return out_of_memory(note);
    }
    return RUNTIME_CHANGE_DONE;
}

static runtime_change_step record_edit(const cJSON *change, cJSON *root, const file_bytes *before, cJSON **record,
                                       runtime_change_note *note) {
    *record = runtime_change_record_with(change, recorded_members, 3);
    if (*record == NULL) {
        return out_of_memory(note);
    }
    json_edit_text edited = {NULL, 0, {0, 0, 0}};
    runtime_change_step step = describe_edit(change, root, before, *record, &edited, note);
    free(edited.bytes);
    if (step != RUNTIME_CHANGE_DONE) {
        cJSON_Delete(*record);
        *record = NULL;
    }
    return step;
}

static runtime_change_step prepare_existing(const cJSON *change, const file_bytes *before, cJSON **record,
                                            runtime_change_note *note) {
    cJSON *root = object_in(before);
    if (root == NULL) {
        return refuse(note, unreadable_object, text_of(change, "file"));
    }
    const cJSON *current = value_at(root, key_of(change));
    runtime_change_step step = current != NULL && cJSON_Compare(current, value_of(change), 1)
                                   ? RUNTIME_CHANGE_UNNEEDED
                                   : record_edit(change, root, before, record, note);
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
        return runtime_probe_entry(path) == RUNTIME_ENTRY_MISSING ? refuse(note, "the JSON file does not exist", path)
                                                                  : refuse(note, unreadable_object, path);
    }
    runtime_change_step step = prepare_existing(change, &before, record, note);
    free(before.bytes);
    return step;
}

static runtime_change_step perform_created(const cJSON *change, cJSON *record, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    json_edit_text created = {NULL, 0, {0, 0, 0}};
    if (!json_edit_new_document(key_of(change), value_of(change), &created)) {
        return out_of_memory(note);
    }
    runtime_entry_outcome made = runtime_create_new_file(path, (const uint8_t *)created.bytes, created.length, 0);
    int recorded = made == RUNTIME_ENTRY_DONE && set_digest(record, "after", created.bytes, created.length);
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

/** Validates, edits and writes the one buffer read, and records the edit made to it. */
static runtime_change_step perform_edit(const cJSON *change, cJSON *record, const file_bytes *current,
                                        runtime_change_note *note) {
    const char *path = text_of(change, "file");
    cJSON *root = object_in(current);
    if (root == NULL) {
        return refuse(note, unreadable_object, path);
    }
    json_edit_text edited = {NULL, 0, {0, 0, 0}};
    runtime_change_step step = describe_edit(change, root, current, record, &edited, note);
    cJSON_Delete(root);
    if (step == RUNTIME_CHANGE_DONE && !spisor_replace_file(path, (const uint8_t *)edited.bytes, edited.length)) {
        step = fail(note, "the key could not be written", path);
    }
    free(edited.bytes);
    return step;
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
    int on_disk = current != NULL && cJSON_Compare(current, value_of(record), 1);
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

static runtime_change_reversal remove_created_file(const cJSON *record, runtime_change_note *note) {
    return runtime_remove_file_entry(text_of(record, "file")) == RUNTIME_ENTRY_DONE ? RUNTIME_CHANGE_REVERSED
                                                                                     : not_reversed(record, note);
}

static runtime_change_reversal write_back(const cJSON *record, const json_edit_text *restored,
                                          runtime_change_note *note) {
    return spisor_replace_file(text_of(record, "file"), (const uint8_t *)restored->bytes, restored->length)
               ? RUNTIME_CHANGE_REVERSED
               : not_reversed(record, note);
}

static int number_of(const cJSON *record, const char *member, size_t *number) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(record, member);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return 0;
    }
    *number = (size_t)item->valuedouble;
    return 1;
}

/** The file before the change, by undoing the recorded splice; 0 unless that lands exactly on the pre-image. */
static int undo_splice(const cJSON *record, const file_bytes *current, json_edit_text *restored) {
    const char *replaced = text_of(record, "replaced");
    size_t at = 0;
    size_t written = 0;
    if (replaced == NULL || !number_of(record, "spliceAt", &at) || !number_of(record, "writtenLength", &written) ||
        !json_edit_splice_text(current->bytes, current->length, at, written, replaced, strlen(replaced), restored)) {
        return 0;
    }
    if (digest_is(record, "beforeDigest", restored->bytes, restored->length)) {
        return 1;
    }
    free(restored->bytes);
    restored->bytes = NULL;
    return 0;
}

/** The previous value's own bytes as the file held them, or failing that the value written in place. */
static int restore_previous(const cJSON *record, const file_bytes *current, json_edit_text *edited) {
    const char *replaced = text_of(record, "replaced");
    if (replaced != NULL) {
        return json_edit_replace_value(current->bytes, current->length, key_of(record), replaced, strlen(replaced),
                                       edited);
    }
    return json_edit_set(current->bytes, current->length, key_of(record),
                         cJSON_GetObjectItemCaseSensitive(record, "previous"), edited);
}

static int created_from(const cJSON *record) {
    const cJSON *depth = cJSON_GetObjectItemCaseSensitive(record, "createdFrom");
    return cJSON_IsNumber(depth) ? depth->valueint : cJSON_GetArraySize(key_of(record)) - 1;
}

static runtime_change_reversal write_undone(const cJSON *record, const file_bytes *current,
                                            const json_edit_text *edited, runtime_change_note *note) {
    if (is_created(record) && json_edit_is_empty_object(edited->bytes, edited->length)) {
        return remove_created_file(record, note);
    }
    if (edited->length == current->length && memcmp(edited->bytes, current->bytes, current->length) == 0) {
        return RUNTIME_CHANGE_REVERSED;
    }
    return write_back(record, edited, note);
}

/** Undoes only the change's own member in the bytes as they now are: the value put back, or the member removed. */
static runtime_change_reversal undo_member(const cJSON *record, const file_bytes *current, runtime_change_note *note) {
    json_edit_text edited = {NULL, 0, {0, 0, 0}};
    int done = cJSON_GetObjectItemCaseSensitive(record, "previous") != NULL
                   ? restore_previous(record, current, &edited)
                   : json_edit_remove(current->bytes, current->length, key_of(record), created_from(record), &edited);
    runtime_change_reversal reversal = done ? write_undone(record, current, &edited, note) : not_reversed(record, note);
    free(edited.bytes);
    return reversal;
}

static runtime_change_reversal reverse_drifted_file(const cJSON *record, const file_bytes *current,
                                                    runtime_change_note *note) {
    cJSON *root = object_in(current);
    if (root == NULL) {
        return drifted_key(record, "is in a file that is no longer readable JSON", note);
    }
    const cJSON *now = value_at(root, key_of(record));
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(record, "previous");
    runtime_change_reversal reversal;
    if (now != NULL && cJSON_Compare(now, value_of(record), 1)) {
        reversal = undo_member(record, current, note);
    } else if (now == NULL && previous == NULL) {
        reversal = undo_member(record, current, note);
    } else if (now != NULL && previous != NULL && cJSON_Compare(now, previous, 1)) {
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

/**
 * @implNote The exact path rewrites the file only when its bytes are the ones the change wrote and
 * undoing the splice lands on the pre-image's digest, so it can never put back anything else.
 */
static runtime_change_reversal reverse_present_file(const cJSON *record, const file_bytes *current,
                                                    runtime_change_note *note) {
    if (!digest_is(record, "after", current->bytes, current->length)) {
        return reverse_drifted_file(record, current, note);
    }
    if (is_created(record)) {
        return remove_created_file(record, note);
    }
    json_edit_text restored = {NULL, 0, {0, 0, 0}};
    runtime_change_reversal reversal = undo_splice(record, current, &restored) ? write_back(record, &restored, note)
                                                                                : reverse_drifted_file(record, current,
                                                                                                       note);
    free(restored.bytes);
    return reversal;
}

runtime_change_reversal runtime_json_key_reverse(const cJSON *record, runtime_change_note *note) {
    file_bytes current = read_bytes(text_of(record, "file"));
    if (current.bytes == NULL) {
        return reverse_missing_file(record, note);
    }
    runtime_change_reversal reversal = reverse_present_file(record, &current, note);
    free(current.bytes);
    return reversal;
}
