#ifndef RUNTIME_MARKETPLACE_READ_H
#define RUNTIME_MARKETPLACE_READ_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** The most bytes a marketplace copy may have, which the entry limit of its schema keeps well inside. */
#define RUNTIME_MARKETPLACE_MAXIMUM_BYTES (8 * 1024 * 1024)

/**
 * What marketplace.read reads: the source source_id serving url, as marketplace.sources answers it, from text, the body
 * the host already fetched, or, with text NULL, by fetching url into destination.
 */
typedef struct runtime_marketplace_read_request {
    const char *source_id;
    const char *url;
    const char *text;
    const char *destination;
} runtime_marketplace_read_request;

/**
 * marketplace.read: answers {copy}, the marketplace copy that the source served, checked as
 * marketplace-copy.schema.json describes it, or refuses naming the source and how to fix it. With destination, an
 * absolute path that does not exist yet, the url, a file: or https: one, is fetched there through fetch.get within
 * RUNTIME_MARKETPLACE_MAXIMUM_BYTES and the file is removed once read.
 *
 * Refuses a body over the limit, one that is a web page, one that is no JSON, a marketplace list where a copy is
 * expected (the fix being to build the copy from it), a copy that breaks its schema or names two entries alike, and a
 * source that has a provider instead of a url, which only the host can ask.
 *
 * @implNote A refusal is per call, so a host reading several sources calls once for each and one bad source stops no
 * other.
 */
cJSON *runtime_marketplace_read(const runtime_marketplace_read_request *request, cJSON **refusal);

#endif
