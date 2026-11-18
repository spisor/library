#ifndef SPISOR_FILE_REPLACEMENT_H
#define SPISOR_FILE_REPLACEMENT_H

#include <stdio.h>

#include "prepared_undo.h"

/** A free temporary name beside path, freed; NULL when memory ran out for it. */
char *spisor_temporary_path_for(const char *path);

/**
 * Creates the temporary sibling a replacement of target is written into, open for binary writing, and hands its
 * removal back in *temporary, prepared before the file was created and released by the caller. NULL when it cannot be
 * created, with nothing to release.
 *
 * @implNote On POSIX the file is created exclusively and given its final mode before this returns,
 * before any byte reaches it: 0600 when owner_only, else target's own mode when target exists, else
 * 0666 less the umask. Windows ignores owner_only, as it has no mode to give.
 */
FILE *spisor_open_replacement(const char *target, int owner_only, spisor_prepared_path *temporary,
                              int *out_of_memory);

#endif
