#ifndef HOST_OPTIONS_H
#define HOST_OPTIONS_H

#include <stddef.h>

#include "cJSON.h"

/** One --name option taking a value, given as --name value or --name=value, as often as it is given. */
typedef struct host_option {
    const char *name;
    cJSON *values;
} host_option;

/**
 * Reads arguments into each option's values, an array of strings in the order given, and every other
 * argument into positionals, which may be NULL when the command takes none. 1, or 0 with *problem set to
 * a message the caller frees, for an option it does not know, one missing its value, or a positional it
 * takes none of. host_free_options frees the values.
 */
int host_parse_options(int count, char **arguments, host_option *options, size_t option_count, cJSON *positionals,
                       char **problem);

void host_free_options(host_option *options, size_t option_count);

/** The last value of option, or NULL when it was not given. */
const char *host_last_value(const host_option *option);

#endif
