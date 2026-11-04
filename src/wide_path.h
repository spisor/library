#ifndef SPISOR_WIDE_PATH_H
#define SPISOR_WIDE_PATH_H

#if defined(_WIN32)
#include <wchar.h>

/**
 * The UTF-8 path as UTF-16 for the wide Windows APIs, freed with free; NULL when it is not valid UTF-8
 * or memory runs out. A path whose full form reaches MAX_PATH comes back full, with the \\?\ prefix
 * (\\?\UNC\ for a share), so a deep cache is stored and swept like any other.
 *
 * @implNote The prefix turns off the Win32 path parsing, so the path is made full first by
 * GetFullPathNameW, which also turns / into \ and resolves . and ..; a shorter path is passed on as
 * given, so nothing that worked before changes. 248 rather than 260 because CreateDirectoryW keeps
 * twelve characters for the 8.3 name of a file inside it.
 */
wchar_t *spisor_wide_path(const char *path);
#endif

#endif
