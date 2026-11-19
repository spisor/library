#ifndef RUNTIME_REGISTRY_SNAPSHOT_H
#define RUNTIME_REGISTRY_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "prepared_undo.h"
#include "registry.h"

/** A registration's file as it stood, which runtime_registry_put_back writes again without allocating. */
typedef struct runtime_registry_snapshot {
    uint8_t *bytes;
    size_t length;
    spisor_prepared_replacement file;
} runtime_registry_snapshot;

/**
 * Takes the file of host id's registration as it stands into *snapshot: OK, or OUT_OF_MEMORY, INVALID, or UNREADABLE
 * when it cannot be read or prepared, *snapshot then holding nothing. The caller holds runtime_hosts_lock.
 */
runtime_registry_answer runtime_registry_take_snapshot(const char *state, const char *id,
                                                       runtime_registry_snapshot *snapshot);

/** Writes the snapshot back over the registration's file, allocating nothing: whether it was. */
int runtime_registry_put_back(const runtime_registry_snapshot *snapshot);

/** Releases what the snapshot holds; one never taken, or released already, holds nothing. */
void runtime_registry_release_snapshot(runtime_registry_snapshot *snapshot);

#endif
