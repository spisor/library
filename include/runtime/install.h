#ifndef RUNTIME_PUBLIC_INSTALL_H
#define RUNTIME_PUBLIC_INSTALL_H

/**
 * @file
 * runtime's `install` service: what a plugin's manifest depends on by name, the packages an item's install methods
 * name, the steps that install it through one method, what each built-in method finds already installed, a
 * loader's application entry, the JavaScript runtime, an installed application's update or removal, and a loader's
 * registration. A manifest, its `packages` record, a loader's application and a method's options cross as
 * spisor_value, their shapes being their authors'. A size-prefixed
 * struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what
 * an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/environment.h"
#include "runtime/registry.h"
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
 * A loader `loader`, installed at the absolute `home`, and `application`, the record its host.json describes (an
 * `id`, a `label` and the `entry` to write), whose changes are recorded under the absolute `state` place, the
 * absolute `cache` place being where an entry finds what it runs. First shape ends at `application`.
 */
typedef struct runtime_install_entry_argument {
    uint32_t size;
    const char *state;
    const char *cache;
    const char *loader;
    const char *home;
    const spisor_value *application;
} runtime_install_entry_argument;

/**
 * Whether the entry was written, whether it runs its host on the JavaScript host, and the sequence of each change
 * recorded now; for an application that declares no entry, `detail` and `fix` say how to start it instead. First
 * shape ends at `storage`.
 */
typedef struct runtime_install_entry_answer {
    uint32_t size;
    uint32_t written;
    uint32_t runs_on_javascript_host;
    const uint64_t *sequences;
    size_t sequence_count;
    const char *detail;
    const char *fix;
    void *storage;
} runtime_install_entry_answer;

/**
 * The absolute `cache` place, the platform as os-arch such as linux-x64, or NULL for this machine's, and the
 * environment whose PATH a JavaScript runtime is looked for on. First shape ends at `environment_count`.
 */
typedef struct runtime_install_runtime_argument {
    uint32_t size;
    const char *cache;
    const char *platform;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_install_runtime_argument;

/** Where a JavaScript runtime came from. Append-only: a published value keeps its number. */
typedef enum runtime_install_javascript_source {
    RUNTIME_INSTALL_FROM_PATH = 1,
    RUNTIME_INSTALL_ACQUIRED = 2
} runtime_install_javascript_source;

/**
 * A JavaScript runtime: its name and version, its executable's path, whether it was fetched now, and a
 * runtime_install_javascript_source. Part of an answer, so it has no size.
 */
typedef struct runtime_install_javascript {
    const char *name;
    const char *version;
    const char *executable;
    uint32_t fetched;
    uint32_t source;
} runtime_install_javascript;

/** The JavaScript runtime every host started on JavaScript runs with. First shape ends at `storage`. */
typedef struct runtime_install_runtime_answer {
    uint32_t size;
    runtime_install_javascript javascript;
    void *storage;
} runtime_install_runtime_answer;

/** What to do to an installed application. Append-only: a published value keeps its number. */
typedef enum runtime_install_application_action {
    RUNTIME_INSTALL_UPDATE = 1,
    RUNTIME_INSTALL_REMOVE = 2
} runtime_install_application_action;

/**
 * The absolute places, the loader whose registered application to change, and a
 * runtime_install_application_action. First shape ends at `operation`.
 */
typedef struct runtime_install_application_argument {
    uint32_t size;
    const char *portable;
    const char *state;
    const char *cache;
    const char *loader;
    uint32_t operation;
} runtime_install_application_argument;

/**
 * The operation carried out, the version an update reports, or NULL; and when that version could not be recorded in
 * the loader's registration, why and how to record it, else NULL. First shape ends at `storage`.
 */
typedef struct runtime_install_application_answer {
    uint32_t size;
    uint32_t operation;
    const char *version;
    const char *unrecorded_detail;
    const char *unrecorded_fix;
    void *storage;
} runtime_install_application_answer;

/**
 * The absolute places, the environment the loader's commands are requested in, and the loader `id` at its installed
 * `version`. First shape ends at `version`.
 */
typedef struct runtime_install_register_loader_argument {
    uint32_t size;
    const char *portable;
    const char *state;
    const char *cache;
    const runtime_variable *environment;
    size_t environment_count;
    const char *id;
    const char *version;
} runtime_install_register_loader_argument;

/** A command request refused, its name, why and how to fix it. An element of an array, so it has no size. */
typedef struct runtime_install_refused_command {
    const char *name;
    const char *detail;
    const char *fix;
} runtime_install_refused_command;

/**
 * The provenance the loader's registration keeps, the commands it holds, and the command requests refused. First
 * shape ends at `storage`.
 */
typedef struct runtime_install_register_loader_answer {
    uint32_t size;
    const runtime_registry_provenance *provenance;
    const char *const *commands;
    size_t command_count;
    const runtime_install_refused_command *refused_commands;
    size_t refused_command_count;
    void *storage;
} runtime_install_register_loader_answer;

/**
 * The plugin to install by its repo `id`, such as github:5 or github:5/kernel, the absolute `cache` place it is
 * installed into and the absolute `plugin_directory` it is deployed into. `version` is the one to install, or NULL for
 * the release index's latest; `last_known` the owner/name a previous lookup saw, asked only when the number cannot be
 * looked up, or NULL; `accept_primary` the new primary the user accepted, or NULL; `api_base` the repo host's API
 * address, honoured only by a test build, or NULL. First shape ends at `api_base`.
 */
typedef struct runtime_install_by_id_argument {
    uint32_t size;
    const char *id;
    const char *cache;
    const char *plugin_directory;
    const char *version;
    const char *last_known;
    const char *accept_primary;
    const char *api_base;
} runtime_install_by_id_argument;

/**
 * The repo installed from, which is the verified primary and so can differ from the id asked for: its `id`, `name`
 * and `full_name`, the `ids` of its verified set, its `primary`, and whether a copy confirmed that primary. When the
 * primary differs from the one recorded for the id and the user has not accepted it, `needs_user_acceptance` is set,
 * `recorded_primary` names the recorded one and nothing was installed; otherwise `version`, `commit` (empty when the
 * index states none) and `sha256`, the release index's digest, say what was. First shape ends at `storage`.
 */
typedef struct runtime_install_by_id_answer {
    uint32_t size;
    const char *id;
    const char *name;
    const char *full_name;
    const char *const *ids;
    size_t id_count;
    const char *primary;
    uint32_t primary_confirmed;
    uint32_t needs_user_acceptance;
    const char *recorded_primary;
    const char *version;
    const char *commit;
    const char *sha256;
    void *storage;
} runtime_install_by_id_answer;

/**
 * The install service. Every operation answers MALFORMED for an argument or answer short of its first shape, a
 * required member that is NULL or empty, a place that is not absolute, a count without its array, a text in an array
 * that is NULL, a variable without a name or a value, or a value that is not a well-formed tree of the kind named;
 * and OUT_OF_MEMORY. Only entry, runtime, application, register_loader and by_id write, as each says. The answer arguments are required and the refusal may be NULL for none. First shape ends at `release_refusal`.
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
    /**
     * Writes the entry the application declares, every directory, file and settings member a change recorded under
     * the loader's id, so removing the loader reverses all of it. A change already on disk is not recorded again, so
     * writing an entry twice changes nothing. A refused call takes back the changes it recorded, as far as the disk
     * lets it; an OUT_OF_MEMORY answer may leave some recorded, for the loader's removal to reverse.
     *
     * @return OK; MALFORMED, also for an application that is not a record with an id; REFUSED for application facts
     * the loader gets wrong, or a change the disk refuses; OUT_OF_MEMORY.
     */
    spisor_status (*entry)(void *context, const runtime_install_entry_argument *argument,
                           runtime_install_entry_answer *answer, spisor_refusal *refusal);
    /**
     * A node on the environment's PATH whose major version is 20 or more, else the pinned Node for the platform,
     * fetched into the cache and verified against its pinned SHA-256 only when it is not there intact, and recorded as
     * the runtime every host started on JavaScript runs with. What it stored in the cache stays there for the next
     * call, whatever this one answers.
     *
     * @return OK; MALFORMED, also for a platform that is not os-arch; REFUSED when there is none to reuse and the pin
     * cannot be acquired; OUT_OF_MEMORY.
     */
    spisor_status (*runtime)(void *context, const runtime_install_runtime_argument *argument,
                             runtime_install_runtime_answer *answer, spisor_refusal *refusal);
    /**
     * Updates or removes the application the loader is registered for, through the method its registration
     * recorded, and after an update records the version the method reports. The loader stays registered either way.
     * An OUT_OF_MEMORY answer may follow an operation the method carried out.
     *
     * @return OK; MALFORMED, also for an operation of no known kind; REFUSED for an application found on the PATH or
     * one whose install is not recorded, naming where it came from, and as the method refuses; OUT_OF_MEMORY.
     */
    spisor_status (*application)(void *context, const runtime_install_application_argument *argument,
                                 runtime_install_application_answer *answer, spisor_refusal *refusal);
    /**
     * Registers the installed loader as an install registers it, its application read from that version's host.json,
     * and requests its commands again: what every start of a loader does, with no catalog read. It writes what hosts'
     * register writes, and a refusal or OUT_OF_MEMORY leaves what that leaves.
     *
     * @return OK; MALFORMED; REFUSED for a loader whose installed version cannot be read, and as hosts' register
     * refuses; OUT_OF_MEMORY.
     */
    spisor_status (*register_loader)(void *context, const runtime_install_register_loader_argument *argument,
                                     runtime_install_register_loader_answer *answer, spisor_refusal *refusal);
    /**
     * Installs the plugin the repo id names into the cache from its primary's latest release, checking every file
     * against the release index's digest, deploys it into the plugin directory under its import name, and records the
     * verified set and primary under the cache's identities for the id asked for. A new version comes only from a
     * confirmed primary, never a mirror; when the set cannot be read, or names a primary no copy confirms, the id asked
     * for is the primary, unconfirmed. A primary that differs from the recorded one installs nothing until
     * `accept_primary` names it. A REFUSED or OUT_OF_MEMORY answer may leave the version in the cache, where a later
     * install finds it, and, once deployed, the plugin, which the next install replaces; the record is written only
     * after the plugin is deployed.
     *
     * @return OK; MALFORMED, also for an id that is not an id at all; REFUSED for a local id, which has no host to install
     * from, a repo that cannot be looked up or was renamed during the install, copies naming different primaries, a
     * release without an index, a file failing its digest, or a deploy or record the disk refuses; OUT_OF_MEMORY.
     */
    spisor_status (*by_id)(void *context, const runtime_install_by_id_argument *argument,
                           runtime_install_by_id_answer *answer, spisor_refusal *refusal);
    void (*release_warnings_answer)(void *context, runtime_install_warnings_answer *answer);
    void (*release_packages_answer)(void *context, runtime_install_packages_answer *answer);
    void (*release_plan_answer)(void *context, runtime_install_plan_answer *answer);
    void (*release_detect_answer)(void *context, runtime_install_detect_answer *answer);
    void (*release_entry_answer)(void *context, runtime_install_entry_answer *answer);
    void (*release_runtime_answer)(void *context, runtime_install_runtime_answer *answer);
    void (*release_application_answer)(void *context, runtime_install_application_answer *answer);
    void (*release_register_loader_answer)(void *context, runtime_install_register_loader_answer *answer);
    void (*release_by_id_answer)(void *context, runtime_install_by_id_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_install_definition;

/** The static runtime_install_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_install_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
