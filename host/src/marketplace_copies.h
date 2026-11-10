#ifndef HOST_MARKETPLACE_COPIES_H
#define HOST_MARKETPLACE_COPIES_H

#include "cJSON.h"
#include "host_context.h"

/**
 * Reads the copy of every marketplace the portable settings list, as an array of {source: {id}, copy} in the order
 * listed. NULL with *refusal set when the settings or the list of sources cannot be read, or when memory ran out.
 *
 * A source with a provider instead of a url cannot be read from C, and a source whose copy marketplace.read refuses is
 * left out likewise: each is printed to standard error with its fix, escaped, and the others are still read.
 *
 * @implNote Each fetch goes to a fresh folder under the cache that this removes again, since marketplace.read
 * refuses a destination that already exists and deletes only the file it fetched.
 */
cJSON *host_read_marketplace_copies(const host_context *context, cJSON **refusal);

#endif
