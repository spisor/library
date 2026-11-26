#ifndef RUNTIME_MARKETPLACE_NAME_LOOKUP_H
#define RUNTIME_MARKETPLACE_NAME_LOOKUP_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * marketplace.lookup: in copies, an array of {source, copy}, answers for name {id, at, marketplace}, the id and places
 * of the one entry the name means and the id of the first source, in the order of copies, it was found in; {ambiguous:
 * ["marketplace/name", ...]} when a bare name is found under more than one id, every match listed in the form that
 * settles it; or {notFound: true}. The same id in several marketplaces is the same plugin and settles.
 *
 * name is a bare name, searched in every copy, or marketplace/name, searched in that marketplace's copy only, so a
 * marketplace that has no copy answers notFound. Names are compared as written, since an import name is lowercase.
 * Refuses a copy that breaks marketplace-copy.schema.json and two copies from one source, naming the source.
 */
cJSON *runtime_marketplace_lookup(const cJSON *copies, const char *name, cJSON **refusal);

#endif
