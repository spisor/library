#ifndef RUNTIME_ON_PATH_H
#define RUNTIME_ON_PATH_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * The executable the bare command name binary resolves to on the PATH of environment, freed with free;
 * NULL when none does or memory runs out. windows chooses the rules, so both run on any OS: on Windows
 * PATH and PATHEXT are found ignoring the case of their names, binary is tried with each PATHEXT
 * extension in its order (.COM;.EXE;.BAT;.CMD when PATHEXT is unset or empty) and then bare, every name
 * matched ignoring ASCII case; on POSIX the entry must be a regular file carrying an execute bit.
 *
 * @implNote Reads directories and probes files only, and never launches anything (shared-host spec 8).
 * The bare name comes last on Windows, so an extensionless npm shell script never wins over the .cmd
 * beside it that Windows can run.
 */
char *runtime_find_on_path(int windows, const char *binary, const cJSON *environment);

/** detect.onPath: { binary, environment } answering { path? }, runtime_find_on_path under this OS's rules. */
RUNTIME_EXPORT int32_t runtime_detect_on_path(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
