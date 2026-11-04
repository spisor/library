#include "command_line_windows.h"

static size_t append_backslashes(wchar_t *line, size_t used, size_t count) {
    for (size_t index = 0; index < count; index++) {
        line[used++] = L'\\';
    }
    return used;
}

size_t runtime_append_quoted_argument(wchar_t *line, size_t used, const wchar_t *argument) {
    line[used++] = L' ';
    line[used++] = L'"';
    size_t backslashes = 0;
    for (const wchar_t *cursor = argument; *cursor != L'\0'; cursor++) {
        if (*cursor == L'\\') {
            backslashes++;
            continue;
        }
        used = append_backslashes(line, used, *cursor == L'"' ? 2 * backslashes + 1 : backslashes);
        backslashes = 0;
        line[used++] = *cursor;
    }
    used = append_backslashes(line, used, 2 * backslashes);
    line[used++] = L'"';
    return used;
}
