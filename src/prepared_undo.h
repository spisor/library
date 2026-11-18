#ifndef SPISOR_PREPARED_UNDO_H
#define SPISOR_PREPARED_UNDO_H

#if defined(_WIN32)
#include <wchar.h>
#endif

#include <stddef.h>
#include <stdint.h>

#include "spisor/status.h"

/**
 * A file or folder an undo may delete, held in every form the delete needs.
 *
 * @implNote AN UNDO MUST NEVER ALLOCATE. An undo runs because something failed, and when that was memory running out
 * every later allocation can fail too, so an undo that builds a path, a wide form or a text at undo time leaves the
 * very change it exists to reverse. Everything an undo needs is prepared before the first write it would reverse,
 * here or in the caller's own prepared state, and the undo uses only what was prepared.
 */
typedef struct spisor_prepared_removal {
    char *path;
#if defined(_WIN32)
    wchar_t *wide;
#endif
} spisor_prepared_removal;

/** Prepares the removal of path, which need not exist yet: SPISOR_OK, SPISOR_OUT_OF_MEMORY, or SPISOR_REFUSED. */
spisor_status spisor_prepare_removal(const char *path, spisor_prepared_removal *removal);

/** Deletes the prepared file, allocating nothing: whether no file stands there afterwards. */
int spisor_remove_prepared(const spisor_prepared_removal *removal);

/** Deletes the prepared folder when it is empty, allocating nothing: whether no folder stands there afterwards. */
int spisor_remove_prepared_directory(const spisor_prepared_removal *removal);

void spisor_release_prepared_removal(spisor_prepared_removal *removal);

/**
 * Deletes the file at path, allocating nothing: whether no file stands there afterwards. For an undo whose path reaches
 * it only as text, across an operation's answer, so no removal could be prepared beside the write; path is absolute
 * and in normal form, as every joined path is.
 *
 * @implNote The wide form is built in a buffer on the stack, sized for the longest path Windows accepts.
 */
int spisor_remove_file_in_place(const char *path);

/** A file an undo may write back whole: its temporary sibling's removal and the file's own path in every form. */
typedef struct spisor_prepared_replacement {
    spisor_prepared_removal temporary;
    spisor_prepared_removal target;
} spisor_prepared_replacement;

/** Prepares replacing the file at path as spisor_replace_private_file does, links not followed. */
spisor_status spisor_prepare_replacement(const char *path, spisor_prepared_replacement *replacement);

/** Replaces the prepared file with bytes at once, allocating nothing: whether it was; its sibling is gone if not. */
int spisor_replace_prepared(const spisor_prepared_replacement *replacement, const uint8_t *bytes, size_t length);

void spisor_release_prepared_replacement(spisor_prepared_replacement *replacement);

#endif
