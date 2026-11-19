#ifndef RUNTIME_MARKETPLACE_SOURCES_H
#define RUNTIME_MARKETPLACE_SOURCES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * marketplace.sources: answers {sources, skipped} from the marketplaces setting of the merged settings object, an array
 * of {id, url} where url is a file: or https: url, or of {id, provider} where provider names a plugin the host asks for
 * the copy. A source with a url answers {id, url} and one with a provider answers {id, provider}, which this library
 * never fetches.
 *
 * An id is the "marketplace" of marketplace/name, so it must be an import name. An entry that is no object, has an id
 * that is no import name, or has not exactly one of a usable url and a provider is left out and listed in skipped as
 * {index, detail}, as catalog.sources lists what it leaves out. Two sources with one id are refused, naming both, since
 * a name could not say which of them it meant.
 */
/** Whether url is a file: or https: url with something after the scheme, the only kinds a source may name. */
int runtime_marketplace_is_usable_url(const char *url);

cJSON *runtime_marketplace_sources(const cJSON *settings, cJSON **refusal);

#endif
