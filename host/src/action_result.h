#ifndef HOST_ACTION_RESULT_H
#define HOST_ACTION_RESULT_H

#include "host_context.h"

/**
 * A path for this process's next result, `<state>/results/<pid>-<n>.json`, with nothing left standing at it, freed;
 * NULL when memory runs out or what stands there cannot be removed, *removable then 0 for the latter.
 *
 * @implNote The name is this process's own, so whatever stands at it is an earlier process's of the same id, which
 * the OS reuses; a run would refuse to write over it and its caller would read it as this run's result.
 */
char *host_result_path(const host_context *context, int *removable);

/**
 * The result the action's run wrote to path when its child exited 0, freed, NULL for none; the file is removed either
 * way. *out_of_memory is set to 1 when memory ran out reading or removing it.
 */
char *host_take_result(const char *path, int exit_code, int *out_of_memory);

#endif
