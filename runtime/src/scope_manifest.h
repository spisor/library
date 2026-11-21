#ifndef RUNTIME_SCOPE_MANIFEST_H
#define RUNTIME_SCOPE_MANIFEST_H

#include "cJSON.h"
#include "spisor/status.h"

/**
 * What a scope reads of a plugin's manifest: the whole `manifest`, which the manifest's `id`, `config` and `settings`
 * point into, `config` and `settings` NULL when the manifest declares none. Freed with runtime_scope_manifest_free.
 */
typedef struct runtime_scope_manifest {
    cJSON *manifest;
    const char *id;
    const cJSON *config;
    const cJSON *settings;
} runtime_scope_manifest;

/**
 * Reads the manifest at `path` into an empty `manifest`.
 *
 * @return OK; REFUSED, with the refusal naming the manifest and what to fix, for a manifest that is absent,
 * unreadable or no JSON object, without an `id` text, whose `config` or `settings` is no record, or one of whose
 * settings is no record or declares a `layer` other than `application` or `portable`; OUT_OF_MEMORY. `manifest` holds
 * nothing unless the answer is OK.
 */
spisor_status runtime_read_scope_manifest(const char *path, runtime_scope_manifest *manifest,
                                          spisor_refusal *refusal);

/** Frees what a manifest holds and leaves it empty. */
void runtime_scope_manifest_free(runtime_scope_manifest *manifest);

#endif
