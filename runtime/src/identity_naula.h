#ifndef RUNTIME_IDENTITY_NAULA_H
#define RUNTIME_IDENTITY_NAULA_H

#include <stddef.h>

#include "naula/provider.h"
#include "naula/resolve.h"

#define RUNTIME_PLACE_TEXT_SIZE 512
#define RUNTIME_TOKEN_LIMIT 512

#define RUNTIME_DISPUTE_LIMIT 8

/**
 * One resolve: the repo `id` and its `at` place texts; the absolute `cache` naula keeps confirmations in, or NULL;
 * a `token` sent only to `token_host`, or both NULL; the place the user accepted as the new primary, or NULL; and the
 * disputed numbers the user accepted, a comma-separated list of `<provider>[@host]:<account>=<number>`, or NULL.
 */
typedef struct runtime_identity_request {
    const char *id;
    const char *const *at;
    size_t at_count;
    const char *cache;
    const char *token;
    const char *token_host;
    const char *accept_primary;
    const char *accept_number;
} runtime_identity_request;

/**
 * An account whose number the records of a repo disagree on: the account as `<provider>[@host]:<account>`, the
 * place it was confirmed at, the place it answers now, and `accept`, the text accept_number takes to accept it.
 */
typedef struct runtime_identity_disputed {
    char account[RUNTIME_PLACE_TEXT_SIZE / 2];
    char recorded_number[RUNTIME_PLACE_TEXT_SIZE];
    char offered_number[RUNTIME_PLACE_TEXT_SIZE];
    char accept[RUNTIME_PLACE_TEXT_SIZE];
} runtime_identity_disputed;

/**
 * What naula asked the user during a resolve and the request did not accept: a new primary, with the one confirmed
 * before, and every disputed number, each once; naula asks about at most RUNTIME_DISPUTE_LIMIT accounts a run.
 */
typedef struct runtime_identity_acceptance {
    int primary_needed;
    char recorded_primary[RUNTIME_PLACE_TEXT_SIZE];
    char offered_primary[RUNTIME_PLACE_TEXT_SIZE];
    size_t dispute_count;
    runtime_identity_disputed disputes[RUNTIME_DISPUTE_LIMIT];
} runtime_identity_acceptance;

/**
 * Whether a resolve answered with `status` and `refusal` is the user's to settle: a declined primary, or a declined
 * number on an answer naula gave or refused only for want of that number. Any other refusal wins over a prompt.
 */
int runtime_identity_needs_the_user(naula_status status, const naula_refusal *refusal,
                                    const runtime_identity_acceptance *acceptance);

/**
 * Resolves the request through naula_resolve over the process's providers, reading history through each provider's
 * API, since the installer cannot assume git. OK fills `resolution`, released with naula_resolution_release, whose
 * `size` the caller sets; MALFORMED, with `refusal`, for no `at`, a place, accepted place or cache naula will not
 * take; OUT_OF_MEMORY; any other status is naula's refusal of the id. `acceptance` is filled whatever the answer: a
 * declined primary answers REFUSED `primary-changed`, a declined number may still answer OK.
 */
naula_status runtime_identity_resolve(const runtime_identity_request *request, naula_resolution *resolution,
                                      runtime_identity_acceptance *acceptance, naula_refusal *refusal);

/** The providers every resolve of this process reads: naula's built-in ones, opened on first use, and those added. */
naula_status runtime_identity_providers(naula_providers **providers);

/**
 * Registers a provider for a word no built-in provider takes, such as one a plugin provides: naula then resolves
 * homes of that word through it. `provider` and what it points at outlive the process's resolves.
 *
 * @implNote Registration is not safe against a resolve running on another thread; a provider is registered before
 * the first resolve that needs it.
 */
naula_status runtime_identity_register_provider(const naula_provider *provider, naula_refusal *refusal);

/** Writes the place as `at` writes it: `<provider>[@host]:<number>`, `web:<domain>` or the url; 0 when it overflows. */
int runtime_identity_place_text(const naula_place *place, char *text, size_t size);

#define RUNTIME_IDENTITY_FIX "check the id and the places recorded for it"

/** naula's refusal as one sentence, its detail then its code in parentheses, so a caller can tell codes apart. */
void runtime_identity_refusal_text(const naula_refusal *refusal, char *text, size_t size);

/** naula's fix, or RUNTIME_IDENTITY_FIX when it gave none. */
const char *runtime_identity_refusal_fix(const naula_refusal *refusal);

/** Whether text can be sent as an access token: one to 512 printable ASCII characters with no space. */
int runtime_identity_is_token_text(const char *text);

#endif
