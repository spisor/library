#ifndef RUNTIME_PLUGIN_SCOPE_H
#define RUNTIME_PLUGIN_SCOPE_H

#include <weivel/services.h>

#include "config.h"
#include "runtime/secrets.h"
#include "scope_host.h"
#include "scope_manifest.h"
#include "spisor/status.h"

/** The plugin's directories its `paths` answers, each absolute and resolved once at the open. */
typedef struct runtime_scope_directories {
    char *cache;
    char *config;
    char *plugin;
} runtime_scope_directories;

/**
 * One plugin's scope, which the scope's `storage` holds: the definitions it answers, each with this scope as its
 * `context`, and what they read. Nothing in it but `references` changes after the open, so every operation reads it
 * from any thread.
 *
 * @implNote No answer a definition here fills points into the scope, and their releases read no context, so an answer
 * released after its scope is still freed whole. A log entry still waiting for the writer holds a reference, so the
 * scope it was written through lives until the entry is written, however soon the scope is released.
 */
typedef struct runtime_plugin_scope {
    weivel_standard_config config;
    weivel_standard_secrets secrets;
    weivel_standard_paths paths;
    weivel_standard_log log;
    runtime_owned_scope_host host;
    runtime_scope_manifest manifest;
    runtime_scope_directories directories;
    volatile long references;
} runtime_plugin_scope;

/**
 * Opens the scope of the plugin whose manifest is at `manifest_path`, in the started host, into `*scope`, holding one
 * reference.
 *
 * @return OK; REFUSED when no host is started, the manifest is refused (runtime_read_scope_manifest), or its config
 * name or id is no portable path segment; OUT_OF_MEMORY. `*scope` is NULL unless the answer is OK.
 */
spisor_status runtime_plugin_scope_open(const char *manifest_path, runtime_plugin_scope **scope,
                                        spisor_refusal *refusal);

/** Takes one more reference to an open scope, from any thread. */
void runtime_plugin_scope_hold(runtime_plugin_scope *scope);

/** Drops one reference, freeing the scope and everything it holds with the last; NULL is accepted and does nothing. */
void runtime_plugin_scope_release(runtime_plugin_scope *scope);

/**
 * The config request every operation of the scope starts from, pointing into the scope. It names no secret places,
 * so runtime's config answers a declared secret NONE and never reads one: secrets reach a plugin only through
 * `weivel.secrets`.
 */
runtime_config_request runtime_plugin_scope_request(const runtime_plugin_scope *scope);

/** The secret places of the host the scope opened in. */
runtime_secret_places runtime_plugin_scope_secret_places(const runtime_plugin_scope *scope);

/** The scope's `config`, served over runtime's config service. */
void runtime_scope_config_fill(runtime_plugin_scope *scope);

/** The scope's `secrets`, served over runtime's secrets service for the plugin's id alone. */
void runtime_scope_secrets_fill(runtime_plugin_scope *scope);

/**
 * Resolves the scope's directories. @return OK; REFUSED when the plugin's id is no portable path segment, so no folder
 * can carry it; OUT_OF_MEMORY. None is resolved unless the answer is OK.
 */
spisor_status runtime_scope_paths_resolve(runtime_plugin_scope *scope, spisor_refusal *refusal);

/** The scope's `paths`, answering the directories runtime_scope_paths_resolve resolved. */
void runtime_scope_paths_fill(runtime_plugin_scope *scope);

/** The scope's `log`, handing each entry to the log's writer (log_writer.h). */
void runtime_scope_log_fill(runtime_plugin_scope *scope);

#endif
