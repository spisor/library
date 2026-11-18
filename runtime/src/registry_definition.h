#ifndef RUNTIME_REGISTRY_DEFINITION_H
#define RUNTIME_REGISTRY_DEFINITION_H

#include "cJSON.h"
#include "runtime/registry.h"
#include "spisor/status.h"

/**
 * The registration as the record registry's operations take, `*json` NULL unless the answer is OK.
 *
 * @return OK; MALFORMED, the refusal filled, for a registration that is absent, short of its first shape, or holding a
 * field registry's register_host refuses; OUT_OF_MEMORY.
 */
spisor_status runtime_registration_record_json(const runtime_registration *registration, cJSON **json,
                                               spisor_refusal *refusal);

#endif
