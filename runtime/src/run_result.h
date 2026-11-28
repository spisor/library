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

/** What stands at a result path a caller of a run left behind: a file an earlier process left there. */
#define RUNTIME_STALE_RESULT_MESSAGE \
    "a file an earlier spisor-host left in the state place's results folder could not be removed"
#define RUNTIME_STALE_RESULT_FIX "remove the files in the results folder of the state place, then run the action again"

/**
 * A path for this process's next result, `<state>/results/<pid>-<n>.json`, n counted from any thread, with nothing
 * left standing at it, freed; NULL when memory runs out or what stands there cannot be removed, *removable then 0 for
 * the latter.
 *
 * @implNote The name is this process's own, so whatever stands at it is an earlier process's of the same id, which
 * the OS reuses; a run would refuse to write over it and its caller would read it as this run's result.
 */
char *runtime_fresh_result_path(const char *state, int *removable);

/**
 * The result the action's run wrote to path when its child exited 0, freed, NULL for none; the file is removed either
 * way. *out_of_memory is set to 1 when memory ran out reading or removing it, the result then NULL.
 *
 * @implNote Removing can itself need memory (a wide path on Windows), so a removal memory refused is tried once more
 * with the result's text released first; a file even that cannot remove is the cache sweep's a day later.
 */
char *runtime_take_result(const char *path, int exit_code, int *out_of_memory);

#endif
