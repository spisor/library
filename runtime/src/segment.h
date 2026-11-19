#ifndef RUNTIME_SEGMENT_H
#define RUNTIME_SEGMENT_H

#include "spisor/status.h"

/**
 * The name trimmed as String.prototype.trim does, in *segment and freed: SPISOR_MALFORMED, before anything is
 * allocated, when the result is not a portable path segment, and SPISOR_OUT_OF_MEMORY with *segment NULL.
 *
 * @implNote Portable means usable on every OS the host runs on: not empty, ".", ".." or ending in "."
 * or a space, free of "/", "\", ":", "<", ">", the double quote, "|", "?", "*" and the control
 * characters U+0000 to U+001F, and with a stem (the part before the first ".") that is not a Windows
 * reserved device name, compared ignoring case. U+0000 never arrives, since spisor_parse_json_reporting refuses
 * a document that escapes it.
 */
spisor_status runtime_portable_segment(const char *name, char **segment);

/** Whether the name trimmed is a portable path segment, as runtime_portable_segment would answer it, unallocated. */
int runtime_trims_to_portable_segment(const char *name);

/** Whether name is a portable path segment as it stands, as runtime_portable_segment would answer it unchanged. */
int runtime_is_portable_segment(const char *name);

#endif
