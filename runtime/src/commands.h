#ifndef RUNTIME_COMMANDS_H
#define RUNTIME_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * Whether name is held on the PATH environment carries, the shim directory left out: on Windows
 * when <directory>/<name><extension> exists for an extension of PATHEXT (.COM;.EXE;.BAT;.CMD when
 * it is absent), or <directory>/<name> when name already ends in one; on POSIX when <directory>/<name>
 * is an executable file. Variable names are matched ignoring case on Windows, as the OS does.
 * Entries that are empty or not absolute are skipped, so nothing depends on the current directory.
 */
int runtime_commands_held_on_path(const cJSON *environment, const char *shims, const char *name);

/**
 * commands.request: { places: { state }, environment, host, home, name, target } answering
 * { name? }. The host gets the command it asks for, under the name it asks for unless that name is
 * held by an active command of any host, by an executable on the PATH of environment, or by
 * something already in state/bin, and then under <name>2, <name>3 and so on. An entry this host
 * already has for that requested name is answered as it stands, whatever the user did to it: a
 * deleted one answers no name. The shim is recorded as file changes running target with the
 * arguments and passing its exit code back: on POSIX the sh script state/bin/<name>, on Windows
 * state/bin/<name>.exe, a copy of runtime-launcher.exe, beside state/bin/<name>.shim holding the
 * target's UTF-8 path. Its first use also records state/bin as a directory and a path-entry in home,
 * with environment's SHELL. A target that moved rewrites the shim in place. A host only ever matches
 * entries it requested itself, so a command the user added is never taken over. Refused, like
 * rename and add, when environment is not an object or carries no PATH string.
 *
 * @implNote The whole call holds runtime_hosts_lock once, and the changes it records are applied
 * with runtime_changes_apply_held, so a request racing a rename from forebay can lose neither the
 * entry nor its shim.
 */
RUNTIME_EXPORT int32_t runtime_commands_request(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * commands.rename: { places: { state }, environment, host, from, to } answering { name }. Moves the
 * host's active command from to the name to, which must not be held (it is refused, never given a
 * fallback, because the user chose it), and marks the entry "owner": "user".
 */
RUNTIME_EXPORT int32_t runtime_commands_rename(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * commands.delete: { places: { state }, host, name } answering { deleted: true }. Removes the shim
 * and keeps the entry as "state": "deleted", "owner": "user", so the host's next request does not
 * bring it back. Refused for the last active command of a host with requiresCommand.
 */
RUNTIME_EXPORT int32_t runtime_commands_delete(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * commands.add: { places: { state }, environment, host, home, name, target } answering { name }.
 * Gives the host a command the user chose, "owner": "user" and "requested": null; a held name is
 * refused.
 */
RUNTIME_EXPORT int32_t runtime_commands_add(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
