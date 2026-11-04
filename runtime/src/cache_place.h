#ifndef RUNTIME_CACHE_PLACE_H
#define RUNTIME_CACHE_PLACE_H

#include <stddef.h>

/**
 * Node's path.resolve(base, ...segments) for this platform: each absolute segment starts over, and the
 * result is absolute and normalized. Freed with free; NULL when memory runs out.
 */
char *runtime_cache_resolve(const char *base, const char *const *segments, size_t count);

/**
 * Whether child, a resolved path, lies below parent, a resolved path, and is not parent itself: what
 * path.relative(parent, child) answers neither empty, nor climbing, nor absolute.
 *
 * @implNote Windows compares without ASCII case, as path.win32.relative lowercases both sides.
 */
int runtime_cache_is_strictly_inside(const char *parent, const char *child);

#endif
