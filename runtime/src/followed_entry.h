#ifndef RUNTIME_FOLLOWED_ENTRY_H
#define RUNTIME_FOLLOWED_ENTRY_H

/** Whether path, a link followed to its target as Node's statSync follows it, is a directory. */
int runtime_is_followed_directory(const char *path);

/**
 * Whether path, a link followed to its target, is a regular file, and with require_execute_bit on
 * POSIX one carrying any execute bit. Windows has no execute bit, so there it is only a file.
 */
int runtime_is_followed_file(const char *path, int require_execute_bit);

#endif
