#ifndef RUNTIME_PUBLIC_INSTALL_H
#define RUNTIME_PUBLIC_INSTALL_H

/**
 * @file
 * runtime's `install` service: what a plugin's manifest depends on by name, the packages an item's install methods
 * name, the steps that install it through one method, and what each built-in method finds already installed. A
 * manifest and its `packages` record cross as spisor_value, their shapes being the manifest author's. A size-prefixed
 * struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what
 * an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A plugin's manifest, the plugin.json record. First shape ends at `manifest`. */
typedef struct runtime_install_warnings_argument {
    uint32_t size;
    const spisor_value *manifest;
} runtime_install_warnings_argument;

/** What a warning is about. Append-only: a published value keeps its number. */
typedef enum runtime_install_warning_kind {
    RUNTIME_INSTALL_NAMED_DEPENDENCY = 1,
    RUNTIME_INSTALL_UNBOUND_SERVICE = 2
} runtime_install_warning_kind;

/**
 * A runtime_install_warning_kind, its subject (the dependency's name, or the service's id) and one sentence saying
 * why it is a risk. An element of an array, so it has no size.
 */
typedef struct runtime_install_warning {
    uint32_t kind;
    const char *subject;
    const char *detail;
} runtime_install_warning;

/** The warnings in the manifest's order. First shape ends at `storage`. */
typedef struct runtime_install_warnings_answer {
    uint32_t size;
    const runtime_install_warning *warnings;
    size_t warning_count;
    void *storage;
} runtime_install_warnings_answer;

/**
 * An item's id and the absolute `portable` place whose packages.json the user maps packages in, and
 * `manifest_packages`, the item's own record of a package per method (a package name, or a record of `name` and
 * `options`), or NULL for none. First shape ends at `manifest_packages`.
 */
typedef struct runtime_install_packages_argument {
    uint32_t size;
    const char *portable;
    const char *item;
    const spisor_value *manifest_packages;
} runtime_install_packages_argument;

/**
 * A package an install method installs: the method, the package's name, and its `options`, data whose shape belongs
 * to that method, or NULL. An element of an array, so it has no size.
 */
typedef struct runtime_install_package {
    const char *method;
    const char *name;
    const spisor_value *options;
} runtime_install_package;

/** One package per method, the manifest's methods first. First shape ends at `storage`. */
typedef struct runtime_install_packages_answer {
    uint32_t size;
    const runtime_install_package *packages;
    size_t package_count;
    void *storage;
} runtime_install_packages_answer;

/**
 * The item as for packages, the `methods` it lists, the method `chosen`, the methods whose providers are installed,
 * and `catalog`, a list of the catalog entries that may provide a method, earlier ones winning, or NULL for none.
 * First shape ends at `catalog`.
 */
typedef struct runtime_install_plan_argument {
    uint32_t size;
    const char *portable;
    const char *item;
    const spisor_value *manifest_packages;
    const char *const *methods;
    size_t method_count;
    const char *chosen;
    const char *const *installed_providers;
    size_t installed_provider_count;
    const spisor_value *catalog;
} runtime_install_plan_argument;

/** What a step does. Append-only: a published value keeps its number. */
typedef enum runtime_install_step_kind {
    RUNTIME_INSTALL_PROVIDER_STEP = 1,
    RUNTIME_INSTALL_PACKAGE_STEP = 2
} runtime_install_step_kind;

/**
 * A runtime_install_step_kind: a provider step installs the catalog entry `provider`; a package step installs
 * `package` through its method, its name NULL for the marketplace method, which needs none. An element of an array,
 * so it has no size.
 */
typedef struct runtime_install_step {
    uint32_t kind;
    const spisor_value *provider;
    runtime_install_package package;
} runtime_install_step;

/** The steps in the order a host carries them out. First shape ends at `storage`. */
typedef struct runtime_install_plan_answer {
    uint32_t size;
    const runtime_install_step *steps;
    size_t step_count;
    void *storage;
} runtime_install_plan_answer;

/**
 * The item as for packages, and the absolute `cache` place a method keeps what it reads under. First shape ends at
 * `manifest_packages`.
 */
typedef struct runtime_install_detect_argument {
    uint32_t size;
    const char *portable;
    const char *cache;
    const char *item;
    const spisor_value *manifest_packages;
} runtime_install_detect_argument;

/**
 * What one method found of the item: the package it looked for and whether it is installed, at which `version` or
 * NULL when the method cannot say; or, when the method refused to look, `refused` saying why and the package naming
 * only its method.
 * An element of an array, so it has no size.
 */
typedef struct runtime_install_detection {
    const char *item;
    runtime_install_package package;
    uint32_t installed;
    const char *version;
    const char *refused;
} runtime_install_detection;

/** A detection per built-in method the item's packages name, in the methods' order. First shape ends at `storage`. */
typedef struct runtime_install_detect_answer {
    uint32_t size;
    const runtime_install_detection *detections;
    size_t detection_count;
    void *storage;
} runtime_install_detect_answer;

/**
 * The install service. Every operation answers MALFORMED for an argument or answer short of its first shape, a
 * required member that is NULL or empty, a place that is not absolute, a count without its array, a text in an array
 * that is NULL, or a value that is not a well-formed tree of the kind named; and OUT_OF_MEMORY. None writes anything.
 * The answer arguments are required and the refusal may be NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_install_definition {
    uint32_t size;
    void *context;
    /**
     * The manifest's dependencies by a name instead of an id, and the services it consumes by a namespaced id that
     * names no provider: what a rename can break, or whoever takes that name can replace.
     *
     * @return OK; MALFORMED, also for a manifest that is not a record; OUT_OF_MEMORY.
     */
    spisor_status (*warnings)(void *context, const runtime_install_warnings_argument *argument,
                              runtime_install_warnings_answer *answer, spisor_refusal *refusal);
    /**
     * The manifest's packages, then the item's entry in packages.json, whose package for a method replaces the
     * manifest's for that method only. An absent file adds nothing.
     *
     * @return OK; MALFORMED, also for manifest packages that are not a record; REFUSED for a package of another
     * shape, or a packages.json that is unreadable, not JSON or not shaped by item and method, the refusal naming
     * it; OUT_OF_MEMORY.
     */
    spisor_status (*packages)(void *context, const runtime_install_packages_argument *argument,
                              runtime_install_packages_answer *answer, spisor_refusal *refusal);
    /**
     * The steps that install the item through the chosen method. The marketplace method is built into every host
     * and always plans one package step with no package. Any other method must be listed, needs a package as
     * packages merges them, and a provider: when it is not installed, the first catalog entry whose installMethod
     * is the method is installed first.
     *
     * @return OK; MALFORMED, also for manifest packages that are not a record or a catalog that is not a list;
     * REFUSED for a method the item does not list, one with no package, or one no catalog entry provides, and as
     * packages refuses; OUT_OF_MEMORY.
     */
    spisor_status (*plan)(void *context, const runtime_install_plan_argument *argument,
                          runtime_install_plan_answer *answer, spisor_refusal *refusal);
    /**
     * What each built-in method the item's merged packages name finds installed, read from the method's own records:
     * it never launches a program or reaches the network.
     *
     * @return OK; MALFORMED, also for manifest packages that are not a record; REFUSED as packages refuses;
     * OUT_OF_MEMORY.
     */
    spisor_status (*detect)(void *context, const runtime_install_detect_argument *argument,
                            runtime_install_detect_answer *answer, spisor_refusal *refusal);
    void (*release_warnings_answer)(void *context, runtime_install_warnings_answer *answer);
    void (*release_packages_answer)(void *context, runtime_install_packages_answer *answer);
    void (*release_plan_answer)(void *context, runtime_install_plan_answer *answer);
    void (*release_detect_answer)(void *context, runtime_install_detect_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_install_definition;

/** The static runtime_install_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_install_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
