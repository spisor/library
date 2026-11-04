#ifndef RUNTIME_COMMAND_LINE_WINDOWS_H
#define RUNTIME_COMMAND_LINE_WINDOWS_H

#include <stddef.h>
#include <wchar.h>

/**
 * Appends a space and argument quoted at line[used] so CommandLineToArgvW reads it back unchanged, and
 * answers the new length: a run of backslashes is doubled before a quote, which is escaped, and before
 * the closing quote. line must have room for 2 * length + 3 more characters.
 */
size_t runtime_append_quoted_argument(wchar_t *line, size_t used, const wchar_t *argument);

#endif
