#ifndef SPISOR_PREPARED_UNDO_H
#define SPISOR_PREPARED_UNDO_H

#if defined(_WIN32)
#include <wchar.h>
#endif

#include <stddef.h>
#include <stdint.h>

#include "spisor/status.h"

/**
 * A path an undo may delete or move a file to or from, held in every form those need.
 *
 * @implNote AN UNDO MUST NEVER ALLOCATE. An undo runs because something failed, and when that was memory running out
 * every later allocation can fail too, so an undo that builds a path, a wide form or a text at undo time leaves the
 * very change it exists to reverse. Everything an undo needs is prepared before the first write it would reverse,
 * here or in the caller's own prepared state, and the undo uses only what was prepared.
 */
typedef struct spisor_prepared_path {
    char *path;
#if defined(_WIN32)
    wchar_t *wide;
#endif
} spisor_prepared_path;

/** Prepares path, which need not exist yet: SPISOR_OK, or SPISOR_OUT_OF_MEMORY or SPISOR_REFUSED holding nothing. */
spisor_status spisor_prepare_path(const char *path, spisor_prepared_path *prepared);

/** Deletes the prepared file, allocating nothing: whether no file stands there afterwards. */
int spisor_remove_prepared_file(const spisor_prepared_path *prepared);

/** Deletes the prepared folder when it is empty, allocating nothing: whether no folder stands there afterwards. */
int spisor_remove_prepared_directory(const spisor_prepared_path *prepared);

/** Renames the file from over to, replacing it, allocating nothing: whether it was moved. */
int spisor_move_prepared_file(const spisor_prepared_path *from, const spisor_prepared_path *to);

/** Hard-links the file at existing to linked, never replacing one there, allocating nothing: whether it was linked. */
int spisor_link_prepared_file(const spisor_prepared_path *existing, const spisor_prepared_path *linked);

/** Reads at most capacity bytes of the prepared file, allocating nothing: whether it was read, *length the bytes. */
int spisor_read_prepared_file(const spisor_prepared_path *prepared, uint8_t *buffer, size_t capacity, size_t *length);

/** Cuts the prepared file back to its first length bytes, links followed, allocating nothing: whether it was. */
int spisor_truncate_prepared_file(const spisor_prepared_path *prepared, uint64_t length);

/** Releases what was prepared; a path never prepared, or released already, holds nothing. */
void spisor_release_prepared_path(spisor_prepared_path *prepared);

/**
 * Deletes the file at path, allocating nothing: whether no file stands there afterwards. For an undo whose path reaches
 * it only as text, across an operation's answer, so no removal could be prepared beside the write; path is absolute
 * and in normal form, as every joined path is.
 *
 * @implNote The wide form is built in a buffer on the stack, sized for the longest path Windows accepts.
 */
int spisor_remove_file_in_place(const char *path);

/** A file an undo may write back whole: its temporary sibling and the file's own path, both prepared. */
typedef struct spisor_prepared_replacement {
    spisor_prepared_path temporary;
    spisor_prepared_path target;
} spisor_prepared_replacement;

/** Prepares replacing the file at path as spisor_replace_private_file does, links not followed. */
spisor_status spisor_prepare_replacement(const char *path, spisor_prepared_replacement *replacement);

/** Replaces the prepared file with bytes at once, allocating nothing: whether it was; its sibling is gone if not. */
int spisor_replace_prepared(const spisor_prepared_replacement *replacement, const uint8_t *bytes, size_t length);

void spisor_release_prepared_replacement(spisor_prepared_replacement *replacement);

#endif
