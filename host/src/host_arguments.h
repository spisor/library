#ifndef HOST_ARGUMENTS_H
#define HOST_ARGUMENTS_H

#include <wchar.h>

/**
 * The UTF-8 copies of a wide argv, NULL-terminated and freed with host_free_arguments; NULL when one is not
 * valid UTF-16 or memory runs out.
 */
char **host_utf8_arguments(int count, wchar_t **wide);

void host_free_arguments(char **arguments, int count);

#endif
