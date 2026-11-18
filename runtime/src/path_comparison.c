#include "change_kinds.h"

#include <string.h>

#include "paths.h"

typedef struct text_span {
    const char *start;
    size_t length;
} text_span;

typedef enum root_kind { ROOT_NONE, ROOT_SEPARATOR, ROOT_DRIVE, ROOT_SHARE } root_kind;

typedef struct path_root {
    root_kind kind;
    int rooted;
    char drive;
    text_span server;
    text_span share;
    size_t end;
} path_root;

typedef struct segment_cursor {
    const char *path;
    size_t start;
    size_t end;
    size_t pending_parents;
    int rooted;
} segment_cursor;

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

static char folded(char character) {
    return RUNTIME_HOST_IS_WINDOWS && character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
}

static int is_letter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

static size_t skip_separators(const char *path, size_t index) {
    while (path[index] != '\0' && is_separator(path[index])) {
        index++;
    }
    return index;
}

static text_span span_at(const char *path, size_t *index) {
    size_t start = skip_separators(path, *index);
    size_t end = start;
    while (path[end] != '\0' && !is_separator(path[end])) {
        end++;
    }
    *index = end;
    text_span span = {path + start, end - start};
    return span;
}

static path_root windows_root_of(const char *path) {
    path_root root = {ROOT_NONE, 0, '\0', {NULL, 0}, {NULL, 0}, 0};
    if (is_letter(path[0]) && path[1] == ':') {
        root.kind = ROOT_DRIVE;
        root.drive = folded(path[0]);
        root.rooted = is_separator(path[2]);
        root.end = 2;
    } else if (is_separator(path[0]) && is_separator(path[1])) {
        root.kind = ROOT_SHARE;
        root.rooted = 1;
        root.end = 2;
        root.server = span_at(path, &root.end);
        root.share = span_at(path, &root.end);
    } else if (is_separator(path[0])) {
        root.kind = ROOT_SEPARATOR;
        root.rooted = 1;
    }
    return root;
}

static path_root root_of(const char *path) {
    if (RUNTIME_HOST_IS_WINDOWS) {
        return windows_root_of(path);
    }
    path_root root = {ROOT_NONE, 0, '\0', {NULL, 0}, {NULL, 0}, 0};
    if (is_separator(path[0])) {
        root.kind = ROOT_SEPARATOR;
        root.rooted = 1;
    }
    return root;
}

static int same_span(text_span left, text_span right) {
    if (left.length != right.length) {
        return 0;
    }
    for (size_t index = 0; index < left.length; index++) {
        if (folded(left.start[index]) != folded(right.start[index])) {
            return 0;
        }
    }
    return 1;
}

static int same_root(const path_root *left, const path_root *right) {
    return left->kind == right->kind && left->rooted == right->rooted && left->drive == right->drive &&
           same_span(left->server, right->server) && same_span(left->share, right->share);
}

static int is_dot_segment(text_span segment, size_t dots) {
    return segment.length == dots && (dots < 1 || segment.start[0] == '.') && (dots < 2 || segment.start[1] == '.');
}

static text_span segment_before(segment_cursor *cursor) {
    while (cursor->end > cursor->start && is_separator(cursor->path[cursor->end - 1])) {
        cursor->end--;
    }
    size_t begin = cursor->end;
    while (begin > cursor->start && !is_separator(cursor->path[begin - 1])) {
        begin--;
    }
    text_span segment = {cursor->path + begin, cursor->end - begin};
    cursor->end = begin;
    return segment;
}

/** The next segment of the path's normal form, read from its end; 0 once none is left. */
static int next_segment(segment_cursor *cursor, text_span *segment) {
    while (cursor->end > cursor->start) {
        text_span candidate = segment_before(cursor);
        if (candidate.length == 0 || is_dot_segment(candidate, 1)) {
            continue;
        }
        if (is_dot_segment(candidate, 2)) {
            cursor->pending_parents++;
            continue;
        }
        if (cursor->pending_parents > 0) {
            cursor->pending_parents--;
            continue;
        }
        *segment = candidate;
        return 1;
    }
    if (cursor->rooted || cursor->pending_parents == 0) {
        return 0;
    }
    cursor->pending_parents--;
    segment->start = "..";
    segment->length = 2;
    return 1;
}

static segment_cursor cursor_after(const char *path, const path_root *root) {
    segment_cursor cursor = {path, root->end, strlen(path), 0, root->rooted};
    return cursor;
}

static int same_segments(segment_cursor *left, segment_cursor *right) {
    text_span left_segment = {NULL, 0};
    text_span right_segment = {NULL, 0};
    for (;;) {
        int left_more = next_segment(left, &left_segment);
        int right_more = next_segment(right, &right_segment);
        if (!left_more || !right_more) {
            return left_more == right_more;
        }
        if (!same_span(left_segment, right_segment)) {
            return 0;
        }
    }
}

/**
 * @implNote Compares the two normal forms segment by segment straight from the given text, so it allocates nothing:
 * a comparison that could run out of memory would have to answer a third value at every caller, and a failed one read
 * as "different" is how a held change was once applied twice.
 */
int runtime_change_paths_equal(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return 0;
    }
    path_root left_root = root_of(left);
    path_root right_root = root_of(right);
    if (!same_root(&left_root, &right_root)) {
        return 0;
    }
    segment_cursor left_cursor = cursor_after(left, &left_root);
    segment_cursor right_cursor = cursor_after(right, &right_root);
    return same_segments(&left_cursor, &right_cursor);
}
