#ifndef RUNTIME_RANDOM_VALUE_H
#define RUNTIME_RANDOM_VALUE_H

#include <stdint.h>

/** Fills *value with 64 bits from the OS's random source: 1, or 0 when the source gave none. */
typedef int (*runtime_random_draw)(uint64_t *value);

/**
 * runtime_random_draw from BCryptGenRandom's system-preferred generator on Windows, getrandom on Linux and
 * /dev/urandom elsewhere.
 */
int runtime_random_64(uint64_t *value);

#endif
