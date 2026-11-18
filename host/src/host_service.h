#ifndef HOST_SERVICE_H
#define HOST_SERVICE_H

#include <stddef.h>

#include "cJSON.h"

/** The string member name of object, or NULL. */
const char *host_text(const cJSON *object, const char *name);

/** The {message, fix} refusal of the message made of parts and fix; NULL when memory runs out. */
cJSON *host_refusal_of(const char *const *parts, size_t count, const char *fix);

/**
 * Gives the owner of the file at path leave to run it; 1 when it may run. Nothing to do on Windows.
 *
 * @implNote A catalog lists no file modes, so the cache stores a native executable as a plain file, and
 * whatever starts one from the cache makes it runnable first.
 */
int host_make_runnable(const char *path);

#endif
