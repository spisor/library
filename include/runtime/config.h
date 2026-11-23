#ifndef RUNTIME_PUBLIC_CONFIG_H
#define RUNTIME_PUBLIC_CONFIG_H

/**
 * @file
 * runtime's `config` service: one plugin's settings, its config defaults merged under the portable place's
 * plugins/<plugin id>/config.json and that under the file its config name names in the home's config folder, each
 * object merged key by key and a file's value winning. A size-prefixed struct is read only up to its `size`. Text the
 * caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the
 * provider's until the release for its kind. Keys, layers and changes are as runtime/settings.h has them.
 *
 * A setting the plugin declares of type `secret` never reaches a file: it is kept by the secrets service, left out of
 * every answer of `all` and `get` below it, and a set may write it only by its own key.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One setting the plugin declares: its key, its type or NULL (`secret` keeps it out of every settings file), and the
 * runtime_settings_layer a set without a layer writes it to, RUNTIME_SETTINGS_LAYER_DEFAULT meaning the portable
 * layer. An element of an array, so it has no size.
 */
typedef struct runtime_setting_declaration {
    const char *key;
    const char *type;
    uint32_t layer;
} runtime_setting_declaration;

/**
 * The plugin whose settings an operation reads or writes. `home`, `names` and `portable` are as a
 * runtime_settings_scope has them; `plugin` is the manifest id; `config` the manifest's config, a record whose
 * `defaults` record and `name` text count, or NULL; `settings` holds `setting_count` declarations, each key once; and
 * `places` is where secret settings live, or NULL for a host with no secret store. First shape ends at `places`.
 */
typedef struct runtime_config_plugin {
    uint32_t size;
    const char *home;
    const spisor_value *names;
    const char *portable;
    const char *plugin;
    const spisor_value *config;
    const runtime_setting_declaration *settings;
    size_t setting_count;
    const runtime_secret_places *places;
} runtime_config_plugin;

/** The key a get reads. First shape ends at `key`. */
typedef struct runtime_config_get_argument {
    uint32_t size;
    const char *key;
} runtime_config_get_argument;

/**
 * What a set answers: for a secret setting `secret` 1 and no previous value, so a secret never leaves through it;
 * otherwise the merged value the key held before the write, of kind NONE when it held none. First shape ends at
 * `storage`.
 */
typedef struct runtime_config_set_answer {
    uint32_t size;
    uint32_t secret;
    spisor_value previous;
    void *storage;
} runtime_config_set_answer;

/**
 * The config service. Every operation answers MALFORMED for a home or portable place that is not absolute, no plugin,
 * `names` or `config` that is not a record, a declaration without a key or with a layer it does not know, a config
 * name or plugin id that is not one portable path segment, or places that are set but short of their first shape or
 * without an absolute secrets file, whether or not the key is a secret setting; and OUT_OF_MEMORY. A settings file
 * is read as the settings service reads one. First shape ends at `release_refusal`.
 */
typedef struct runtime_config_definition {
    uint32_t size;
    void *context;
    /**
     * Every setting, merged, without the declared secrets.
     *
     * @param values the consumer's empty tree, filled with a record. Required.
     */
    spisor_status (*all)(void *context, const runtime_config_plugin *plugin, spisor_value_tree *values,
                         spisor_refusal *refusal);
    /**
     * One setting: a declared secret from the secrets service, any other from the merged settings without the
     * declared secrets below it.
     *
     * @param value the consumer's empty tree. Required.
     * @return OK; NONE when nothing holds the key, the key has an empty segment, or it lies inside a declared secret;
     * MALFORMED for no key; REFUSED as the secrets service refuses. A secret's text is overwritten when its tree is
     * released.
     */
    spisor_status (*get)(void *context, const runtime_config_plugin *plugin, const runtime_config_get_argument *argument,
                         spisor_value_tree *value, spisor_refusal *refusal);
    /**
     * Writes one setting, as the settings service's set writes one, or a declared secret through the secrets service:
     * text stores it and null removes it.
     *
     * @param answer the consumer's empty answer. Required.
     * @return OK; MALFORMED as the settings service's set; REFUSED as that set, for a value that would write into a
     * declared secret below its key, a secret setting whose value is neither text nor null or whose host has no secret
     * store, or as the secrets service refuses.
     */
    spisor_status (*set)(void *context, const runtime_config_plugin *plugin, const runtime_settings_change *change,
                         runtime_config_set_answer *answer, spisor_refusal *refusal);
    void (*release_tree)(void *context, spisor_value_tree *tree);
    void (*release_set_answer)(void *context, runtime_config_set_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_config_definition;

/** The static runtime_config_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_config_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
