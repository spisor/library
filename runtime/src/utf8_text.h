#ifndef RUNTIME_UTF8_TEXT_H
#define RUNTIME_UTF8_TEXT_H

#include <stddef.h>

/**
 * The bytes as UTF-8 text, freed: each byte that starts no well-formed sequence, NUL among them, replaced by U+FFFD,
 * and with `cut`, when the bytes are the end of a longer run, up to three leading continuation bytes dropped first, so
 * the text starts where a character does; NULL when memory runs out.
 */
char *runtime_utf8_text(const char *bytes, size_t length, int cut);

#endif
