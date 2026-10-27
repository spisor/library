#ifndef SPISOR_JSON_FILE_H
#define SPISOR_JSON_FILE_H

#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum spisor_json_file_state {
    SPISOR_JSON_FILE_ABSENT,
    SPISOR_JSON_FILE_UNREADABLE,
    SPISOR_JSON_FILE_PARSED
} spisor_json_file_state;

/**
 * Removes every line that holds only whitespace and a // comment (basekit's readJson regex, with
 * the multiline flag) from well-formed UTF-8, freed with free; NULL only when memory runs out.
 * The result is NUL terminated one past stripped_length.
 *
 * @implNote The regex's \s spans line feeds and a BOM, so a match starting on an earlier blank
 * line eats the blank lines up to the comment; a comment that follows content on its line stays.
 */
char *spisor_strip_comment_lines(const char *text, size_t length, size_t *stripped_length);

/**
 * basekit's readJson without the fallback: the file read as Node reads UTF-8, whole-line comments
 * stripped, then JSON.parse. The tree comes back with PARSED. NULL comes back with ABSENT only when
 * the path genuinely does not exist, or UNREADABLE when something exists but cannot be read (a
 * directory, say) or is not JSON, or when the OS would not say whether anything exists (a parent
 * without search permission, say).
 *
 * @implNote A BOM not followed by a comment is refused, as JSON.parse refuses it. UNREADABLE also
 * covers running out of memory while reading or parsing, since spisor_read_file and
 * spisor_parse_json answer NULL for every failure; that fails safe, as a caller that refuses to
 * write over an UNREADABLE file never overwrites one. The state is left untouched only when
 * memory runs out in the decode or strip step.
 */
cJSON *spisor_read_json_file(const char *path, spisor_json_file_state *state);

#ifdef __cplusplus
}
#endif

#endif
