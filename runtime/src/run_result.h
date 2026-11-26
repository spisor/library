#ifndef RUNTIME_RUN_RESULT_H
#define RUNTIME_RUN_RESULT_H

#include "spisor/status.h"
#include "spisor/value.h"

/** The folder of the state place an ACTION run writes its action's result into, one file per run. */
#define RUNTIME_RESULTS_FOLDER "results"

/**
 * Whether path, as written, names one file directly inside `<state>/results`: absolute, no segment `.` or `..`, a
 * last segment that is a portable file name, and the rest the results folder itself once resolved. *out_of_memory is
 * set to 1 when memory ran out asking, which answers 0.
 *
 * @implNote A drive-relative Windows path is not absolute, and on Windows either separator ends a segment, so neither
 * reaches past the check; a link is the write's to refuse, since only the file system can say what one points at.
 */
int runtime_is_result_path(const char *state, const char *path, int *out_of_memory);

/**
 * Writes result, as compact JSON and `null` for a value of kind NONE, to path, which runtime_is_result_path accepted,
 * as a new file: the results folder made when missing, and refused when it is not a real folder, a link to one
 * included, or when anything already stands at path.
 *
 * @return OK; MALFORMED for a result spisor_json_of_value refuses beside NONE; REFUSED, with the refusal filled, when
 * it cannot be written so; OUT_OF_MEMORY.
 */
spisor_status runtime_write_result(const char *state, const char *path, const spisor_value *result,
                                   spisor_refusal *refusal);

#endif
