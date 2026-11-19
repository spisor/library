#ifndef RUNTIME_COMMANDS_H
#define RUNTIME_COMMANDS_H

#include "cJSON.h"
#include "spisor/status.h"

/**
 * Whether name is held on the PATH environment carries, the shim directory left out: on Windows
 * when <directory>/<name><extension> exists for an extension of PATHEXT (.COM;.EXE;.BAT;.CMD when
 * it is absent), or <directory>/<name> when name already ends in one; on POSIX when <directory>/<name>
 * is an executable file. Variable names are matched ignoring case on Windows, as the OS does.
 * Entries that are empty or not absolute are skipped, so nothing depends on the current directory.
 * 0 with *out_of_memory set to 1 when memory ran out looking.
 */
int runtime_commands_held_on_path(const cJSON *environment, const char *shims, const char *name, int *out_of_memory);

/** Which of the commands service's operations runtime_commands_serve serves. */
typedef enum runtime_command_operation {
    RUNTIME_COMMAND_REQUEST,
    RUNTIME_COMMAND_RENAME,
    RUNTIME_COMMAND_DELETE,
    RUNTIME_COMMAND_ADD
} runtime_command_operation;

/** The bytes a command name answered by runtime_commands_serve takes at most, its NUL included. */
#define RUNTIME_COMMAND_NAME_SIZE 80

/**
 * One commands call for host under the absolute state, with what its operation reads: environment, an object of
 * variable names to values, home, name, target and arguments, an array of texts, for request and add; environment,
 * from and to for rename; name for delete. What an operation does not read is NULL.
 */
typedef struct runtime_command_request {
    const char *state;
    const char *host;
    const cJSON *environment;
    const char *home;
    const char *name;
    const char *target;
    const cJSON *arguments;
    const char *from;
    const char *to;
} runtime_command_request;

/**
 * Serves one commands operation for request. OK with `name` the name the command has, empty when there is none (a
 * delete, or a request finding its entry deleted): the caller's buffer, so nothing is allocated once the operation has
 * written; MALFORMED or REFUSED with the refusal filled; OUT_OF_MEMORY. runtime/commands.h says what each operation
 * does.
 *
 * @implNote The whole call holds runtime_hosts_lock once, and the changes it records are applied
 * with runtime_changes_apply_held, so a request racing a rename from forebay can lose neither the
 * entry nor its shim.
 */
spisor_status runtime_commands_serve(runtime_command_operation operation, const runtime_command_request *request,
                                     char name[RUNTIME_COMMAND_NAME_SIZE], spisor_refusal *refusal);


#endif
