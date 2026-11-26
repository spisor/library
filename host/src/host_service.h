#ifndef HOST_SERVICE_H
#define HOST_SERVICE_H

#include <stddef.h>

#include "cJSON.h"

/** The string member name of object, or NULL. */
const char *host_text(const cJSON *object, const char *name);

/** The {message, fix} refusal of the message made of parts and fix; NULL when memory runs out. */
cJSON *host_refusal_of(const char *const *parts, size_t count, const char *fix);

#endif
