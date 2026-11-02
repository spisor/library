#ifndef RUNTIME_DIRECTORY_TREE_H
#define RUNTIME_DIRECTORY_TREE_H

#include <stddef.h>

typedef struct runtime_directory_entry {
    char *name;
    int is_directory;
} runtime_directory_entry;

typedef struct runtime_directory_listing {
    runtime_directory_entry *entries;
    size_t count;
} runtime_directory_listing;

/**
 * Fills listing with the entries directly inside path, sorted by name, "." and ".." left out. A
 * path that does not exist lists as empty. Answers 1 on success and 0 when the directory exists but
 * cannot be read or memory runs out, leaving listing empty.
 */
int runtime_list_directory(const char *path, runtime_directory_listing *listing);

void runtime_free_directory_listing(runtime_directory_listing *listing);

/**
 * Deletes path and everything under it. A path that does not exist counts as removed. Answers 1
 * when nothing is left at path afterwards.
 *
 * @implNote A symbolic link, or on Windows any reparse point, is removed itself and never followed,
 * so a link inside a host's own folder cannot lead the deletion out of it.
 */
int runtime_remove_tree(const char *path);

#endif
