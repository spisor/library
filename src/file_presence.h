#ifndef SPISOR_FILE_PRESENCE_H
#define SPISOR_FILE_PRESENCE_H

typedef enum spisor_file_presence {
    SPISOR_FILE_PRESENT,
    SPISOR_FILE_MISSING,
    SPISOR_FILE_UNKNOWN
} spisor_file_presence;

/**
 * Whether something exists at path, telling a path that genuinely does not exist (MISSING) apart from
 * one the OS would not answer for (UNKNOWN), such as a parent without search permission.
 *
 * @implNote MISSING is only POSIX ENOENT or ENOTDIR, or Windows ERROR_FILE_NOT_FOUND or
 * ERROR_PATH_NOT_FOUND. A caller about to replace a file must treat UNKNOWN as present, so it never
 * replaces a file it failed to see.
 */
spisor_file_presence spisor_probe_file(const char *path);

#endif
