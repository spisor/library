#ifndef RUNTIME_PUBLIC_COMMANDS_H
#define RUNTIME_PUBLIC_COMMANDS_H

/**
 * @file
 * runtime's `commands` service: the commands a registered host puts on the user's PATH, each a shim in the state
 * place's `bin` folder that runs the host's target, recorded as the host's changes so every one can be reversed. A
 * size-prefixed struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for
 * the call; what an answer or a refusal points at is the provider's until the release for its kind. A place is an
 * absolute directory, as places' `resolve` answers it, and a command name is at most 64 letters, digits, `-`, `_` and
 * `.`, starting with a letter or digit.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/environment.h"
#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A command for the host `host` recorded in the `state` place: `name` the name it asks for, `target` the absolute path
 * of the program the shim runs, and `arguments` the `argument_count` arguments the shim always passes before the
 * caller's, each without a line break. `home` is the user's home, absolute, where the shim folder's PATH entry goes,
 * and `environment` the user's environment, whose PATH says which names are taken and whose SHELL names the login
 * shell. First shape ends at `environment_count`.
 */
typedef struct runtime_command_argument {
    uint32_t size;
    const char *state;
    const char *host;
    const char *home;
    const char *name;
    const char *target;
    const char *const *arguments;
    size_t argument_count;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_command_argument;

/**
 * A move of the host's active command `from` to the name `to`, `environment` as for a request. First shape ends at
 * `environment_count`.
 */
typedef struct runtime_commands_rename_argument {
    uint32_t size;
    const char *state;
    const char *host;
    const char *from;
    const char *to;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_commands_rename_argument;

/** The host's active command `name`, to delete. First shape ends at `name`. */
typedef struct runtime_commands_delete_argument {
    uint32_t size;
    const char *state;
    const char *host;
    const char *name;
} runtime_commands_delete_argument;

/**
 * The name the command has, NULL when a request finds the host's entry for that name deleted. First shape ends at
 * `storage`.
 */
typedef struct runtime_command_answer {
    uint32_t size;
    const char *name;
    void *storage;
} runtime_command_answer;

/**
 * The commands service. Every operation holds the registry lock for its whole call, and answers MALFORMED for a place
 * that is not absolute, no host, a name that is not a command name, a target that is not absolute or holds a line
 * break, an argument with a line break, a home that is not absolute, or an environment without PATH or with a variable
 * lacking a name or a value; REFUSED, with the refusal saying what to repair, when the host is not registered, its
 * registration cannot be read or written, the registry lock cannot be taken, or a change cannot be made; and
 * OUT_OF_MEMORY. Each shim file and PATH entry is recorded as its own change, so an operation stopped part way leaves
 * every change it made recorded and reversible. A refused call takes those changes back before it answers, as far as
 * the disk lets it; an OUT_OF_MEMORY answer may leave some of them recorded, for the host's removal to reverse. A
 * rename that has stored the new name answers OK even when removing the old shim fails, whose record then stays.
 * The answer arguments are required and the refusal may be NULL for
 * none. First shape ends at `release_refusal`.
 *
 * @implNote `remove` and not `delete`, so the header stays includable from C++, where `delete` is a keyword.
 */
typedef struct runtime_commands_definition {
    uint32_t size;
    void *context;
    /**
     * Gives the host the command it asks for, under that name unless an active command of any host, an executable on
     * the PATH or something already in the shim folder holds it, and then under name2, name3 and so on. An entry the
     * host already has for the requested name is answered as it stands, whatever the user did to it, and a target or
     * arguments that changed rewrite its shim in place. A host only matches entries it requested itself, so a command
     * the user added is never taken over.
     *
     * @return OK; REFUSED as above, or when every fallback name is held.
     */
    spisor_status (*request)(void *context, const runtime_command_argument *argument, runtime_command_answer *answer,
                             spisor_refusal *refusal);
    /**
     * Moves the host's active command to a name the user chose, which is refused rather than given a fallback when it
     * is held, and makes the user its owner.
     *
     * @return OK; REFUSED as above, when the host has no active command `from`, or when `to` is held.
     */
    spisor_status (*rename)(void *context, const runtime_commands_rename_argument *argument,
                            runtime_command_answer *answer, spisor_refusal *refusal);
    /**
     * Removes the shim and keeps the entry as deleted and the user's, so the host's next request does not bring it
     * back.
     *
     * @return OK; REFUSED as above, when the host has no active command `name`, or for the last active command of a
     * host that requires one.
     */
    spisor_status (*remove)(void *context, const runtime_commands_delete_argument *argument, spisor_refusal *refusal);
    /**
     * Gives the host a command the user chose, owned by the user and requested by no host.
     *
     * @return OK; REFUSED as above, or when the name is held.
     */
    spisor_status (*add)(void *context, const runtime_command_argument *argument, runtime_command_answer *answer,
                         spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_command_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_commands_definition;

/** The static runtime_commands_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_commands_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
