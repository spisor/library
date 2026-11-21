#ifndef RUNTIME_PLUGIN_SCOPE_H
#define RUNTIME_PLUGIN_SCOPE_H

#include <weivel/services.h>

#include "config.h"
#include "scope_host.h"
#include "scope_manifest.h"
#include "spisor/status.h"

/**
 * One plugin's scope, which the scope's `storage` owns: the definitions it answers, each with this scope as its
 * `context`, and what they read. Nothing in it changes after the open, so every operation reads it from any thread.
 *
 * @implNote No answer a definition here fills points into the scope, and their releases read no context, so an answer
 * released after its scope is still freed whole.
 */
typedef struct runtime_plugin_scope {
    weivel_standard_config config;
    runtime_owned_scope_host host;
    runtime_scope_manifest manifest;
} runtime_plugin_scope;

/**
 * Opens the scope of the plugin whose manifest is at `manifest_path`, in the started host, into `*scope`.
 *
 * @return OK; REFUSED when no host is started, the manifest is refused (runtime_read_scope_manifest), or its config
 * name or id is no portable path segment; OUT_OF_MEMORY. `*scope` is NULL unless the answer is OK.
 */
spisor_status runtime_plugin_scope_open(const char *manifest_path, runtime_plugin_scope **scope,
                                        spisor_refusal *refusal);

/** Frees a scope and everything it holds; NULL is accepted and does nothing. */
void runtime_plugin_scope_free(runtime_plugin_scope *scope);

/**
 * The config request every operation of the scope starts from, pointing into the scope. It names no secret places,
 * so runtime's config answers a declared secret NONE and never reads one: secrets reach a plugin only through
 * `weivel.secrets`.
 */
runtime_config_request runtime_plugin_scope_request(const runtime_plugin_scope *scope);

/** The scope's `config`, served over runtime's config service. */
void runtime_scope_config_fill(runtime_plugin_scope *scope);

#endif
