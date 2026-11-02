#include "change_kinds.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "path_entry.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

typedef struct text_buffer {
    char *bytes;
    size_t length;
} text_buffer;

typedef struct line_span {
    size_t start;
    size_t end;
    int found;
} line_span;

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

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static int is_kind(const cJSON *change, const char *kind) {
    const char *actual = text_of(change, "kind");
    return actual != NULL && strcmp(actual, kind) == 0;
}

static int is_absolute_member(const cJSON *change, const char *member) {
    const char *path = text_of(change, member);
    return path != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, path);
}

static int add_copy(cJSON *target, const cJSON *source, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(source, member);
    cJSON *copy = item == NULL ? NULL : cJSON_Duplicate(item, 1);
    if (copy != NULL && !cJSON_AddItemToObject(target, member, copy)) {
        cJSON_Delete(copy);
        copy = NULL;
    }
    return copy != NULL;
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

static int is_single_line(const char *line) {
    return line != NULL && line[0] != '\0' && strchr(line, '\n') == NULL && strchr(line, '\r') == NULL;
}

static const char *file_defect(const cJSON *change) {
    const cJSON *executable = cJSON_GetObjectItemCaseSensitive(change, "executable");
    int has_content = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(change, "content"));
    int has_source = cJSON_GetObjectItemCaseSensitive(change, "source") != NULL;
    if (!is_absolute_member(change, "path")) {
        return "a file change needs an absolute path";
    }
    if (has_content == has_source || (has_source && !is_absolute_member(change, "source"))) {
        return "a file change needs either its content as a string or an absolute source file to copy";
    }
    return executable == NULL || cJSON_IsBool(executable) ? NULL : "a file change's executable is not a boolean";
}

static const char *json_key_defect(const cJSON *change) {
    if (!is_absolute_member(change, "file")) {
        return "a json-key change needs an absolute file";
    }
    if (!is_string_list(cJSON_GetObjectItemCaseSensitive(change, "key"))) {
        return "a json-key change needs its key as a non-empty array of strings";
    }
    return cJSON_GetObjectItemCaseSensitive(change, "value") != NULL ? NULL : "a json-key change needs a value";
}

static const char *profile_line_defect(const cJSON *change) {
    const cJSON *create = cJSON_GetObjectItemCaseSensitive(change, "create");
    if (!is_absolute_member(change, "file")) {
        return "a profile-line change needs an absolute file";
    }
    if (create != NULL && !cJSON_IsBool(create)) {
        return "a profile-line change's create is not a boolean";
    }
    return is_single_line(text_of(change, "line")) ? NULL : "a profile-line change needs one non-empty line";
}

static const char *path_entry_defect(const cJSON *change) {
    if (!is_absolute_member(change, "directory")) {
        return "a path-entry change needs an absolute directory";
    }
    if (!is_absolute_member(change, "home")) {
        return "a path-entry change needs the user's home as an absolute path";
    }
    const cJSON *shell = cJSON_GetObjectItemCaseSensitive(change, "shell");
    if (shell != NULL && !cJSON_IsString(shell)) {
        return "a path-entry change's shell is not a string";
    }
    return runtime_path_entry_defect(text_of(change, "directory"));
}

const char *runtime_change_defect(const cJSON *change) {
    if (!cJSON_IsObject(change)) {
        return "the change is not an object";
    }
    if (is_kind(change, "file")) {
        return file_defect(change);
    }
    if (is_kind(change, "directory")) {
        return is_absolute_member(change, "path") ? NULL : "a directory change needs an absolute path";
    }
    if (is_kind(change, "json-key")) {
        return json_key_defect(change);
    }
    if (is_kind(change, "profile-line")) {
        return profile_line_defect(change);
    }
    if (is_kind(change, "path-entry")) {
        return path_entry_defect(change);
    }
    return "the change's kind is not one runtime can record (file, directory, json-key, profile-line, path-entry)";
}

int runtime_change_is_shared(const char *kind) {
    return kind != NULL &&
           (strcmp(kind, "directory") == 0 || strcmp(kind, "profile-line") == 0 || strcmp(kind, "path-entry") == 0);
}

static int same_member(const cJSON *left, const cJSON *right, const char *member) {
    const char *left_text = text_of(left, member);
    const char *right_text = text_of(right, member);
    return left_text != NULL && right_text != NULL && strcmp(left_text, right_text) == 0;
}

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

static int is_root(const char *path, size_t length) {
    if (RUNTIME_HOST_IS_WINDOWS && length == 3 && path[1] == ':') {
        return 1;
    }
    return length == 1;
}

static char *normal_path(const char *path) {
    const char *segments[1] = {path};
    char *joined = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 1);
    if (joined == NULL) {
        return NULL;
    }
    size_t length = strlen(joined);
    while (length > 1 && is_separator(joined[length - 1]) && !is_root(joined, length)) {
        joined[--length] = '\0';
    }
    return joined;
}

static char folded(char character) {
    return RUNTIME_HOST_IS_WINDOWS && character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
}

static int same_folded(const char *left, const char *right) {
    while (*left != '\0' && folded(*left) == folded(*right)) {
        left++;
        right++;
    }
    return folded(*left) == folded(*right);
}

int runtime_change_paths_equal(const char *left, const char *right) {
    char *left_normal = left == NULL ? NULL : normal_path(left);
    char *right_normal = right == NULL ? NULL : normal_path(right);
    int equal = left_normal != NULL && right_normal != NULL && same_folded(left_normal, right_normal);
    free(left_normal);
    free(right_normal);
    return equal;
}

static int same_path_member(const cJSON *left, const cJSON *right, const char *member) {
    return runtime_change_paths_equal(text_of(left, member), text_of(right, member));
}

static int normalize_member(cJSON *change, const char *member) {
    const char *path = text_of(change, member);
    if (path == NULL) {
        return 1;
    }
    char *normal = normal_path(path);
    cJSON *value = normal == NULL ? NULL : cJSON_CreateString(normal);
    free(normal);
    int replaced = value != NULL && cJSON_ReplaceItemInObjectCaseSensitive(change, member, value);
    if (value != NULL && !replaced) {
        cJSON_Delete(value);
    }
    return replaced;
}

cJSON *runtime_change_normalized(const cJSON *change) {
    cJSON *copy = cJSON_Duplicate(change, 1);
    int normal = copy != NULL && normalize_member(copy, "path") && normalize_member(copy, "file") &&
                 normalize_member(copy, "directory") && normalize_member(copy, "home") &&
                 normalize_member(copy, "source");
    if (copy != NULL && !normal) {
        cJSON_Delete(copy);
        copy = NULL;
    }
    return copy;
}

int runtime_change_same_target(const cJSON *left, const cJSON *right) {
    if (!same_member(left, right, "kind")) {
        return 0;
    }
    if (is_kind(left, "directory")) {
        return same_path_member(left, right, "path");
    }
    if (is_kind(left, "profile-line")) {
        return same_path_member(left, right, "file") && same_member(left, right, "line");
    }
    if (is_kind(left, "path-entry")) {
        return same_path_member(left, right, "directory") &&
               (RUNTIME_HOST_IS_WINDOWS || same_path_member(left, right, "home"));
    }
    return 0;
}

int runtime_change_repeats(const cJSON *record, const cJSON *change) {
    if (!same_member(record, change, "kind")) {
        return 0;
    }
    if (is_kind(change, "file")) {
        return same_path_member(record, change, "path");
    }
    if (is_kind(change, "json-key")) {
        return same_path_member(record, change, "file") &&
               cJSON_Compare(cJSON_GetObjectItemCaseSensitive(record, "key"),
                             cJSON_GetObjectItemCaseSensitive(change, "key"), 1) &&
               cJSON_Compare(cJSON_GetObjectItemCaseSensitive(record, "value"),
                             cJSON_GetObjectItemCaseSensitive(change, "value"), 1);
    }
    return runtime_change_same_target(record, change);
}

static cJSON *record_with(const cJSON *change, const char *const *members, size_t count) {
    cJSON *record = cJSON_CreateObject();
    for (size_t index = 0; record != NULL && index < count; index++) {
        if (!add_copy(record, change, members[index])) {
            cJSON_Delete(record);
            record = NULL;
        }
    }
    return record;
}

static runtime_change_step out_of_memory(runtime_change_note *note) {
    write_note(note, "memory ran out");
    return RUNTIME_CHANGE_FAILED;
}

/**
 * The digest a file record keeps of the content it was asked to hold, as "fnv1a64:" and 16 hex digits.
 *
 * @implNote It only tells one host's earlier content from its new content, which nobody chooses to
 * collide, so a 64-bit FNV-1a is enough and keeps the library free of a cryptographic hash.
 */
static void content_digest(const uint8_t *bytes, size_t length, char digest[RUNTIME_CHANGE_DIGEST_SIZE]) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t index = 0; index < length; index++) {
        hash = (hash ^ bytes[index]) * 1099511628211ull;
    }
    snprintf(digest, RUNTIME_CHANGE_DIGEST_SIZE, "fnv1a64:%016llx", (unsigned long long)hash);
}

typedef struct file_bytes {
    uint8_t *bytes;
    size_t length;
} file_bytes;

/** The bytes a file change asks for: its content, or what its source file holds; bytes is NULL when unreadable. */
static file_bytes bytes_asked_for(const cJSON *change) {
    file_bytes asked = {NULL, 0};
    const char *content = text_of(change, "content");
    if (content == NULL) {
        asked.bytes = spisor_read_file(text_of(change, "source"), &asked.length);
        return asked;
    }
    asked.length = strlen(content);
    asked.bytes = malloc(asked.length + 1);
    if (asked.bytes != NULL) {
        memcpy(asked.bytes, content, asked.length + 1);
    }
    return asked;
}

static int set_digest(cJSON *record, const file_bytes *asked) {
    char digest[RUNTIME_CHANGE_DIGEST_SIZE];
    content_digest(asked->bytes, asked->length, digest);
    cJSON *value = cJSON_CreateString(digest);
    int set = value != NULL && (cJSON_GetObjectItemCaseSensitive(record, "digest") != NULL
                                    ? cJSON_ReplaceItemInObjectCaseSensitive(record, "digest", value)
                                    : cJSON_AddItemToObject(record, "digest", value));
    if (value != NULL && !set) {
        cJSON_Delete(value);
    }
    return set;
}

static runtime_change_step prepare_file(const cJSON *change, cJSON **record, runtime_change_note *note) {
    const char *path = text_of(change, "path");
    if (runtime_probe_entry(path) != RUNTIME_ENTRY_MISSING) {
        return refuse(note, "something already exists where the file would be created; it was left unchanged", path);
    }
    file_bytes asked = bytes_asked_for(change);
    if (asked.bytes == NULL) {
        return refuse(note, "the file to copy cannot be read", text_of(change, "source"));
    }
    static const char *const members[] = {"path"};
    *record = record_with(change, members, 1);
    if (*record != NULL && !set_digest(*record, &asked)) {
        cJSON_Delete(*record);
        *record = NULL;
    }
    free(asked.bytes);
    return *record != NULL ? RUNTIME_CHANGE_DONE : out_of_memory(note);
}

int runtime_change_asks_new_content(const cJSON *record, const cJSON *change) {
    if (!is_kind(record, "file") || !is_kind(change, "file")) {
        return 0;
    }
    file_bytes asked = bytes_asked_for(change);
    if (asked.bytes == NULL) {
        return 1;
    }
    char digest[RUNTIME_CHANGE_DIGEST_SIZE];
    content_digest(asked.bytes, asked.length, digest);
    free(asked.bytes);
    const char *recorded = text_of(record, "digest");
    return recorded == NULL || strcmp(recorded, digest) != 0;
}

static runtime_entry_outcome write_content(const char *path, const file_bytes *asked, int executable) {
    runtime_entry_kind found = runtime_probe_entry(path);
    if (found == RUNTIME_ENTRY_MISSING) {
        return runtime_create_new_file(path, asked->bytes, asked->length, executable);
    }
    if (found != RUNTIME_ENTRY_OTHER || spisor_is_link(path)) {
        return RUNTIME_ENTRY_KEPT;
    }
    return spisor_replace_file(path, asked->bytes, asked->length) ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

runtime_change_step runtime_change_rewrite(const cJSON *change, cJSON *record, runtime_change_note *note) {
    const char *path = text_of(change, "path");
    file_bytes asked = bytes_asked_for(change);
    if (asked.bytes == NULL) {
        return refuse(note, "the file to copy cannot be read", text_of(change, "source"));
    }
    runtime_entry_outcome written =
        write_content(path, &asked, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(change, "executable")));
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    if (written == RUNTIME_ENTRY_KEPT || written == RUNTIME_ENTRY_EXISTS) {
        step = refuse(note, "a directory or link now stands where the host's file was; it was left unchanged", path);
    } else if (written != RUNTIME_ENTRY_DONE) {
        step = fail(note, "the host's file could not be rewritten", path);
    } else if (!set_digest(record, &asked)) {
        step = out_of_memory(note);
    }
    free(asked.bytes);
    return step;
}

static runtime_change_step perform_file(const cJSON *change, runtime_change_note *note) {
    const char *path = text_of(change, "path");
    int executable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(change, "executable"));
    file_bytes asked = bytes_asked_for(change);
    if (asked.bytes == NULL) {
        return fail(note, "the file to copy could not be read", text_of(change, "source"));
    }
    runtime_entry_outcome created = runtime_create_new_file(path, asked.bytes, asked.length, executable);
    free(asked.bytes);
    if (created == RUNTIME_ENTRY_EXISTS) {
        return refuse(note, "something already exists where the file would be created; it was left unchanged", path);
    }
    if (created != RUNTIME_ENTRY_DONE) {
        return fail(note, "the file could not be created (does its directory exist?)", path);
    }
    return RUNTIME_CHANGE_DONE;
}

static runtime_change_reversal reverse_file(const cJSON *record, runtime_change_note *note) {
    const char *path = text_of(record, "path");
    runtime_entry_outcome removed = runtime_remove_file_entry(path);
    if (removed == RUNTIME_ENTRY_KEPT) {
        write_note(note, "a directory now stands where the host created a file; it was left: %s", path);
        return RUNTIME_CHANGE_DRIFTED;
    }
    if (removed != RUNTIME_ENTRY_DONE) {
        write_note(note, "the file the host created could not be deleted: %s", path);
        return RUNTIME_CHANGE_NOT_REVERSED;
    }
    return RUNTIME_CHANGE_REVERSED;
}

static runtime_change_step prepare_directory(const cJSON *change, const cJSON *holder, cJSON **record,
                                             runtime_change_note *note) {
    const char *path = text_of(change, "path");
    runtime_entry_kind found = runtime_probe_entry(path);
    if (found == RUNTIME_ENTRY_OTHER || found == RUNTIME_ENTRY_UNKNOWN) {
        return refuse(note, "something that is not a directory already exists there; it was left unchanged", path);
    }
    if (found == RUNTIME_ENTRY_DIRECTORY && holder == NULL) {
        return RUNTIME_CHANGE_UNNEEDED;
    }
    static const char *const members[] = {"path"};
    *record = record_with(change, members, 1);
    return *record != NULL ? RUNTIME_CHANGE_DONE : out_of_memory(note);
}

static runtime_change_step perform_directory(const cJSON *change, runtime_change_note *note) {
    const char *path = text_of(change, "path");
    return runtime_create_directory(path) == RUNTIME_ENTRY_DONE
               ? RUNTIME_CHANGE_DONE
               : fail(note, "the directory could not be created (does its parent exist?)", path);
}

static runtime_change_reversal reverse_directory(const cJSON *record, int held, runtime_change_note *note) {
    const char *path = text_of(record, "path");
    runtime_entry_outcome removed = held ? RUNTIME_ENTRY_DONE : runtime_remove_empty_directory(path);
    if (removed == RUNTIME_ENTRY_KEPT) {
        write_note(note, "the directory the host created holds something it did not create, or is no longer a "
                         "directory; it was left: %s", path);
        return RUNTIME_CHANGE_DRIFTED;
    }
    if (removed != RUNTIME_ENTRY_DONE) {
        write_note(note, "the directory the host created could not be removed: %s", path);
        return RUNTIME_CHANGE_NOT_REVERSED;
    }
    return RUNTIME_CHANGE_REVERSED;
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

static const char *trailing_ending(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    const char *ending = "";
    if (bytes != NULL && length >= 2 && bytes[length - 2] == '\r' && bytes[length - 1] == '\n') {
        ending = "\r\n";
    } else if (bytes != NULL && length >= 1 && bytes[length - 1] == '\n') {
        ending = "\n";
    }
    free(bytes);
    return ending;
}

/**
 * @implNote The file is written as JSON.stringify(value, null, 2) followed by the ending it had, the
 * form a Node application such as Claude Code writes, so a file in that form comes back byte-equal
 * after a reversal. Whole-line comments, which the reader tolerates, are not written back.
 */
static int write_json(const char *path, const cJSON *root, const char *ending) {
    char *text = spisor_json_stringify_pretty(root);
    size_t text_length = text == NULL ? 0 : strlen(text);
    char *bytes = text == NULL ? NULL : realloc(text, text_length + strlen(ending) + 1);
    if (bytes == NULL) {
        free(text);
        return 0;
    }
    memcpy(bytes + text_length, ending, strlen(ending) + 1);
    int written = spisor_replace_file(path, (const uint8_t *)bytes, strlen(bytes));
    free(bytes);
    return written;
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

static runtime_change_step record_json_key(const cJSON *change, cJSON *root, int missing_from, cJSON **record,
                                           runtime_change_note *note) {
    static const char *const members[] = {"file", "key", "value"};
    *record = record_with(change, members, 3);
    int complete = *record != NULL;
    if (complete && missing_from >= 0) {
        complete = cJSON_AddNumberToObject(*record, "createdFrom", missing_from) != NULL;
    } else if (complete) {
        cJSON *previous = cJSON_Duplicate(value_at(root, cJSON_GetObjectItemCaseSensitive(change, "key")), 1);
        complete = previous != NULL && cJSON_AddItemToObject(*record, "previous", previous);
        if (previous != NULL && !complete) {
            cJSON_Delete(previous);
        }
    }
    if (!complete) {
        cJSON_Delete(*record);
        *record = NULL;
        return out_of_memory(note);
    }
    return RUNTIME_CHANGE_DONE;
}

static int holds_value_already(cJSON *root, const cJSON *change) {
    const cJSON *current = value_at(root, cJSON_GetObjectItemCaseSensitive(change, "key"));
    return current != NULL && cJSON_Compare(current, cJSON_GetObjectItemCaseSensitive(change, "value"), 1);
}

static runtime_change_step prepare_json_key(const cJSON *change, cJSON **record, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    cJSON *root = read_json_object(path, note, &step);
    if (root == NULL) {
        return step;
    }
    int missing_from = first_missing_depth(root, cJSON_GetObjectItemCaseSensitive(change, "key"), note, path);
    if (missing_from == -2) {
        step = RUNTIME_CHANGE_REFUSED;
    } else if (missing_from == -1 && holds_value_already(root, change)) {
        step = RUNTIME_CHANGE_UNNEEDED;
    } else {
        step = record_json_key(change, root, missing_from, record, note);
    }
    cJSON_Delete(root);
    return step;
}

static cJSON *object_creating(cJSON *root, const cJSON *key) {
    int last = cJSON_GetArraySize(key) - 1;
    cJSON *node = root;
    for (int index = 0; node != NULL && index < last; index++) {
        const char *name = cJSON_GetArrayItem(key, index)->valuestring;
        cJSON *child = cJSON_GetObjectItemCaseSensitive(node, name);
        node = child == NULL ? cJSON_AddObjectToObject(node, name) : (cJSON_IsObject(child) ? child : NULL);
    }
    return node;
}

static int set_member(cJSON *parent, const char *name, const cJSON *value) {
    cJSON *copy = cJSON_Duplicate(value, 1);
    if (copy == NULL) {
        return 0;
    }
    int set = cJSON_GetObjectItemCaseSensitive(parent, name) != NULL
                  ? cJSON_ReplaceItemInObjectCaseSensitive(parent, name, copy)
                  : cJSON_AddItemToObject(parent, name, copy);
    if (!set) {
        cJSON_Delete(copy);
    }
    return set;
}

static runtime_change_step perform_json_key(const cJSON *change, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(change, "key");
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    const char *ending = trailing_ending(path);
    cJSON *root = read_json_object(path, note, &step);
    if (root == NULL) {
        return step;
    }
    cJSON *parent = object_creating(root, key);
    const char *leaf = cJSON_GetArrayItem(key, cJSON_GetArraySize(key) - 1)->valuestring;
    int written = parent != NULL && set_member(parent, leaf, cJSON_GetObjectItemCaseSensitive(change, "value")) &&
                  write_json(path, root, ending);
    cJSON_Delete(root);
    return written ? RUNTIME_CHANGE_DONE : fail(note, "the key could not be written", path);
}

static int json_key_is_on_disk(const cJSON *record) {
    spisor_json_file_state state;
    cJSON *root = spisor_read_json_file(text_of(record, "file"), &state);
    const cJSON *current = root == NULL ? NULL : value_at(root, cJSON_GetObjectItemCaseSensitive(record, "key"));
    int on_disk = current != NULL && cJSON_Compare(current, cJSON_GetObjectItemCaseSensitive(record, "value"), 1);
    cJSON_Delete(root);
    return on_disk;
}

static void remove_created_objects(cJSON *root, const cJSON *key, int created_from) {
    for (int depth = cJSON_GetArraySize(key) - 1; depth > created_from; depth--) {
        cJSON *object = object_at(root, key, depth);
        if (object == NULL || object->child != NULL) {
            return;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(object_at(root, key, depth - 1),
                                                cJSON_GetArrayItem(key, depth - 1)->valuestring);
    }
}

static int restore_previous(cJSON *root, const cJSON *record) {
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(record, "key");
    int last = cJSON_GetArraySize(key) - 1;
    cJSON *parent = object_at(root, key, last);
    const char *leaf = cJSON_GetArrayItem(key, last)->valuestring;
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(record, "previous");
    if (previous != NULL) {
        return set_member(parent, leaf, previous);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(parent, leaf);
    const cJSON *created_from = cJSON_GetObjectItemCaseSensitive(record, "createdFrom");
    remove_created_objects(root, key, cJSON_IsNumber(created_from) ? created_from->valueint : last);
    return 1;
}

static int is_already_restored(const cJSON *current, const cJSON *record) {
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(record, "previous");
    return previous == NULL ? current == NULL : current != NULL && cJSON_Compare(current, previous, 1);
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
    key_text(cJSON_GetObjectItemCaseSensitive(record, "key"), key, sizeof key);
    write_note(note, "the key %s in %s %s; it was left", key, text_of(record, "file"), why);
    return RUNTIME_CHANGE_DRIFTED;
}

static runtime_change_reversal restore_key(cJSON *root, const cJSON *record, const char *ending,
                                           runtime_change_note *note) {
    if (restore_previous(root, record) && write_json(text_of(record, "file"), root, ending)) {
        return RUNTIME_CHANGE_REVERSED;
    }
    write_note(note, "the key's previous value could not be written back to %s", text_of(record, "file"));
    return RUNTIME_CHANGE_NOT_REVERSED;
}

static runtime_change_reversal reverse_json_key(const cJSON *record, runtime_change_note *note) {
    const char *path = text_of(record, "file");
    const char *ending = trailing_ending(path);
    spisor_json_file_state state;
    cJSON *root = spisor_read_json_file(path, &state);
    if (state != SPISOR_JSON_FILE_PARSED || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        const char *why = state == SPISOR_JSON_FILE_ABSENT ? "is gone with its file"
                                                           : "is in a file that is no longer readable JSON";
        return drifted_key(record, why, note);
    }
    const cJSON *current = value_at(root, cJSON_GetObjectItemCaseSensitive(record, "key"));
    runtime_change_reversal reversal;
    if (current != NULL && cJSON_Compare(current, cJSON_GetObjectItemCaseSensitive(record, "value"), 1)) {
        reversal = restore_key(root, record, ending, note);
    } else if (is_already_restored(current, record)) {
        reversal = RUNTIME_CHANGE_REVERSED;
    } else {
        reversal = drifted_key(record, "no longer holds the value the host set", note);
    }
    cJSON_Delete(root);
    return reversal;
}

static int read_text(const char *path, text_buffer *text) {
    text->bytes = (char *)spisor_read_file(path, &text->length);
    return text->bytes != NULL;
}

static line_span find_last_line(const text_buffer *text, const char *line) {
    line_span span = {0, 0, 0};
    size_t line_length = strlen(line);
    size_t start = 0;
    while (start < text->length) {
        const char *newline = memchr(text->bytes + start, '\n', text->length - start);
        size_t line_end = newline == NULL ? text->length : (size_t)(newline - text->bytes);
        size_t content_end = line_end > start && text->bytes[line_end - 1] == '\r' ? line_end - 1 : line_end;
        if (content_end - start == line_length && memcmp(text->bytes + start, line, line_length) == 0) {
            span.start = start;
            span.end = newline == NULL ? text->length : line_end + 1;
            span.found = 1;
        }
        start = line_end + 1;
    }
    return span;
}

static int has_line(const char *path, const char *line) {
    text_buffer text;
    int found = read_text(path, &text) && find_last_line(&text, line).found;
    free(text.bytes);
    return found;
}

static int is_unterminated(const text_buffer *text) {
    return text->length > 0 && text->bytes[text->length - 1] != '\n';
}

static int add_separated(cJSON *record, int separated) {
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(record, "separated");
    if (existing != NULL) {
        cJSON_SetBoolValue(existing, separated);
        return 1;
    }
    return cJSON_AddBoolToObject(record, "separated", separated) != NULL;
}

static int is_holder_flag(const cJSON *holder, const char *member) {
    return holder != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(holder, member));
}

static int may_create(const cJSON *change, const char *path) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(change, "create")) &&
           runtime_probe_entry(path) == RUNTIME_ENTRY_MISSING;
}

static runtime_change_step record_profile_line(const cJSON *change, int separated, int created, cJSON **record,
                                               runtime_change_note *note) {
    static const char *const members[] = {"file", "line"};
    *record = record_with(change, members, 2);
    if (*record != NULL && (!add_separated(*record, separated) ||
                            (created && cJSON_AddTrueToObject(*record, "created") == NULL))) {
        cJSON_Delete(*record);
        *record = NULL;
    }
    return *record != NULL ? RUNTIME_CHANGE_DONE : out_of_memory(note);
}

/**
 * @implNote A record of a line another host already holds inherits that host's separated and created,
 * so whichever record is reversed last undoes exactly what the first one did, the file it created
 * included.
 */
static runtime_change_step prepare_profile_line(const cJSON *change, const cJSON *holder, cJSON **record,
                                                runtime_change_note *note) {
    const char *path = text_of(change, "file");
    text_buffer text;
    if (!read_text(path, &text)) {
        return may_create(change, path) ? record_profile_line(change, 0, 1, record, note)
                                        : refuse(note, "the profile file does not exist or cannot be read", path);
    }
    int present = find_last_line(&text, text_of(change, "line")).found;
    int separated = present ? is_holder_flag(holder, "separated") : is_unterminated(&text);
    free(text.bytes);
    if (present && holder == NULL) {
        return RUNTIME_CHANGE_UNNEEDED;
    }
    return record_profile_line(change, separated, present && is_holder_flag(holder, "created"), record, note);
}

static const char *line_ending_of(const text_buffer *text) {
    return strstr(text->bytes, "\r\n") != NULL ? "\r\n" : "\n";
}

static int append_line(const char *path, const text_buffer *text, const char *line, int separated) {
    const char *ending = line_ending_of(text);
    size_t size = text->length + 2 * strlen(ending) + strlen(line) + 1;
    char *bytes = malloc(size);
    if (bytes == NULL) {
        return 0;
    }
    memcpy(bytes, text->bytes, text->length);
    int appended = snprintf(bytes + text->length, size - text->length, "%s%s%s", separated ? ending : "", line, ending);
    int written = appended > 0 && spisor_replace_file(path, (const uint8_t *)bytes, text->length + (size_t)appended);
    free(bytes);
    return written;
}

static runtime_change_step create_profile(const char *path, const char *line, runtime_change_note *note) {
    size_t size = strlen(line) + 2;
    char *bytes = malloc(size);
    if (bytes == NULL) {
        return out_of_memory(note);
    }
    snprintf(bytes, size, "%s\n", line);
    runtime_entry_outcome created = runtime_create_new_file(path, (const uint8_t *)bytes, size - 1, 0);
    free(bytes);
    return created == RUNTIME_ENTRY_DONE ? RUNTIME_CHANGE_DONE : fail(note, "the profile could not be created", path);
}

static runtime_change_step perform_profile_line(const cJSON *change, cJSON *record, runtime_change_note *note) {
    const char *path = text_of(change, "file");
    const char *line = text_of(change, "line");
    text_buffer text;
    if (!read_text(path, &text) && is_holder_flag(record, "created")) {
        return create_profile(path, line, note);
    }
    if (text.bytes == NULL) {
        return fail(note, "the profile file could not be read", path);
    }
    int written = 1;
    if (!find_last_line(&text, line).found) {
        int separated = is_unterminated(&text);
        written = add_separated(record, separated) && append_line(path, &text, line, separated);
    }
    free(text.bytes);
    return written ? RUNTIME_CHANGE_DONE : fail(note, "the line could not be added", path);
}

static size_t trailing_newline_length(const char *bytes, size_t length) {
    if (length >= 2 && bytes[length - 2] == '\r' && bytes[length - 1] == '\n') {
        return 2;
    }
    return length >= 1 && bytes[length - 1] == '\n' ? 1 : 0;
}

static int remove_line(const char *path, const text_buffer *text, line_span span, int separated) {
    size_t kept = text->length - (span.end - span.start);
    char *bytes = malloc(kept + 1);
    if (bytes == NULL) {
        return 0;
    }
    memcpy(bytes, text->bytes, span.start);
    memcpy(bytes + span.start, text->bytes + span.end, text->length - span.end);
    if (separated && span.end == text->length) {
        kept -= trailing_newline_length(bytes, kept);
    }
    int written = spisor_replace_file(path, (const uint8_t *)bytes, kept);
    free(bytes);
    return written;
}

static int is_empty_file(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    free(bytes);
    return bytes != NULL && length == 0;
}

/** Deletes a profile the host created once the line is all it held; anything the user added keeps it. */
static int remove_emptied_profile(const cJSON *record, const char *path) {
    if (!is_holder_flag(record, "created") || !is_empty_file(path)) {
        return 1;
    }
    return runtime_remove_file_entry(path) == RUNTIME_ENTRY_DONE;
}

static runtime_change_reversal reverse_profile_line(const cJSON *record, int held, runtime_change_note *note) {
    const char *path = text_of(record, "file");
    text_buffer text;
    if (held || !read_text(path, &text)) {
        return RUNTIME_CHANGE_REVERSED;
    }
    line_span span = find_last_line(&text, text_of(record, "line"));
    int removed = !span.found ||
                  remove_line(path, &text, span, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "separated")));
    free(text.bytes);
    if (!removed) {
        write_note(note, "the line could not be removed from %s", path);
        return RUNTIME_CHANGE_NOT_REVERSED;
    }
    if (!remove_emptied_profile(record, path)) {
        write_note(note, "the profile the host created could not be deleted: %s", path);
        return RUNTIME_CHANGE_NOT_REVERSED;
    }
    return RUNTIME_CHANGE_REVERSED;
}

runtime_change_step runtime_change_children(const cJSON *change, cJSON **children, runtime_change_note *note) {
    *children = NULL;
    return is_kind(change, "path-entry") ? runtime_path_entry_children(change, children, note) : RUNTIME_CHANGE_DONE;
}

runtime_change_step runtime_change_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                           runtime_change_note *note) {
    *record = NULL;
    if (is_kind(change, "file")) {
        return prepare_file(change, record, note);
    }
    if (is_kind(change, "directory")) {
        return prepare_directory(change, holder, record, note);
    }
    if (is_kind(change, "json-key")) {
        return prepare_json_key(change, record, note);
    }
    if (is_kind(change, "path-entry")) {
        return runtime_path_entry_prepare(change, holder, record, note);
    }
    return prepare_profile_line(change, holder, record, note);
}

runtime_change_step runtime_change_perform(const cJSON *change, cJSON *record, runtime_change_note *note) {
    if (is_kind(change, "file")) {
        return perform_file(change, note);
    }
    if (is_kind(change, "directory")) {
        return perform_directory(change, note);
    }
    if (is_kind(change, "json-key")) {
        return perform_json_key(change, note);
    }
    if (is_kind(change, "path-entry")) {
        return runtime_path_entry_perform(change, record, note);
    }
    return perform_profile_line(change, record, note);
}

int runtime_change_is_on_disk(const cJSON *record) {
    if (is_kind(record, "file")) {
        return runtime_probe_entry(text_of(record, "path")) == RUNTIME_ENTRY_OTHER;
    }
    if (is_kind(record, "directory")) {
        return runtime_probe_entry(text_of(record, "path")) == RUNTIME_ENTRY_DIRECTORY;
    }
    if (is_kind(record, "json-key")) {
        return json_key_is_on_disk(record);
    }
    if (is_kind(record, "path-entry")) {
        return runtime_path_entry_is_on_disk(record);
    }
    return is_kind(record, "profile-line") && has_line(text_of(record, "file"), text_of(record, "line"));
}

runtime_change_reversal runtime_change_reverse(const cJSON *record, int held, runtime_change_note *note) {
    note->detail[0] = '\0';
    if (is_kind(record, "file")) {
        return reverse_file(record, note);
    }
    if (is_kind(record, "directory")) {
        return reverse_directory(record, held, note);
    }
    if (is_kind(record, "json-key")) {
        return reverse_json_key(record, note);
    }
    if (is_kind(record, "profile-line")) {
        return reverse_profile_line(record, held, note);
    }
    if (is_kind(record, "path-entry")) {
        return runtime_path_entry_reverse(record, held, note);
    }
    write_note(note, "the record's kind is not one this runtime can reverse");
    return RUNTIME_CHANGE_NOT_REVERSED;
}
