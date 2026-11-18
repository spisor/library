#ifndef HOST_MARKETPLACE_COPIES_H
#define HOST_MARKETPLACE_COPIES_H

#include "cJSON.h"
#include "host_context.h"
#include "runtime/marketplace.h"

/** The copies read, in the order listed; each copy is held by its read answer until the release. */
typedef struct host_marketplace_copies {
    runtime_marketplace_copy *copies;
    runtime_marketplace_copy_answer *answers;
    size_t count;
    runtime_marketplace_sources_answer sources;
} host_marketplace_copies;

/**
 * Reads the copy of every marketplace the portable settings list into `copies`, in the order listed. 1, or 0 with
 * *refusal set when the settings or the list of sources cannot be read, NULL when memory ran out; `copies` is freed
 * with host_release_marketplace_copies either way.
 *
 * A source with a provider instead of a url cannot be read from C, and a source whose copy marketplace.read refuses is
 * left out likewise: each is printed to standard error with its fix, escaped, and the others are still read.
 *
 * @implNote Each fetch goes to a fresh folder under the cache that this removes again, since marketplace.read
 * refuses a destination that already exists and deletes only the file it fetched.
 */
int host_read_marketplace_copies(const host_context *context, host_marketplace_copies *copies, cJSON **refusal);

/** Frees what host_read_marketplace_copies read and empties `copies`. */
void host_release_marketplace_copies(host_marketplace_copies *copies);

#endif
