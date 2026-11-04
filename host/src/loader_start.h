#ifndef HOST_LOADER_START_H
#define HOST_LOADER_START_H

#include "cJSON.h"
#include "host_context.h"

/** What a start of a loader settled before it runs anything. */
typedef struct loader_start {
    cJSON *registration;
    cJSON *plugins;
    cJSON *standard;
    char *version;
} loader_start;

/**
 * What every start of loader id does, a launch or the application's own hook: switches it to the version
 * its references schedule once that is installed, keeping the one it ran for rollback (updates.apply),
 * registers it at that version, requesting its commands again, and reads that version's host.json; 1, or 0
 * with *refusal set. A registration that cannot be written is reported on stderr and the start carries
 * on. The plugins are the ones its settings enable, which only a launch starts.
 *
 * @implNote The run record updates.apply writes for this process is removed at once: the references file
 * already keeps the version this start runs, and nothing here stamps a record while it lives.
 */
int host_start_loader(const host_context *context, const char *id, loader_start *start, cJSON **refusal);

void host_finish_loader(loader_start *start);

#endif
