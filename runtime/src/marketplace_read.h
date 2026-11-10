#ifndef RUNTIME_MARKETPLACE_READ_H
#define RUNTIME_MARKETPLACE_READ_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/** The most bytes a marketplace copy may have, which the entry limit of its schema keeps well inside. */
#define RUNTIME_MARKETPLACE_MAXIMUM_BYTES (8 * 1024 * 1024)

/**
 * marketplace.read: {source, text} or {source, destination} answers {copy}, the marketplace copy that source served,
 * checked as marketplace-copy.schema.json describes it, or refuses naming the source and how to fix it. source is
 * {id, url} as marketplace.sources answers it. With text, the body the host already fetched is read; with destination,
 * an absolute path that does not exist yet, the url, a file: or https: one, is fetched there through fetch.get within RUNTIME_MARKETPLACE_MAXIMUM_BYTES and the file is
 * removed once read.
 *
 * Refuses a body over the limit, one that is a web page, one that is no JSON, a marketplace list where a copy is
 * expected (the fix being to build the copy from it), a copy that breaks its schema or names two entries alike, and a
 * source that has a provider instead of a url, which only the host can ask.
 *
 * @implNote A refusal is per call, so a host reading several sources calls once for each and one bad source stops no
 * other.
 */
RUNTIME_EXPORT int32_t runtime_marketplace_read(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
