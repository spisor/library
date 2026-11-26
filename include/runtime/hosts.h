#ifndef RUNTIME_PUBLIC_HOSTS_H
#define RUNTIME_PUBLIC_HOSTS_H

/**
 * @file
 * runtime's `hosts` service: what every start of a host does to register it and request its commands, the hosts
 * registered, a host's removal, the provenance an application's registration records, the whole start of a host
 * that runs its plugins in a language layer, and how to start each layer a loader's plugins run in. A size-prefixed
 * struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an
 * answer or a refusal points at is the provider's until the release for its kind. A place is an absolute directory, as
 * places' `resolve` answers it, and a host id a plain file name.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/changes.h"
#include "runtime/definition.h"
#include "runtime/environment.h"
#include "runtime/install.h"
#include "runtime/item_version.h"
#include "runtime/places.h"
#include "runtime/registry.h"
#include "runtime/updates.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A command a host asks for: `name` the name it asks for, `target` the absolute path of the program its shim runs, and
 * `arguments` the `argument_count` arguments the shim always passes before the caller's. An element of an array, so it
 * has no size.
 */
typedef struct runtime_hosts_command {
    const char *name;
    const char *target;
    const char *const *arguments;
    size_t argument_count;
} runtime_hosts_command;

/**
 * The host `registration` names to register in the `state` place, its provenance the one its first start records, and
 * its `command_count` commands to request in the user's `environment`, whose HOME, or USERPROFILE on Windows, is where
 * the shims' PATH entry goes. `requires_command` of the registration decides whether a refused command refuses the
 * whole start. First shape ends at `environment_count`.
 */
typedef struct runtime_hosts_register_argument {
    uint32_t size;
    const char *state;
    const runtime_registration *registration;
    const runtime_hosts_command *commands;
    size_t command_count;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_hosts_register_argument;

/**
 * How many changes the removal reversed, those of them that had drifted, and whether a registration was there to
 * forget. First shape ends at `storage`.
 */
typedef struct runtime_hosts_remove_answer {
    uint32_t size;
    uint64_t reversed;
    const runtime_change_drift *drifted;
    size_t drifted_count;
    uint32_t forgotten;
    void *storage;
} runtime_hosts_remove_answer;

/**
 * Where an application came from, the absolute `home` it was installed into, its `binary` as found on the PATH of
 * `environment`, and `now` in milliseconds since the epoch, 0 for the current time. First shape ends at `now`.
 */
typedef struct runtime_hosts_provenance_argument {
    uint32_t size;
    const runtime_install_origin *origin;
    const char *binary;
    const char *home;
    const runtime_variable *environment;
    size_t environment_count;
    double now;
} runtime_hosts_provenance_argument;

/** The provenance the application's registration records. First shape ends at `storage`. */
typedef struct runtime_hosts_provenance_answer {
    uint32_t size;
    const runtime_registry_provenance *provenance;
    void *storage;
} runtime_hosts_provenance_answer;

/**
 * The start of `host_id` in the user's `environment`, its language `layer`'s bundle and the `library_count`
 * `libraries` that layer loaded, and `session` 1 for the application's session host, else 0; the `layer_runtime`,
 * `javascript` or `native`, whose plugins the start plans; and `action_plugin`, the plugin an ACTION start runs an
 * action of, NULL for any other start. First shape ends at `session`.
 */
typedef struct runtime_hosts_start_argument {
    uint32_t size;
    const runtime_variable *environment;
    size_t environment_count;
    const char *host_id;
    const runtime_item_version *layer;
    const runtime_item_version *libraries;
    size_t library_count;
    uint32_t session;
    const char *layer_runtime;
    const char *action_plugin;
} runtime_hosts_start_argument;

/**
 * The timers of a session host, `update_check_milliseconds` 0 for a layer that does not hold the front door, so one
 * layer of a session checks for updates. Part of an answer, so it has no size.
 */
typedef struct runtime_hosts_timers {
    double record_run_milliseconds;
    uint32_t prune_failures_before_exit;
    double update_check_milliseconds;
} runtime_hosts_timers;

/**
 * A host's start: the places the environment resolves to; the `version` the plan runs the host at, else the one it is
 * registered at, its `home`, `requires_command`, and `application` as its registration records them, NULL for none;
 * the ecosystem `settings` of that home; the `plan` updates' apply answers, which carries no next or rollback for a
 * reused recorded run; `reused_recorded_run` 1 when a start that is not the session start reused the run a live run
 * record names; the `timers` of the session start, NULL for any other start; and `no_color` 1 when the environment
 * carries NO_COLOR at all. `places` and `plan` live in this answer's storage, their own `storage` NULL. First shape
 * ends at `storage`.
 */
typedef struct runtime_hosts_start_answer {
    uint32_t size;
    const runtime_places_answer *places;
    const char *version;
    const char *home;
    uint32_t requires_command;
    const spisor_value *application;
    const spisor_value *settings;
    const runtime_updates_apply_answer *plan;
    uint32_t reused_recorded_run;
    const runtime_hosts_timers *timers;
    uint32_t no_color;
    void *storage;
} runtime_hosts_start_answer;

/** How a layer's start runs the loader's plugins: serving them, serving them in-process, or running one action. */
typedef enum runtime_hosts_layer_mode {
    RUNTIME_HOSTS_LAYER_SERVE = 1,
    RUNTIME_HOSTS_LAYER_IN_PROCESS = 2,
    RUNTIME_HOSTS_LAYER_ACTION = 3
} runtime_hosts_layer_mode;

/**
 * The starts of `loader_id`'s layers in the user's `environment`, whose places are the ones read, in `mode`, a
 * runtime_hosts_layer_mode; for an ACTION, the `plugin` whose `action` runs with `input`, a JSON object's text, all
 * three NULL for any other mode. First shape ends at `input`.
 */
typedef struct runtime_hosts_layer_start_argument {
    uint32_t size;
    const runtime_variable *environment;
    size_t environment_count;
    const char *loader_id;
    uint32_t mode;
    const char *plugin;
    const char *action;
    const char *input;
} runtime_hosts_layer_start_argument;

/**
 * How to start one layer: its `runtime`, `javascript` or `native`; the `host_id` its run publishes its ready record and
 * is held under, the loader id for the layer holding the front door and `<loader id>.<runtime>` for the other; and the
 * `executable` to run with its `argument_count` `arguments` and its `environment`. An element of an array, so it has no
 * size.
 */
typedef struct runtime_hosts_layer {
    const char *runtime;
    const char *host_id;
    const char *executable;
    const char *const *arguments;
    size_t argument_count;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_hosts_layer;

/**
 * The `layer_count` `layers` to start, in the order to start them, the front door's first. First shape ends at
 * `storage`.
 */
typedef struct runtime_hosts_layer_start_answer {
    uint32_t size;
    const runtime_hosts_layer *layers;
    size_t layer_count;
    void *storage;
} runtime_hosts_layer_start_answer;

/**
 * The hosts service. Every operation answers MALFORMED for an argument short of its first shape, a place that is not
 * absolute, a host id that is not a plain file name, a required text that is NULL or empty, an array whose count has
 * no elements or holds a NULL or empty text or an id twice, or an environment as runtime/environment.h refuses one;
 * such a call has written nothing. The one array whose texts may be empty is a command's `arguments`, since a program
 * may take an empty argument; it still refuses NULL. Memory running out never reads as a host that is not registered,
 * a command that was refused or a file that could not be deleted, and never removes, registers or forgets anything a
 * clean run would not. The answer arguments are required and the refusal may be NULL for none. First shape ends at
 * `release_refusal`.
 *
 * @implNote `register_host` and not `register`, which is a C keyword.
 */
typedef struct runtime_hosts_definition {
    uint32_t size;
    void *context;
    /**
     * Registers the host, keeping the provenance of its first start, then requests each command in order; the answer
     * names the commands the host holds and, for a host that does not require a command, the requests refused.
     *
     * @return OK; MALFORMED, also for a registration as registry's register_host refuses one; REFUSED when the
     * registration cannot be recorded, the environment has no user home while a command is asked for, which is
     * refused before anything is registered, or a command is refused for a host that requires one; OUT_OF_MEMORY. A
     * REFUSED or OUT_OF_MEMORY register may have recorded the registration and the commands requested before it
     * stopped, which a repeated register records alike.
     */
    spisor_status (*register_host)(void *context, const runtime_hosts_register_argument *argument,
                                   runtime_install_register_loader_answer *answer, spisor_refusal *refusal);
    /**
     * Every host registered, as registry's list answers it. It writes nothing.
     *
     * @return OK; MALFORMED; REFUSED when the hosts folder cannot be read; OUT_OF_MEMORY.
     */
    spisor_status (*list)(void *context, const runtime_registry_list_argument *argument,
                          runtime_registry_listing *answer, spisor_refusal *refusal);
    /**
     * Reverses every change the host recorded, then deletes its holders file, its references file and run records and
     * its boot hint, and forgets its registration last. The answer is built in full before the forget, which only sets
     * `forgotten`, so no answer but OK follows a forget.
     *
     * @return OK, also for a host that had no registration; MALFORMED; REFUSED when a reversal is refused, the registry
     * lock cannot be taken, or a file cannot be deleted, the registration then kept, and with it the record of what is
     * left to undo, for another attempt; OUT_OF_MEMORY, the registration kept. A REFUSED or OUT_OF_MEMORY removal may
     * have reversed changes and deleted files, which a repeated removal finds done.
     */
    spisor_status (*remove)(void *context, const runtime_registry_forget_argument *argument,
                            runtime_hosts_remove_answer *answer, spisor_refusal *refusal);
    /**
     * The provenance an application's registration records: the origin's method, package, options and version, the
     * binary on the environment's PATH, else home, as its location, this build's os and architecture as Node names
     * them, and `now` as its installed_at. It writes nothing.
     *
     * @return OK; MALFORMED, also for an origin with no method, options that are not a well-formed record, or a
     * time that is negative, not finite or past what a time can write; OUT_OF_MEMORY.
     */
    spisor_status (*provenance)(void *context, const runtime_hosts_provenance_argument *argument,
                                runtime_hosts_provenance_answer *answer, spisor_refusal *refusal);
    /**
     * The whole start of a host: resolves the places, reads the host's registration and its home's settings, then
     * plans what runs as updates' apply does, with the layer and each library at the version it runs; a start that is
     * not the session start of the layer holding the front door reuses instead the run a live run record of the host
     * names, with a run record of its own naming that run and what this start runs beside it, so one session applies
     * updates once. The plan holds only the enabled plugins whose catalog runtime is `layer_runtime`, and for an ACTION start only
     * `action_plugin` and every plugin of the layer that provides, transitively, a capability it consumes. A refused
     * session start publishes its refusal to the ready record of the run's identity, the host id for the layer that
     * holds the front door and `<host id>.<layer runtime>` for the other, once its places resolved.
     *
     * @return OK; MALFORMED, also for a `layer_runtime` that is neither `javascript` nor `native`; REFUSED when the
     * host is not registered or its registration carries no version or home, the settings cannot be read, a planned
     * plugin consumes a capability only a plugin of the other layer provides, an ACTION's plugin runs in the other
     * layer, the first plugin the loader requires runs on a runtime no layer runs, or as updates' apply refuses;
     * OUT_OF_MEMORY, removing the run record this start wrote, and publishing no refusal. A refused or OUT_OF_MEMORY start may have recorded the references, which the
     * next start would record alike.
     */
    spisor_status (*start)(void *context, const runtime_hosts_start_argument *argument,
                           runtime_hosts_start_answer *answer, spisor_refusal *refusal);
    void (*release_register_answer)(void *context, runtime_install_register_loader_answer *answer);
    void (*release_listing)(void *context, runtime_registry_listing *answer);
    void (*release_remove_answer)(void *context, runtime_hosts_remove_answer *answer);
    void (*release_provenance_answer)(void *context, runtime_hosts_provenance_answer *answer);
    void (*release_start_answer)(void *context, runtime_hosts_start_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
    /**
     * How to start the layers a loader's plugins run in, so no caller decides a layer, an argument order or an
     * identity: a SERVE start answers one layer per runtime of the enabled plugins a start resolves, the front door's
     * first, since only its session start applies updates and the other reuses the run it records; an IN_PROCESS start
     * the javascript layer alone, the one a host inside another application's process runs; an ACTION one layer, that
     * of the plugin's catalog runtime. A `native` layer runs the entry of `spisor-interface`'s
     * bundle as `host <runtime binary> native <loader id>`, a `javascript` one runs the JavaScript runtime with the
     * entry of `javascript-host`'s bundle and `<runtime binary> javascript <loader id>`, each at the version its boot
     * hint names and its runtime binary the one that hint names for `runtime`; `in-process`, or `action <plugin>
     * <action> <input>`, follows for those modes. Each start's environment is the argument's. It writes nothing but
     * the JavaScript runtime's record when it acquires one.
     *
     * @return OK, with no layer for a loader whose settings enable no plugin it can run; MALFORMED, also for a mode
     * that is none of the three, or an ACTION without its plugin, action and input or another mode with any of them;
     * REFUSED when the loader is not registered or its settings or catalog cannot be read, its first required plugin
     * runs on a runtime no layer runs, an IN_PROCESS start finds no javascript plugin, a layer's item has no
     * hinted version or runtime binary or is not installed intact, its bundle names no entry inside it or a native one
     * cannot be made runnable, or no JavaScript runtime can be found or acquired; OUT_OF_MEMORY.
     */
    spisor_status (*layer_start)(void *context, const runtime_hosts_layer_start_argument *argument,
                                 runtime_hosts_layer_start_answer *answer, spisor_refusal *refusal);
    void (*release_layer_start_answer)(void *context, runtime_hosts_layer_start_answer *answer);
} runtime_hosts_definition;

/** The static runtime_hosts_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_hosts_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
