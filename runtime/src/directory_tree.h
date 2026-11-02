#ifndef RUNTIME_DIRECTORY_TREE_H
#define RUNTIME_DIRECTORY_TREE_H

#include <stddef.h>
#include <stdint.h>

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

typedef enum runtime_entry_kind {
    RUNTIME_ENTRY_MISSING,
    RUNTIME_ENTRY_DIRECTORY,
    RUNTIME_ENTRY_OTHER,
    RUNTIME_ENTRY_UNKNOWN
} runtime_entry_kind;

/**
 * What stands at path itself, never following a link: a symbolic link, or on Windows any reparse
 * point, is OTHER whatever it points at. UNKNOWN when the OS would not say.
 */
runtime_entry_kind runtime_probe_entry(const char *path);

typedef enum runtime_entry_outcome {
    RUNTIME_ENTRY_DONE,
    RUNTIME_ENTRY_EXISTS,
    RUNTIME_ENTRY_KEPT,
    RUNTIME_ENTRY_FAILED
} runtime_entry_outcome;

/**
 * Creates path as a new file holding length bytes, executable by its owner on POSIX when asked.
 * EXISTS when anything, a link included, already stands at path, which is left untouched; DONE when
 * created. The parent must exist.
 */
runtime_entry_outcome runtime_create_new_file(const char *path, const uint8_t *bytes, size_t length, int executable);

/** Creates the directory path itself, never its parents. DONE when a directory stands there afterwards. */
runtime_entry_outcome runtime_create_directory(const char *path);

/**
 * Deletes the file or link at path, never following a link. DONE when nothing is left at path, KEPT
 * when a real directory stands there, which is left.
 */
runtime_entry_outcome runtime_remove_file_entry(const char *path);

/**
 * Deletes path when it is a real, empty directory. DONE when nothing is left at path, KEPT when it
 * holds anything or is not a real directory (a link to one included), which is left.
 */
runtime_entry_outcome runtime_remove_empty_directory(const char *path);

#endif
