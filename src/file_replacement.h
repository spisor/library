#ifndef SPISOR_FILE_REPLACEMENT_H
#define SPISOR_FILE_REPLACEMENT_H

#include <stdio.h>

/**
 * Creates the temporary sibling a replacement of target is written into, open for binary writing,
 * and hands its path back in *temporary_path (freed with free). NULL when it cannot be created.
 *
 * @implNote On POSIX the file is created exclusively and given its final mode before this returns,
 * before any byte reaches it: 0600 when owner_only, else target's own mode when target exists, else
 * 0666 less the umask. Windows ignores owner_only, as it has no mode to give.
 */
FILE *spisor_open_replacement(const char *target, int owner_only, char **temporary_path);

#endif
