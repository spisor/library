#ifndef RUNTIME_ON_PATH_H
#define RUNTIME_ON_PATH_H

#include "cJSON.h"

typedef enum runtime_on_path_status {
    RUNTIME_ON_PATH_FOUND,
    RUNTIME_ON_PATH_ABSENT,
    RUNTIME_ON_PATH_OUT_OF_MEMORY
} runtime_on_path_status;

/**
 * Finds the executable the bare command name binary resolves to on the PATH of environment, `*found` freed with free
 * and NULL unless FOUND. A directory that cannot be listed holds nothing. windows chooses the rules, so both run on
 * any OS: on Windows PATH and PATHEXT are found ignoring the case of their names, binary is tried with each PATHEXT
 * extension in its order (.COM;.EXE;.BAT;.CMD when PATHEXT is unset or empty) and then bare, every name matched
 * ignoring ASCII case; on POSIX the entry must be a regular file carrying an execute bit.
 *
 * @implNote Reads directories and probes files only, and never launches anything (shared-host spec 8).
 * The bare name comes last on Windows, so an extensionless npm shell script never wins over the .cmd
 * beside it that Windows can run.
 */
runtime_on_path_status runtime_find_on_path(int windows, const char *binary, const cJSON *environment,
                                            char **found);

#endif
