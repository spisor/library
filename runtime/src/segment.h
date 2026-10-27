#ifndef RUNTIME_SEGMENT_H
#define RUNTIME_SEGMENT_H

/**
 * The name trimmed as String.prototype.trim does, when the result is a portable path segment, else NULL.
 * Freed with free.
 *
 * @implNote Portable means usable on every OS the host runs on: not empty, ".", ".." or ending in "."
 * or a space, free of "/", "\" and ":", and with a stem (the part before the first ".") that is not a
 * Windows reserved device name, compared ignoring case.
 */
char *runtime_portable_segment(const char *name);

#endif
