#ifndef HOST_OUTPUT_H
#define HOST_OUTPUT_H

#include "cJSON.h"

/** What a spisor-host command exits with: done, refused, or misused. */
#define HOST_EXIT_DONE 0
#define HOST_EXIT_REFUSED 1
#define HOST_EXIT_MISUSED 2

#define HOST_REINSTALL_FIX "run spisor-host install --catalog <path or url> again"

/**
 * Writes the UTF-8 text to standard output, or to standard error when to_error is set.
 *
 * @implNote A Windows console is written through WriteConsoleW, since the bytes of a non-ASCII path would
 * otherwise be shown in the console's codepage; a redirected stream gets the UTF-8 bytes unchanged.
 */
void host_write(int to_error, const char *text);

/** Prints value as JSON.stringify(value, null, 2) does, and a newline, on standard output. */
void host_print_json(const cJSON *value);

/**
 * text as it may be printed on a terminal: every byte below 0x20, 0x7F and the UTF-8 form of each of U+0080 to
 * U+009F becomes a visible \\xNN or \\u00NN, so a name read from elsewhere cannot move the cursor or hide the line. NULL when
 * memory runs out.
 */
char *host_printable(const char *text);

/** Prints refusal, a {message, fix}, as "<message>\nfix: <fix>" on standard error; NULL is out of memory. */
void host_print_refusal(const cJSON *refusal);

/** host_print_refusal with every name in the message and the fix escaped as host_printable does. */
void host_print_refusal_printably(const cJSON *refusal);

/** host_print_refusal_printably, then HOST_EXIT_REFUSED; refusal is freed. */
int host_refuse_printably(cJSON *refusal);

/** host_print_refusal, then HOST_EXIT_REFUSED; refusal is freed. */
int host_refuse(cJSON *refusal);

/** Prints message and the usage on standard error and answers HOST_EXIT_MISUSED. */
int host_misuse(const char *message);

#endif
