#ifndef RUNTIME_CACHE_REFUSAL_H
#define RUNTIME_CACHE_REFUSAL_H

#include <stddef.h>

#include "cJSON.h"

/** The parts written one after another, freed with free; NULL when memory runs out. */
char *runtime_cache_text(const char *const *parts, size_t count);

/** The {message, fix} answer every service refuses with, message being parts written one after another. */
cJSON *runtime_cache_refusal(const char *const *parts, size_t count, const char *fix);

#endif
