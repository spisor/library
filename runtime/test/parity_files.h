#ifndef RUNTIME_TEST_PARITY_FILES_H
#define RUNTIME_TEST_PARITY_FILES_H

#include <stddef.h>

#include "cJSON.h"

/**
 * @file
 * The parity observer's own file work: laying a case out and reading back what the runtime left. Test code
 * only.
 */

/** `left` and `right` joined with a slash, allocated with malloc; NULL when memory runs out. */
char *parity_join(const char *left, const char *right);

/** Deletes `path` and everything under it, never following a link; 1 when nothing is left there. */
int parity_remove_tree(const char *path);

/** Writes `length` bytes to `path`, creating its missing parent folders; 1 when written. */
int parity_write_file(const char *path, const char *bytes, size_t length);

/** Copies the file at `source` to `target`, creating the target's missing parent folders; 1 when copied. */
int parity_copy_file(const char *source, const char *target);

/**
 * The names of the folders, or with `folders` 0 of everything else, directly inside `path`, sorted, as a cJSON array
 * of texts; NULL when it cannot be read.
 */
cJSON *parity_names_in(const char *path, int folders);

/**
 * Adds every file under `home`/`folder`, at any depth, to the `files` object, keyed by its home-relative path with
 * slashes and holding its bytes as text. A folder that is absent, or is not a folder, adds nothing. 1 when every file
 * was read.
 */
int parity_add_files_under(const char *home, const char *folder, cJSON *files);

#endif
