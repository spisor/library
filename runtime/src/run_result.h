#ifndef RUNTIME_RUN_RESULT_H
#define RUNTIME_RUN_RESULT_H

#include "random_value.h"
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

/** Why no result path could be named: the random source gave nothing, or every name drawn was taken. */
#define RUNTIME_UNNAMED_RESULT_MESSAGE \
    "no name for the action's result could be drawn that nothing in the state place's results folder holds"
#define RUNTIME_UNNAMED_RESULT_FIX \
    "check that the system's random source can be read, remove the files in the results folder of the state place, " \
    "then run the action again"

/**
 * A path for this process's next result, `<state>/results/<pid>-<n>-<r>.json`, n counted from any thread and r 64 bits
 * draw drew, in hex, at which nothing stands, freed; a name something stands at is never answered, and is drawn again
 * a few times. NULL when memory runs out, *out_of_memory then 1, or when draw gives nothing or every name drawn is
 * taken.
 *
 * @implNote The random part keeps two processes with the same pid apart, as processes in two containers sharing one
 * state place are, where pid and counter alone collide; what stands at a name is never removed, since it may be another
 * process's live result, and the run writes the file only as a new one, so a name taken after the check is refused
 * rather than written over.
 */
char *runtime_fresh_result_path_drawn(const char *state, runtime_random_draw draw, int *out_of_memory);

/** runtime_fresh_result_path_drawn with the OS's random source. */
char *runtime_fresh_result_path(const char *state, int *out_of_memory);

/**
 * The result the action's run wrote to path when its child exited 0, freed, NULL for none; the file is removed either
 * way. *out_of_memory is set to 1 when memory ran out reading or removing it, the result then NULL.
 *
 * @implNote Removing can itself need memory (a wide path on Windows), so a removal memory refused is tried once more
 * with the result's text released first; a file even that cannot remove is the cache sweep's a day later.
 */
char *runtime_take_result(const char *path, int exit_code, int *out_of_memory);

#endif
