#ifndef RUNTIME_PUBLIC_CONFIG_H
#define RUNTIME_PUBLIC_CONFIG_H

/**
 * @file
 * A spike of runtime's `config` service as a definition: `set` only, beside the JSON exports. Size-prefixed structs
 * are read up to their `size`, their first shape ending at their last field. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or refusal points at is the provider's until the release
 * for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum runtime_settings_layer {
    /** The key's declared layer, or the application layer for a key no declaration names. */
    RUNTIME_LAYER_DECLARED = 0,
    RUNTIME_LAYER_APPLICATION = 1,
    RUNTIME_LAYER_PORTABLE = 2
} runtime_settings_layer;

/** One setting the plugin declares: its type (`secret` keeps it out of every settings file) and its layer. */
typedef struct runtime_setting_declaration {
    const char *key;
    const char *type;
    uint32_t layer;
} runtime_setting_declaration;

typedef struct runtime_secret_places {
    const char *secrets_file;
    uint32_t force_file_store;
} runtime_secret_places;

/**
 * `names` and `config` are the plugin's own data, so they cross as value trees (a record, or NULL for none);
 * `places` is NULL for a host with no secret store; `portable` NULL for a host with no portable layer.
 */
typedef struct runtime_config_set_argument {
    uint32_t size;
    const char *home;
    const char *portable;
    const char *plugin;
    const spisor_value *names;
    const spisor_value *config;
    const runtime_setting_declaration *settings;
    size_t setting_count;
    const runtime_secret_places *places;
    const char *key;
    const spisor_value *value;
    uint32_t layer;
} runtime_config_set_argument;

/** The merged value the key held before the write (kind NONE when it held none), or `secret` 1 for a secret. */
typedef struct runtime_config_set_answer {
    uint32_t size;
    uint32_t secret;
    spisor_value previous;
    void *storage;
} runtime_config_set_answer;

/** The config service. First shape ends at `release_refusal`. */
typedef struct runtime_config_definition {
    uint32_t size;
    void *context;
    spisor_status (*set)(void *context, const runtime_config_set_argument *argument, runtime_config_set_answer *answer,
                         spisor_refusal *refusal);
    void (*release_set)(void *context, runtime_config_set_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_config_definition;

#if defined(_WIN32) && defined(RUNTIME_LIBRARY_BUILDING)
#define RUNTIME_DEFINITION_API __declspec(dllexport)
#elif defined(_WIN32)
#define RUNTIME_DEFINITION_API __declspec(dllimport)
#else
#define RUNTIME_DEFINITION_API __attribute__((visibility("default")))
#endif

RUNTIME_DEFINITION_API const runtime_config_definition *runtime_config_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
