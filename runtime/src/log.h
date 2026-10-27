#ifndef RUNTIME_LOG_H
#define RUNTIME_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * The ANSI colour code of a log name: basekit's palette indexed by its 31-multiplier hash.
 *
 * @implNote The hash runs over the name's UTF-16 code units, as charCodeAt does, so a character
 * outside the BMP counts as two surrogate units. name is valid UTF-8.
 */
uint32_t runtime_name_colour(const char *name);

RUNTIME_EXPORT int32_t runtime_log_write(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
