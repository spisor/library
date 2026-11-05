#ifndef RUNTIME_TEST_TREE_SNAPSHOT_H
#define RUNTIME_TEST_TREE_SNAPSHOT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "directory_tree.h"
#include "spisor/file.h"

static inline void snapshot_directory(const char *directory, const char *relative, const char *left_out,
                                      char **snapshot, size_t *length);

static inline void snapshot_append(char **snapshot, size_t *length, const char *text, size_t text_length) {
    char *grown = realloc(*snapshot, *length + text_length + 1);
    CHECK(grown != NULL);
    if (grown == NULL) {
        return;
    }
    memcpy(grown + *length, text, text_length);
    *length += text_length;
    grown[*length] = '\0';
    *snapshot = grown;
}

static inline void snapshot_entry(const char *directory, const char *relative, const runtime_directory_entry *entry,
                                  const char *left_out, char **snapshot, size_t *length) {
    char path[2048];
    char name[2048];
    snprintf(path, sizeof path, "%s/%s", directory, entry->name);
    snprintf(name, sizeof name, "%s/%s", relative, entry->name);
    if (left_out != NULL && strcmp(name, left_out) == 0) {
        return;
    }
    char header[2200];
    if (entry->is_directory) {
        snprintf(header, sizeof header, "directory %s\n", name);
        snapshot_append(snapshot, length, header, strlen(header));
        snapshot_directory(path, name, left_out, snapshot, length);
        return;
    }
    size_t content_length = 0;
    uint8_t *content = spisor_read_file(path, &content_length);
    snprintf(header, sizeof header, "file %s %zu\n", name, content_length);
    snapshot_append(snapshot, length, header, strlen(header));
    if (content != NULL) {
        snapshot_append(snapshot, length, (const char *)content, content_length);
    }
    free(content);
}

static inline void snapshot_directory(const char *directory, const char *relative, const char *left_out,
                                      char **snapshot, size_t *length) {
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(directory, &listing) == 1);
    for (size_t index = 0; index < listing.count; index++) {
        snapshot_entry(directory, relative, &listing.entries[index], left_out, snapshot, length);
    }
    runtime_free_directory_listing(&listing);
}

/**
 * Every name, kind and byte under root, freed with free, its length in *length; left_out, a path relative
 * to root written "/a/b", is skipped with everything under it, or nothing is when it is NULL.
 *
 * @implNote Compare two of these by length and bytes when the tree may hold an executable: its NUL bytes
 * end a strcmp early, and everything after them would go unseen.
 */
static inline char *tree_snapshot_sized(const char *root, const char *left_out, size_t *length) {
    char *snapshot = NULL;
    *length = 0;
    snapshot_append(&snapshot, length, "", 0);
    snapshot_directory(root, "", left_out, &snapshot, length);
    return snapshot;
}

/** tree_snapshot_sized without the length, for a tree of text alone. */
static inline char *tree_snapshot(const char *root, const char *left_out) {
    size_t length = 0;
    return tree_snapshot_sized(root, left_out, &length);
}

#endif
