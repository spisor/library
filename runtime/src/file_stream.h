#ifndef RUNTIME_FILE_STREAM_H
#define RUNTIME_FILE_STREAM_H

#include <stdio.h>

typedef enum runtime_file_mode {
    RUNTIME_FILE_READ,
    RUNTIME_FILE_WRITE
} runtime_file_mode;

/**
 * The file at the UTF-8 path, opened for binary reading, or for binary writing from empty. NULL when it
 * cannot be opened.
 *
 * @implNote Windows opens through spisor_windows_open, wide and sharing delete access, so a non-ASCII name
 * resolves whatever the process code page is and an open reader never blocks a replace.
 */
FILE *runtime_open_file(const char *path, runtime_file_mode mode);

/** Renames from onto to, replacing a file already there. 1 on success. */
int runtime_move_file_over(const char *from, const char *to);

#endif
