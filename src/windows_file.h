#ifndef SPISOR_WINDOWS_FILE_H
#define SPISOR_WINDOWS_FILE_H

#if defined(_WIN32)
#include <stdio.h>

/**
 * Opens the UTF-8 path as a binary stream, for reading an existing file or, with writing, creating or
 * truncating one; NULL when it cannot be opened.
 *
 * @implNote Opened through CreateFileW sharing read, write and delete, then handed to the C runtime with
 * _open_osfhandle, because _wfopen shares no delete access: a reader holding a file open made every
 * MoveFileExW that replaces it fail, so one host replacing a hint, a marker or a cached file failed
 * whenever another happened to be reading it.
 */
FILE *spisor_windows_open(const char *path, int writing);

/**
 * Renames from over to, replacing it, as one step. 1 on success.
 *
 * @implNote Retried a bounded number of times on a sharing violation or access denied, which is what a
 * replace meets while another process (a virus scanner, the search indexer, or a reader opened by
 * older code) briefly holds the destination; any other failure is answered at once.
 */
int spisor_windows_move_over(const char *from, const char *to);
#endif

#endif
