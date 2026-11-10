#ifndef RUNTIME_MARKETPLACE_SOURCES_H
#define RUNTIME_MARKETPLACE_SOURCES_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * marketplace.sources: {settings} answers {sources, skipped} from the setting marketplaces, an array of {id, url} where
 * url is a file: or https: url, or of {id, provider} where provider names a plugin the host asks for the copy. A source
 * with a url answers {id, url} and one with a provider answers {id, provider}, which this library never fetches.
 *
 * An id is the "marketplace" of marketplace/name, so it must be an import name. An entry that is no object, has an id
 * that is no import name, or has not exactly one of a usable url and a provider is left out and listed in skipped as
 * {index, detail}, as catalog.sources lists what it leaves out. Two sources with one id are refused, naming both, since
 * a name could not say which of them it meant.
 */
RUNTIME_EXPORT int32_t runtime_marketplace_sources(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
