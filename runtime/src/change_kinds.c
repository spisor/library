#include "change_kinds.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "json_key_change.h"
#include "path_entry.h"
#include "paths.h"
#include "spisor/file.h"
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
        return runtime_json_key_defect(change);
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

cJSON *runtime_change_record_with(const cJSON *change, const char *const *members, size_t count) {
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

void runtime_change_digest(const uint8_t *bytes, size_t length, char digest[RUNTIME_CHANGE_DIGEST_SIZE]) {
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
    runtime_change_digest(asked->bytes, asked->length, digest);
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
    *record = runtime_change_record_with(change, members, 1);
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
    runtime_change_digest(asked.bytes, asked.length, digest);
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
    *record = runtime_change_record_with(change, members, 1);
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
    *record = runtime_change_record_with(change, members, 2);
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

/**
 * @implNote CRLF only when every line ending in the file is CRLF: one stray CRLF in a LF profile would
 * otherwise give the appended PATH line a carriage return, which POSIX shells keep in the last entry.
 */
static int is_every_line_crlf(const text_buffer *text) {
    size_t endings = 0;
    for (size_t index = 0; index < text->length; index++) {
        if (text->bytes[index] != '\n') {
            continue;
        }
        if (index == 0 || text->bytes[index - 1] != '\r') {
            return 0;
        }
        endings++;
    }
    return endings > 0;
}

static const char *line_ending_of(const text_buffer *text) {
    return is_every_line_crlf(text) ? "\r\n" : "\n";
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
        return runtime_json_key_prepare(change, record, note);
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
        return runtime_json_key_perform(change, record, note);
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
        return runtime_json_key_is_on_disk(record);
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
        return runtime_json_key_reverse(record, note);
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
