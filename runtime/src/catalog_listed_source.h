#ifndef RUNTIME_CATALOG_LISTED_SOURCE_H
#define RUNTIME_CATALOG_LISTED_SOURCE_H

#include <stddef.h>

#include "cJSON.h"
#include "runtime/catalog.h"
#include "spisor/status.h"

/** Whether `count` sources are listed, each of a runtime_catalog_source_kind with a location that is not empty. */
int runtime_catalog_are_sources(const runtime_catalog_source *listed, size_t count);

/** A listed source as runtime_catalog_source_json writes it; NULL when memory runs out. */
cJSON *runtime_catalog_listed_source_json(const runtime_catalog_source *source);

/** Adds the `count` listed sources to `call` as its `sources` array: OK or OUT_OF_MEMORY. */
spisor_status runtime_catalog_add_sources(cJSON *call, const runtime_catalog_source *listed, size_t count);

#endif
