#ifndef SPISOR_UTF8_H
#define SPISOR_UTF8_H

#include <stddef.h>
#include <stdint.h>

/** True when the bytes are well-formed UTF-8: no overlong form, no surrogate, nothing past U+10FFFF. */
int spisor_is_valid_utf8(const uint8_t *bytes, size_t length);

#endif
