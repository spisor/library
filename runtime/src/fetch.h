#ifndef RUNTIME_FETCH_H
#define RUNTIME_FETCH_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * fetch.get: fetches url into destination and answers {outcome: "stored", sha256, bytes, etag?,
 * lastModified?}, or {outcome: "not-modified"} when the server answers 304 to the etag or lastModified
 * sent; anything else is refused.
 *
 * @implNote The bytes go to destination.partial-PID and are renamed into place only after they are
 * complete, within maxBytes, and match sha256 when it is given, so a refusal never leaves a file at
 * destination or the partial one beside it. A file: url is copied through the same path, so a local
 * catalog and a published one are verified alike.
 */
RUNTIME_EXPORT int32_t runtime_fetch_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
