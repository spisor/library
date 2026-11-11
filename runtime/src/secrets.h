#ifndef RUNTIME_SECRETS_H
#define RUNTIME_SECRETS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/** Where a plugin's secrets live, as places.resolve answers secretsFile and forceFileStore. */
typedef struct runtime_secret_place {
    const char *secrets_file;
    int force_file_store;
} runtime_secret_place;

/** Reads argument's "places" into place, borrowing its strings; 0 when it is absent or unusable. */
int runtime_secret_place_of(const cJSON *argument, runtime_secret_place *place);

/** The refusal every secrets function answers when its places are absent or unusable. */
int32_t runtime_refuse_secret_places(uint8_t **out, size_t *out_len);

/**
 * secrets.get, secrets.set and secrets.delete for one entry, with the same rules, refusals and
 * answers, taking their arguments as C values rather than a JSON call.
 */
int32_t runtime_get_secret(const runtime_secret_place *place, const char *plugin, const char *name, uint8_t **out,
                           size_t *out_len);
int32_t runtime_set_secret(const runtime_secret_place *place, const char *plugin, const char *name, const char *value,
                           uint8_t **out, size_t *out_len);
int32_t runtime_delete_secret(const runtime_secret_place *place, const char *plugin, const char *name, uint8_t **out,
                              size_t *out_len);

/**
 * A plugin's secrets, kept in the OS credential store or, when the store declines or is absent, in
 * the private secrets file; spec section 5.1. Every call takes {"places": {"secretsFile",
 * "forceFileStore"}} as places.resolve answers them, and an entry is named
 * "spisor/<plugin id>/<name>" in the store and in the file alike. get answers {"value"} from the
 * file, else from the store, or {} when neither holds it; it never writes to the store. When neither
 * holds it and the store declined or failed, get answers a failure, since the store may hold it.
 *
 * @implNote get reads the file first because a file entry is always the newer copy: one is written
 * only while the store refuses, and every later successful store write empties the file. Reading the
 * store first would answer a stale stored value, and a file-held secret never causes a store access.
 * Spec section 5.1 words the order the other way round.
 *
 * Unlike every other runtime service, these touch process-wide OS state: the user's
 * credential store, which may prompt (Keychain asking whether this program may use an item, a locked
 * keyring asking to be unlocked) and whose entries outlive the process. Nothing else is read from
 * the process, with one exception on Linux: the Secret Service backend reads DBUS_SESSION_BUS_ADDRESS
 * and XDG_RUNTIME_DIR to find the session bus, and the first use loads libsecret with glib, gobject and
 * gio into the host process, which starts GDBus's worker thread. The file's location arrives in the
 * argument. forceFileStore (SPISOR_SECRETS) skips
 * the store entirely, so not one store call is made.
 */
RUNTIME_EXPORT int32_t runtime_secrets_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * Stores {"plugin", "name", "value"} and answers {"place": "store" | "file"}. A value the store took
 * is followed by every file-held secret, one entry at a time and each written to the store before it
 * leaves the file, so an interruption never loses one; a store that refuses partway leaves the rest
 * in the file. A declined or absent store sends the value to the file, and the set still succeeds.
 * A value over RUNTIME_SECRET_VALUE_CAP_BYTES is refused before either place is touched. A secrets
 * file that is a symbolic link is never written through; SPISOR_SECRETS is how it moves.
 *
 * @implNote The file's own copy of this entry leaves the file before the move starts, as the move
 * would otherwise carry that older value over the one just stored. If it cannot leave, or the file
 * cannot be read to tell whether it holds one, the set is answered as a failure, since a later get
 * would answer that older copy and a later move would overwrite the stored value with it.
 */
RUNTIME_EXPORT int32_t runtime_secrets_set(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * Removes {"plugin", "name"} from both places. A store that declines or fails keeps its copy, and
 * then the file's copy is kept too, since it is the newer one: removing it alone would make the
 * store's older value readable again.
 */
RUNTIME_EXPORT int32_t runtime_secrets_delete(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/** Host only: removes every secret of {"plugin"} from both places, for uninstall. */
RUNTIME_EXPORT int32_t runtime_secrets_clear(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * Host only, called once at start: when the store is available and the file holds secrets, moves
 * them into the store, answering {"moved": n}. This is how a store installed since (Secret Service,
 * say) takes over the secrets that went to the file before it existed.
 */
RUNTIME_EXPORT int32_t runtime_secrets_reconcile(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
