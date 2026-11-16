#ifndef RUNTIME_PUBLIC_LISTS_H
#define RUNTIME_PUBLIC_LISTS_H

/**
 * @file
 * runtime's `lists` service: three-state lists of plugins and applications, installed, then detected, then
 * available, each group sorted by id and each id listed once, in the strongest group that holds it. A size-prefixed
 * struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what
 * an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/environment.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A row's group. Append-only: a published value keeps its number. */
typedef enum runtime_list_state {
    RUNTIME_LIST_INSTALLED = 1,
    RUNTIME_LIST_DETECTED = 2,
    RUNTIME_LIST_AVAILABLE = 3
} runtime_list_state;

/**
 * The plugins of each group, each a list of the caller's rows: records with a text member `id` and whatever else the
 * caller keeps in them, which the answer carries back. A detector that failed has not found its plugin. First shape
 * ends at `available`.
 */
typedef struct runtime_lists_plugins_argument {
    uint32_t size;
    const spisor_value *installed;
    const spisor_value *detected;
    const spisor_value *available;
} runtime_lists_plugins_argument;

/**
 * What an install method answered about one catalog item: `installed` 1 when it found its package installed, and
 * `refused` the method's refusal, or NULL when it answered. A refused method never detects. An element of an array, so
 * it has no size.
 */
typedef struct runtime_list_detection {
    const char *item;
    uint32_t installed;
    const char *refused;
} runtime_list_detection;

/**
 * The catalog's `items`, the record catalog's read answers, whose host items name their application; the `state`
 * place whose registered hosts are installed; and the environment whose PATH finds a detected application's binary,
 * read and never launched. First shape ends at `detection_count`.
 */
typedef struct runtime_lists_applications_argument {
    uint32_t size;
    const spisor_value *items;
    const char *state;
    const runtime_variable *environment;
    size_t environment_count;
    const runtime_list_detection *detections;
    size_t detection_count;
} runtime_lists_applications_argument;

/**
 * One row: its id, its runtime_list_state, the application's label or NULL, and for a plugin the caller's row as it
 * was first given, NULL for an application. An element of an array, so it has no size.
 */
typedef struct runtime_list_item {
    const char *id;
    uint32_t state;
    const char *label;
    const spisor_value *row;
} runtime_list_item;

/** The rows in order. First shape ends at `storage`. */
typedef struct runtime_list {
    uint32_t size;
    const runtime_list_item *items;
    size_t item_count;
    void *storage;
} runtime_list;

/**
 * The lists service. The answer arguments are required and the refusal may be NULL for none. First shape ends at
 * `release_refusal`.
 */
typedef struct runtime_lists_definition {
    uint32_t size;
    void *context;
    /**
     * The plugins of the three groups as one list.
     *
     * @return OK; MALFORMED for a group that is not a list of records each with a text id, or not a well-formed value;
     * OUT_OF_MEMORY.
     */
    spisor_status (*plugins)(void *context, const runtime_lists_plugins_argument *argument, runtime_list *answer,
                             spisor_refusal *refusal);
    /**
     * The application of every host item of the catalog: installed when a registered host carries its id, detected
     * when its binary is on the PATH or a detection says its package is installed, available otherwise.
     *
     * @return OK; MALFORMED for items that are not a record, a place that is not absolute, a detection without its
     * item, or a variable without a name or a value; REFUSED when the registry cannot be read; OUT_OF_MEMORY.
     */
    spisor_status (*applications)(void *context, const runtime_lists_applications_argument *argument,
                                  runtime_list *answer, spisor_refusal *refusal);
    void (*release_list)(void *context, runtime_list *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_lists_definition;

/** The static runtime_lists_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_lists_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
