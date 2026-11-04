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
 * older code) briefly holds the destination. Access denied on a directory or a read-only file can never
 * clear, so it is answered at once, as is any other failure.
 */
int spisor_windows_move_over(const char *from, const char *to);

/**
 * Renames the directory from to to, which must not exist. 1 on success; 0 at once when to exists
 * (ERROR_ALREADY_EXISTS), which the caller judges.
 *
 * @implNote Retried as spisor_windows_move_over retries, on a sharing violation or access denied, which
 * is what a move meets while a virus scanner reads a fresh file inside the folder.
 */
int spisor_windows_move_directory(const char *from, const char *to);
#endif

#endif
