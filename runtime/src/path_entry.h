#ifndef RUNTIME_PATH_ENTRY_H
#define RUNTIME_PATH_ENTRY_H

#if defined(_WIN32)
#include <wchar.h>
#endif

#include "cJSON.h"
#include "change_kinds.h"
#include "spisor/status.h"

/**
 * Why directory cannot be put on PATH on this OS, or NULL when it can. A path-entry change is
 * { kind: "path-entry", directory, home, shell? }: path_entry_windows.c keeps it as one entry of the
 * user's Path value, path_entry_posix.c applies it as profile-line children.
 */
const char *runtime_path_entry_defect(const char *directory);

/**
 * The changes a path-entry is applied as, recorded as its children: on POSIX an array of profile-line
 * changes adding export PATH="<directory>:$PATH" to each of home's .profile, .bashrc, .zshrc and
 * .zprofile that exists, and to the login shell's profile (runtime_path_entry_login_profile), which is
 * created when missing; on Windows NULL, as the entry needs none. Freed with cJSON_Delete.
 */
runtime_change_step runtime_path_entry_children(const cJSON *change, cJSON **children, runtime_change_note *note);

/**
 * POSIX only: the profile the login shell reads, ".zprofile" when shell (the SHELL a caller passes,
 * or NULL) ends in zsh or macos is set, as macOS defaults to zsh, otherwise ".profile".
 */
const char *runtime_path_entry_login_profile(const char *shell, int macos);

/** As runtime_change_prepare, for a path-entry. */
runtime_change_step runtime_path_entry_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                               runtime_change_note *note);

/** As runtime_change_perform, for a path-entry. */
runtime_change_step runtime_path_entry_perform(const cJSON *change, cJSON *record, runtime_change_note *note);

/** As runtime_change_is_on_disk, for a path-entry. */
runtime_change_presence runtime_path_entry_is_on_disk(const cJSON *record);

/**
 * The user's PATH as a path-entry change found it, so its undo allocates nothing (prepared_undo.h): on Windows the
 * Path value's text and type, or that there was none; on POSIX nothing, since its profile lines are its children.
 */
typedef struct runtime_path_entry_undo {
#if defined(_WIN32)
    wchar_t *text;
    unsigned long type;
#endif
    int present;
} runtime_path_entry_undo;

/** Prepares undoing a path-entry change: SPISOR_OK, or SPISOR_OUT_OF_MEMORY or SPISOR_REFUSED holding nothing. */
spisor_status runtime_path_entry_prepare_undo(runtime_path_entry_undo *undo);
/** Puts the user's PATH back as it was prepared, allocating nothing: whether it was. */
int runtime_path_entry_restore(const runtime_path_entry_undo *undo);
void runtime_path_entry_release_undo(runtime_path_entry_undo *undo);
/** As runtime_change_reverse, for a path-entry. */
runtime_change_reversal runtime_path_entry_reverse(const cJSON *record, int held, runtime_change_note *note);

#endif
