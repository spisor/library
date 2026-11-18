#ifndef RUNTIME_SECRETS_H
#define RUNTIME_SECRETS_H

#include <stdint.h>

#include "runtime/secrets.h"
#include "spisor/status.h"

/** Whether places are present, reach through their first shape and name an absolute secrets file. */
int runtime_is_usable_secret_place(const runtime_secret_places *place);

/** The MALFORMED refusal every secrets operation answers for places runtime_is_usable_secret_place refuses. */
spisor_status runtime_refuse_secret_place(spisor_refusal *refusal);

/**
 * The secrets service's operations on a usable place, with the rules, statuses and refusals
 * include/runtime/secrets.h documents for them. A value get answers is freed.
 */
spisor_status runtime_get_secret(const runtime_secret_places *place, const char *plugin, const char *name, char **value,
                                 spisor_refusal *refusal);
spisor_status runtime_set_secret(const runtime_secret_places *place, const char *plugin, const char *name,
                                 const char *value, runtime_secret_location *location, spisor_refusal *refusal);
spisor_status runtime_delete_secret(const runtime_secret_places *place, const char *plugin, const char *name,
                                    spisor_refusal *refusal);
spisor_status runtime_clear_secrets(const runtime_secret_places *place, const char *plugin, spisor_refusal *refusal);
spisor_status runtime_reconcile_secrets(const runtime_secret_places *place, uint64_t *moved, spisor_refusal *refusal);

#endif
