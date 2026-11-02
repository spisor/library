#ifndef RUNTIME_PATH_ENTRY_H
#define RUNTIME_PATH_ENTRY_H

#include "cJSON.h"
#include "change_kinds.h"

/**
 * Why directory cannot be put on PATH on this OS, or NULL when it can. A path-entry change is
 * { kind: "path-entry", directory, home }: path_entry_windows.c keeps it as one entry of the user's
 * Path value, path_entry_posix.c applies it as profile-line children, one per profile that exists.
 */
const char *runtime_path_entry_defect(const char *directory);

/**
 * The changes a path-entry is applied as, recorded as its children: on POSIX an array of profile-line
 * changes adding export PATH="<directory>:$PATH" to each of home's .profile, .bashrc and .zshrc that
 * exists, and REFUSED when none does; on Windows NULL, as the entry needs none. Freed with cJSON_Delete.
 */
runtime_change_step runtime_path_entry_children(const cJSON *change, cJSON **children, runtime_change_note *note);

/** As runtime_change_prepare, for a path-entry. */
runtime_change_step runtime_path_entry_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                               runtime_change_note *note);

/** As runtime_change_perform, for a path-entry. */
runtime_change_step runtime_path_entry_perform(const cJSON *change, cJSON *record, runtime_change_note *note);

/** As runtime_change_is_on_disk, for a path-entry. */
int runtime_path_entry_is_on_disk(const cJSON *record);

/** As runtime_change_reverse, for a path-entry. */
runtime_change_reversal runtime_path_entry_reverse(const cJSON *record, int held, runtime_change_note *note);

#endif
