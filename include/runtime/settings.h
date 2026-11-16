#ifndef RUNTIME_PUBLIC_SETTINGS_H
#define RUNTIME_PUBLIC_SETTINGS_H

/**
 * @file
 * runtime's `settings` service: the ecosystem's own settings, the portable place's settings.json merged under the one
 * in the home's config folder, each object merged key by key and a file's value winning. A size-prefixed struct is
 * read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer
 * or a refusal points at is the provider's until the release for its kind. A key is a dot path of one or more
 * non-empty segments, as `catalog.sources`.
 *
 * The values are data whose shape belongs to whoever stored them, so they cross as one spisor_value tree, answered in
 * a spisor_value_tree released by the definition's `release_tree`.
 */

#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Which settings file a set writes. Append-only: a published value keeps its number. */
typedef enum runtime_settings_layer {
    /** The key's declared layer, else the application layer: for a declaration, that it declares none. */
    RUNTIME_SETTINGS_LAYER_DEFAULT = 0,
    /** The file under the application's home. */
    RUNTIME_SETTINGS_LAYER_APPLICATION = 1,
    /** The file in the portable place. */
    RUNTIME_SETTINGS_LAYER_PORTABLE = 2
} runtime_settings_layer;

/**
 * Whose settings: `home` the application's home, absolute; `names` its `names` declaration, a record, or NULL; and
 * `portable` the portable place, absolute, or NULL for a host with no portable layer. First shape ends at `portable`.
 */
typedef struct runtime_settings_scope {
    uint32_t size;
    const char *home;
    const spisor_value *names;
    const char *portable;
} runtime_settings_scope;

/**
 * One write: `value` any value but one of kind NONE, read during the call only; `layer` a runtime_settings_layer.
 * First shape ends at `layer`.
 */
typedef struct runtime_settings_change {
    uint32_t size;
    const char *key;
    const spisor_value *value;
    uint32_t layer;
} runtime_settings_change;

/** The settings service. First shape ends at `release_refusal`. */
typedef struct runtime_settings_definition {
    uint32_t size;
    void *context;
    /**
     * Every setting, merged. A file that is absent, malformed, not an object, or holding a number no value tree
     * carries (as 1e999 parses) contributes nothing.
     *
     * @param values the consumer's empty tree, filled with a record. Required.
     * @return OK; MALFORMED for a home or portable place that is not absolute, or `names` that is not a record;
     * OUT_OF_MEMORY.
     */
    spisor_status (*all)(void *context, const runtime_settings_scope *scope, spisor_value_tree *values,
                         spisor_refusal *refusal);
    /**
     * Writes one key into its layer's file under that file's own lock, so two writers of different keys never lose
     * an update.
     *
     * @param previous the consumer's empty tree, filled with the merged value the key held before the write, or left
     * of kind NONE when it held none. Required.
     * @return OK; MALFORMED for an unusable scope, key, value or layer, or the portable layer on a host without one;
     * REFUSED when the file is not a readable JSON object (it is never discarded), the key crosses a value that is
     * not an object, or the file cannot be locked or written, a refused set having written nothing; OUT_OF_MEMORY.
     */
    spisor_status (*set)(void *context, const runtime_settings_scope *scope, const runtime_settings_change *change,
                         spisor_value_tree *previous, spisor_refusal *refusal);
    void (*release_tree)(void *context, spisor_value_tree *tree);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_settings_definition;

/** The static runtime_settings_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_settings_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
