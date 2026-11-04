#ifndef HOST_OUTPUT_H
#define HOST_OUTPUT_H

#include "cJSON.h"

/** What a forebay-host command exits with: done, refused, or misused. */
#define HOST_EXIT_DONE 0
#define HOST_EXIT_REFUSED 1
#define HOST_EXIT_MISUSED 2

#define HOST_REINSTALL_FIX "run forebay-host install --catalog <path or url> again"

/**
 * Writes the UTF-8 text to standard output, or to standard error when to_error is set.
 *
 * @implNote A Windows console is written through WriteConsoleW, since the bytes of a non-ASCII path would
 * otherwise be shown in the console's codepage; a redirected stream gets the UTF-8 bytes unchanged.
 */
void host_write(int to_error, const char *text);

/** Prints value as JSON.stringify(value, null, 2) does, and a newline, on standard output. */
void host_print_json(const cJSON *value);

/** Prints refusal, a {message, fix}, as "<message>\nfix: <fix>" on standard error; NULL is out of memory. */
void host_print_refusal(const cJSON *refusal);

/** host_print_refusal, then HOST_EXIT_REFUSED; refusal is freed. */
int host_refuse(cJSON *refusal);

/** Prints message and the usage on standard error and answers HOST_EXIT_MISUSED. */
int host_misuse(const char *message);

#endif
