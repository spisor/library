#ifndef RUNTIME_RUNNABLE_H
#define RUNTIME_RUNNABLE_H

/**
 * Makes the file at path runnable by its owner; 1 when it is, which it always is on Windows, else 0.
 *
 * @implNote A catalog lists no file modes, so the cache stores a native executable as a plain file, and whatever
 * starts one from the cache makes it runnable first.
 */
int runtime_make_runnable(const char *path);

#endif
