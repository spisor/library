#ifndef SPISOR_WIDE_PATH_H
#define SPISOR_WIDE_PATH_H

#if defined(_WIN32)
#include <wchar.h>

/** Whether path is valid UTF-8, all spisor_wide_path needs of it, read without allocating. */
int spisor_is_convertible_path(const char *path);
/**
 * The UTF-8 path as UTF-16 for the wide Windows APIs, freed; NULL when it is not valid UTF-8
 * or memory runs out, so NULL for a path spisor_is_convertible_path accepts is out of memory alone.
 * A path whose full form reaches MAX_PATH comes back full, with the \\?\ prefix
 * (\\?\UNC\ for a share), so a deep cache is stored and swept like any other.
 *
 * @implNote The prefix turns off the Win32 path parsing, so the path is made full first by
 * GetFullPathNameW, which also turns / into \ and resolves . and ..; a shorter path is passed on as
 * given, so nothing that worked before changes. 248 rather than 260 because CreateDirectoryW keeps
 * twelve characters for the 8.3 name of a file inside it.
 */
wchar_t *spisor_wide_path(const char *path);

/**
 * spisor_wide_path's form of an absolute path in normal form, written into buffer without allocating: whether it was
 * valid UTF-8 and fit in capacity characters.
 *
 * @implNote No full form is asked of the OS, so a path holding . or .. or a relative one is passed on as given.
 */
int spisor_wide_path_into(const char *path, wchar_t *buffer, size_t capacity);
#endif

#endif
