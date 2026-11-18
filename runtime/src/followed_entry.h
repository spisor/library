#ifndef RUNTIME_FOLLOWED_ENTRY_H
#define RUNTIME_FOLLOWED_ENTRY_H

/**
 * Whether path, a link followed to its target as Node's statSync follows it, is a directory; 0 with
 * *out_of_memory set to 1 when memory ran out asking.
 */
int runtime_is_followed_directory(const char *path, int *out_of_memory);

/**
 * Whether path, a link followed to its target, is a regular file, and with require_execute_bit on
 * POSIX one carrying any execute bit. Windows has no execute bit, so there it is only a file. 0 with
 * *out_of_memory set to 1 when memory ran out asking.
 */
int runtime_is_followed_file(const char *path, int require_execute_bit, int *out_of_memory);

#endif
