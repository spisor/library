#ifndef SPISOR_JSON_GRAMMAR_H
#define SPISOR_JSON_GRAMMAR_H

#include <stddef.h>

/**
 * True when JSON.parse accepts the text's grammar: whitespace is SP, TAB, LF and CR only, a string
 * holds no raw character below 0x20 and only JSON's escapes, a number is
 * -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?, and nothing but whitespace follows the value.
 *
 * @implNote cJSON is laxer on each count: it skips a UTF-8 BOM, treats every byte up to 0x20 as
 * whitespace, takes raw control characters inside a string, and reads 01 and 1. through strtod.
 */
int spisor_is_json_parse_grammar(const char *text, size_t length);

#endif
