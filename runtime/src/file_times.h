#ifndef RUNTIME_FILE_TIMES_H
#define RUNTIME_FILE_TIMES_H

/** The milliseconds since the epoch now, as Date.now() reads it. */
double runtime_now_milliseconds(void);

/**
 * The latest modification time of path or anything under it, in milliseconds since the epoch, never
 * following a link, into *milliseconds. 0 when anything under path cannot be read.
 */
int runtime_newest_change(const char *path, double *milliseconds);

#endif
