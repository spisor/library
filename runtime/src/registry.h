#ifndef RUNTIME_REGISTRY_H
#define RUNTIME_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

#define RUNTIME_REINSTALL_FIX "run spisor-host install --catalog <path or url> again"

typedef enum runtime_registry_answer {
    RUNTIME_REGISTRY_OK,
    RUNTIME_REGISTRY_ABSENT,
    RUNTIME_REGISTRY_UNREADABLE,
    RUNTIME_REGISTRY_INVALID,
    RUNTIME_REGISTRY_UNWRITABLE,
    RUNTIME_REGISTRY_OUT_OF_MEMORY
} runtime_registry_answer;

/**
 * Reads the registration of host id from state/hosts/id.json into *registration, freed with
 * cJSON_Delete. ABSENT when the host has none, UNREADABLE when the file exists but is not a JSON
 * object carrying that id, INVALID when id is not a portable segment.
 *
 * @implNote A registration that cannot be read is never treated as absent, so a caller that stores
 * only after ABSENT or OK never writes over a file it could not understand.
 */
runtime_registry_answer runtime_registry_load(const char *state, const char *id, cJSON **registration);

/**
 * Writes registration, a whole host record whose "id" names the file, to state/hosts/id.json
 * atomically, creating state/hosts when it is missing. The caller owns the read-modify-write
 * sequence: two writers of one host can lose an update, two writers of different hosts never
 * touch one file.
 */
runtime_registry_answer runtime_registry_store(const char *state, const cJSON *registration);

/**
 * Whether registration, host id's in state, can start the host: it carries a string version and a string
 * home. 0 with *refusal set naming id as not registered in state with the reinstall fix, or with *refusal
 * NULL when memory runs out; a NULL registration is refused the same way.
 */
int runtime_registration_usable(const cJSON *registration, const char *id, const char *state, cJSON **refusal);

/**
 * Host id's registration in state, freed with cJSON_Delete, once runtime_registration_usable accepts it;
 * NULL with *refusal set when it is not, or cannot be read, and with *refusal NULL when memory runs out.
 */
cJSON *runtime_usable_registration(const char *state, const char *id, cJSON **refusal);

RUNTIME_EXPORT int32_t runtime_registry_register(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
/**
 * registry.register, except that a provenance the host's stored registration already carries is kept in
 * place of the given one, decided under the registry lock, so two first starts of one host cannot both
 * record theirs. hosts.register calls it in-process; it is no function of the declaration.
 */
int32_t runtime_registry_register_first_provenance(const uint8_t *in, size_t in_len, uint8_t **out,
                                                   size_t *out_len);

RUNTIME_EXPORT int32_t runtime_registry_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
RUNTIME_EXPORT int32_t runtime_registry_list(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
RUNTIME_EXPORT int32_t runtime_registry_forget(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
/** registry.forget for a caller that already holds runtime_hosts_lock, which is not taken again. */
int32_t runtime_registry_forget_locked(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
