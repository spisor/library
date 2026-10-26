#include "spisor/path.h"

#include <stdlib.h>
#include <string.h>

#define NOT_FOUND ((size_t)-1)

typedef struct text {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} text;

typedef struct path_view {
    const char *data;
    size_t length;
} path_view;

typedef struct win32_root {
    text device;
    int has_device;
    size_t root_end;
    int is_absolute;
} win32_root;

static void text_reserve(text *buffer, size_t extra) {
    if (buffer->failed || buffer->length + extra + 1 <= buffer->capacity) {
        return;
    }
    size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
    while (capacity < buffer->length + extra + 1) {
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        buffer->failed = 1;
        return;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
}

static void text_append(text *buffer, const char *bytes, size_t length) {
    text_reserve(buffer, length);
    if (buffer->failed) {
        return;
    }
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void text_append_string(text *buffer, const char *string) {
    text_append(buffer, string, strlen(string));
}

static void text_truncate(text *buffer, size_t length) {
    buffer->length = length;
    if (buffer->data != NULL) {
        buffer->data[length] = '\0';
    }
}

static char *text_finish(text *buffer) {
    text_reserve(buffer, 0);
    if (buffer->failed) {
        free(buffer->data);
        return NULL;
    }
    buffer->data[buffer->length] = '\0';
    return buffer->data;
}

static char *copy_of(const char *bytes, size_t length) {
    text buffer = {NULL, 0, 0, 0};
    text_append(&buffer, bytes, length);
    return text_finish(&buffer);
}

static int is_posix_separator(char character) {
    return character == '/';
}

static int is_win32_separator(char character) {
    return character == '/' || character == '\\';
}

static int is_separator(int windows, char character) {
    return windows ? is_win32_separator(character) : is_posix_separator(character);
}

static int is_drive_letter(char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

static size_t last_index_of(const text *buffer, char character) {
    for (size_t index = buffer->length; index > 0; index--) {
        if (buffer->data[index - 1] == character) {
            return index - 1;
        }
    }
    return NOT_FOUND;
}

static size_t index_of(path_view path, char character, size_t from) {
    for (size_t index = from; index < path.length; index++) {
        if (path.data[index] == character) {
            return index;
        }
    }
    return NOT_FOUND;
}

static int ends_with_two_dots(const text *buffer) {
    return buffer->length >= 2 && buffer->data[buffer->length - 1] == '.' && buffer->data[buffer->length - 2] == '.';
}

/**
 * Node's normalizeString, step for step.
 *
 * @implNote Node counts UTF-16 units and this counts bytes; every decision it takes compares
 * against ASCII separators and dots, or against lengths of 2 that only ".." can reach, so the two
 * agree on every input.
 */
static text normalize_string(int windows, path_view path, int allow_above_root) {
    text result = {NULL, 0, 0, 0};
    char separator = windows ? '\\' : '/';
    size_t last_segment_length = 0;
    size_t last_slash = NOT_FOUND;
    int dots = 0;
    char code = 0;
    for (size_t index = 0; index <= path.length; index++) {
        if (index < path.length) {
            code = path.data[index];
        } else if (is_separator(windows, code)) {
            break;
        } else {
            code = '/';
        }
        if (!is_separator(windows, code)) {
            dots = (code == '.' && dots != -1) ? dots + 1 : -1;
            continue;
        }
        if (last_slash + 1 == index || dots == 1) {
        } else if (dots == 2) {
            if (result.length < 2 || last_segment_length != 2 || !ends_with_two_dots(&result)) {
                if (result.length > 2) {
                    size_t cut = last_index_of(&result, separator);
                    if (cut == NOT_FOUND) {
                        text_truncate(&result, 0);
                        last_segment_length = 0;
                    } else {
                        text_truncate(&result, cut);
                        size_t previous = last_index_of(&result, separator);
                        last_segment_length = previous == NOT_FOUND ? result.length : result.length - 1 - previous;
                    }
                    last_slash = index;
                    dots = 0;
                    continue;
                } else if (result.length != 0) {
                    text_truncate(&result, 0);
                    last_segment_length = 0;
                    last_slash = index;
                    dots = 0;
                    continue;
                }
            }
            if (allow_above_root) {
                if (result.length > 0) {
                    text_append(&result, &separator, 1);
                }
                text_append_string(&result, "..");
                last_segment_length = 2;
            }
        } else {
            size_t start = last_slash + 1;
            if (result.length > 0) {
                text_append(&result, &separator, 1);
            }
            text_append(&result, path.data + start, index - start);
            last_segment_length = index - start;
        }
        last_slash = index;
        dots = 0;
    }
    return result;
}

static char *posix_normalize(path_view path) {
    if (path.length == 0) {
        return copy_of(".", 1);
    }
    int is_absolute = path.data[0] == '/';
    int trailing_separator = path.data[path.length - 1] == '/';
    text normalized = normalize_string(0, path, !is_absolute);
    if (normalized.length == 0 && !normalized.failed) {
        free(normalized.data);
        return is_absolute ? copy_of("/", 1) : copy_of(trailing_separator ? "./" : ".", trailing_separator ? 2 : 1);
    }
    text result = {NULL, 0, 0, normalized.failed};
    if (is_absolute) {
        text_append_string(&result, "/");
    }
    text_append(&result, normalized.data, normalized.length);
    if (trailing_separator) {
        text_append_string(&result, "/");
    }
    free(normalized.data);
    return text_finish(&result);
}

static size_t last_character_start(path_view path) {
    size_t start = path.length;
    while (start > 0) {
        start--;
        if (((unsigned char)path.data[start] & 0xC0) != 0x80) {
            break;
        }
    }
    return start;
}

static int is_reserved_device(const char *bytes, size_t length) {
    static const char *const reserved_names[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
        "COM\xc2\xb9", "COM\xc2\xb2", "COM\xc2\xb3", "LPT\xc2\xb9", "LPT\xc2\xb2", "LPT\xc2\xb3",
    };
    for (size_t name = 0; name < sizeof(reserved_names) / sizeof(reserved_names[0]); name++) {
        const char *candidate = reserved_names[name];
        if (strlen(candidate) != length) {
            continue;
        }
        size_t index = 0;
        while (index < length) {
            char character = bytes[index];
            char upper = (character >= 'a' && character <= 'z') ? (char)(character - 'a' + 'A') : character;
            if (upper != candidate[index]) {
                break;
            }
            index++;
        }
        if (index == length) {
            return 1;
        }
    }
    return 0;
}

/**
 * Node's isWindowsReservedName(path, colonIndex).
 *
 * @implNote Node slices to colonIndex, and a colonIndex of -1 slices off the last UTF-16 unit.
 * Dropping a whole astral character instead is safe because Node would leave half of it, a lone
 * surrogate, which no reserved name contains.
 */
static int is_reserved_name(path_view path, size_t colon_index) {
    if (colon_index != NOT_FOUND) {
        return is_reserved_device(path.data, colon_index);
    }
    if (path.length == 0) {
        return 0;
    }
    size_t start = last_character_start(path);
    if (path.length - start == 4) {
        return 0;
    }
    return is_reserved_device(path.data, start);
}

static size_t skip_while(path_view path, size_t index, int separators) {
    while (index < path.length && is_win32_separator(path.data[index]) == separators) {
        index++;
    }
    return index;
}

static void set_device(win32_root *root, const char *prefix, const char *bytes, size_t length) {
    root->has_device = 1;
    text_truncate(&root->device, 0);
    text_append_string(&root->device, prefix);
    text_append(&root->device, bytes, length);
}

static void match_device_namespace(path_view path, const char *marker, win32_root *root) {
    set_device(root, "\\\\", marker, 1);
    root->root_end = 4;
    size_t colon_index = index_of(path, ':', 0);
    if (colon_index != NOT_FOUND && colon_index >= 4 && is_reserved_device(path.data + 4, colon_index - 4)) {
        set_device(root, "\\\\?\\", path.data + 4, colon_index + 1 - 4);
        root->root_end = colon_index + 1;
    }
}

static char *win32_unc_root_only(path_view path, path_view server, size_t share_start) {
    text result = {NULL, 0, 0, 0};
    text_append_string(&result, "\\\\");
    text_append(&result, server.data, server.length);
    text_append_string(&result, "\\");
    text_append(&result, path.data + share_start, path.length - share_start);
    text_append_string(&result, "\\");
    return text_finish(&result);
}

/** Matches a root starting with two separators; answers a finished path when the root is all there is. */
static char *match_double_separator_root(path_view path, win32_root *root) {
    size_t server_end = skip_while(path, 2, 0);
    if (server_end >= path.length || server_end == 2) {
        return NULL;
    }
    path_view server = {path.data + 2, server_end - 2};
    size_t share_start = skip_while(path, server_end, 1);
    if (share_start >= path.length || share_start == server_end) {
        return NULL;
    }
    size_t share_end = skip_while(path, share_start, 0);
    if (server.length == 1 && (server.data[0] == '.' || server.data[0] == '?')) {
        match_device_namespace(path, server.data, root);
        return NULL;
    }
    if (share_end == path.length) {
        return win32_unc_root_only(path, server, share_start);
    }
    set_device(root, "\\\\", server.data, server.length);
    text_append_string(&root->device, "\\");
    text_append(&root->device, path.data + share_start, share_end - share_start);
    root->root_end = share_end;
    return NULL;
}

static void match_drive_or_reserved_root(path_view path, win32_root *root) {
    size_t colon_index = index_of(path, ':', 0);
    if (colon_index == NOT_FOUND || colon_index == 0) {
        return;
    }
    if (is_drive_letter(path.data[0]) && colon_index == 1) {
        set_device(root, "", path.data, 2);
        root->root_end = 2;
        if (path.length > 2 && is_win32_separator(path.data[2])) {
            root->is_absolute = 1;
            root->root_end = 3;
        }
    } else if (is_reserved_name(path, colon_index)) {
        set_device(root, "", path.data, colon_index + 1);
        root->root_end = colon_index + 1;
    }
}

static int tail_could_read_as_absolute(path_view path, const text *tail) {
    if (tail->length >= 2 && is_drive_letter(tail->data[0]) && tail->data[1] == ':') {
        return 1;
    }
    for (size_t index = index_of(path, ':', 0); index != NOT_FOUND; index = index_of(path, ':', index + 1)) {
        if (index == path.length - 1 || is_win32_separator(path.data[index + 1])) {
            return 1;
        }
    }
    return 0;
}

static char *win32_assemble(path_view path, win32_root *root, text *tail) {
    text result = {NULL, 0, 0, tail->failed || root->device.failed};
    if (!root->is_absolute && !root->has_device && index_of(path, ':', 0) != NOT_FOUND &&
        tail_could_read_as_absolute(path, tail)) {
        text_append_string(&result, ".\\");
    } else if (is_reserved_name(path, index_of(path, ':', 0))) {
        text_append_string(&result, ".\\");
        text_append(&result, root->device.data, root->device.length);
    } else {
        text_append(&result, root->device.data, root->device.length);
        if (root->is_absolute) {
            text_append_string(&result, "\\");
        }
    }
    text_append(&result, tail->data, tail->length);
    free(tail->data);
    free(root->device.data);
    return text_finish(&result);
}

static char *win32_normalize(path_view path) {
    win32_root root = {{NULL, 0, 0, 0}, 0, 0, 0};
    if (path.length == 0) {
        return copy_of(".", 1);
    }
    if (path.length == 1) {
        return is_posix_separator(path.data[0]) ? copy_of("\\", 1) : copy_of(path.data, 1);
    }
    if (is_win32_separator(path.data[0])) {
        root.is_absolute = 1;
        if (is_win32_separator(path.data[1])) {
            char *finished = match_double_separator_root(path, &root);
            if (finished != NULL) {
                free(root.device.data);
                return finished;
            }
        } else {
            root.root_end = 1;
        }
    } else {
        match_drive_or_reserved_root(path, &root);
    }
    text tail = {NULL, 0, 0, 0};
    if (root.root_end < path.length) {
        path_view rest = {path.data + root.root_end, path.length - root.root_end};
        tail = normalize_string(1, rest, !root.is_absolute);
    }
    if (tail.length == 0 && !root.is_absolute) {
        text_append_string(&tail, ".");
    }
    if (tail.length > 0 && is_win32_separator(path.data[path.length - 1])) {
        text_append_string(&tail, "\\");
    }
    return win32_assemble(path, &root, &tail);
}

static int part_is_reserved(const char *bytes, size_t length) {
    path_view part = {bytes, length};
    size_t colon_index = index_of(part, ':', 0);
    return colon_index != NOT_FOUND && is_reserved_name(part, colon_index);
}

static int has_reserved_part(const text *joined) {
    size_t start = 0;
    for (size_t index = 0; index <= joined->length; index++) {
        if (index == joined->length || joined->data[index] == '\\') {
            if (index > start && part_is_reserved(joined->data + start, index - start)) {
                return 1;
            }
            start = index + 1;
        }
    }
    return 0;
}

static size_t leading_separators_to_collapse(const char *first_part, const text *joined) {
    size_t first_length = strlen(first_part);
    size_t count = 0;
    if (is_win32_separator(first_part[0])) {
        count++;
        if (first_length > 1 && is_win32_separator(first_part[1])) {
            count++;
            if (first_length > 2) {
                if (!is_win32_separator(first_part[2])) {
                    return 0;
                }
                count++;
            }
        }
    }
    while (count < joined->length && is_win32_separator(joined->data[count])) {
        count++;
    }
    return count >= 2 ? count : 0;
}

static char *win32_join_finish(text *joined, const char *first_part) {
    if (joined->failed) {
        free(joined->data);
        return NULL;
    }
    size_t collapse = leading_separators_to_collapse(first_part, joined);
    if (collapse > 0) {
        joined->data[collapse - 1] = '\\';
        memmove(joined->data, joined->data + collapse - 1, joined->length - collapse + 2);
        joined->length -= collapse - 1;
    }
    if (has_reserved_part(joined)) {
        for (size_t index = 0; index < joined->length; index++) {
            if (joined->data[index] == '/') {
                joined->data[index] = '\\';
            }
        }
        return text_finish(joined);
    }
    path_view view = {joined->data, joined->length};
    char *normalized = win32_normalize(view);
    free(joined->data);
    return normalized;
}

char *spisor_path_join(int windows, const char *const *segments, size_t count) {
    text joined = {NULL, 0, 0, 0};
    const char *first_part = NULL;
    for (size_t index = 0; index < count; index++) {
        if (segments[index][0] == '\0') {
            continue;
        }
        if (first_part == NULL) {
            first_part = segments[index];
        } else {
            text_append_string(&joined, windows ? "\\" : "/");
        }
        text_append_string(&joined, segments[index]);
    }
    if (first_part == NULL) {
        return copy_of(".", 1);
    }
    if (windows) {
        return win32_join_finish(&joined, first_part);
    }
    path_view view = {joined.data, joined.length};
    char *normalized = joined.failed ? NULL : posix_normalize(view);
    free(joined.data);
    return normalized;
}

int spisor_path_is_absolute(int windows, const char *path) {
    size_t length = strlen(path);
    if (!windows) {
        return length > 0 && path[0] == '/';
    }
    if (length == 0) {
        return 0;
    }
    return is_win32_separator(path[0]) ||
           (length > 2 && is_drive_letter(path[0]) && path[1] == ':' && is_win32_separator(path[2]));
}

static int starts_with_home_marker(const char *path) {
    return path[0] == '~' && (path[1] == '/' || path[1] == '\\');
}

char *spisor_expand_home(int windows, const char *path, const char *home) {
    if (strcmp(path, "~") == 0) {
        return copy_of(home, strlen(home));
    }
    if (starts_with_home_marker(path)) {
        const char *segments[2] = {home, path + 2};
        return spisor_path_join(windows, segments, 2);
    }
    return copy_of(path, strlen(path));
}
