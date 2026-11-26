#include "identity_naula.h"
#include "allocation.h"

#include <stdio.h>
#include <string.h>

#include "naula/memory.h"
#include "naula/session.h"
#include "thread.h"

#define GITHUB_WORD "github"
#define GITHUB_HOST "github.com"
#define ACCEPT_NUMBER_FIX "accept a number as <provider>[@host]:<account>=<number>, several joined by commas"
#define AT_FIX "record each place as <provider>[@host]:<number>, web:<domain> or a git url"

static runtime_mutex registry_lock = RUNTIME_MUTEX_INIT;
static naula_providers *registry;

static naula_status open_registry_locked(void) {
    return registry != NULL ? NAULA_OK : naula_providers_open(&registry);
}

naula_status runtime_identity_providers(naula_providers **providers) {
    runtime_mutex_lock(&registry_lock);
    naula_status status = open_registry_locked();
    *providers = registry;
    runtime_mutex_unlock(&registry_lock);
    return status;
}

naula_status runtime_identity_register_provider(const naula_provider *provider, naula_refusal *refusal) {
    runtime_mutex_lock(&registry_lock);
    naula_status status = open_registry_locked();
    if (status == NAULA_OK) {
        status = naula_providers_register(registry, provider, refusal);
    }
    runtime_mutex_unlock(&registry_lock);
    return status;
}

int runtime_identity_place_text(const naula_place *place, char *text, size_t size) {
    int written;
    if (place->kind == NAULA_HOME_PROVIDER && place->host != NULL) {
        written = snprintf(text, size, "%s@%s:%llu", place->provider, place->host, (unsigned long long)place->number);
    } else if (place->kind == NAULA_HOME_PROVIDER) {
        written = snprintf(text, size, "%s:%llu", place->provider, (unsigned long long)place->number);
    } else if (place->kind == NAULA_HOME_WEB) {
        written = snprintf(text, size, "web:%s", place->domain);
    } else {
        written = snprintf(text, size, "%s", place->url == NULL ? "" : place->url);
    }
    return written >= 0 && (size_t)written < size;
}

static void account_text(const naula_home *home, char *text, size_t size) {
    if (home->host != NULL) {
        snprintf(text, size, "%s@%s:%s", home->provider, home->host, home->account);
    } else {
        snprintf(text, size, "%s:%s", home->provider, home->account);
    }
}

static void number_text(const naula_home *home, uint64_t number, char *text, size_t size) {
    if (home->host != NULL) {
        snprintf(text, size, "%s@%s:%llu", home->provider, home->host, (unsigned long long)number);
    } else {
        snprintf(text, size, "%s:%llu", home->provider, (unsigned long long)number);
    }
}

void runtime_identity_refusal_text(const naula_refusal *refusal, char *text, size_t size) {
    if (refusal->code[0] == '\0') {
        snprintf(text, size, "%s", refusal->detail[0] == '\0' ? "the repo could not be resolved" : refusal->detail);
    } else {
        snprintf(text, size, "%s (%s)", refusal->detail, refusal->code);
    }
}

const char *runtime_identity_refusal_fix(const naula_refusal *refusal) {
    return refusal->fix == NULL ? RUNTIME_IDENTITY_FIX : refusal->fix;
}

int runtime_identity_is_token_text(const char *text) {
    if (text == NULL || text[0] == '\0' || strlen(text) > RUNTIME_TOKEN_LIMIT) {
        return 0;
    }
    for (const unsigned char *at = (const unsigned char *)text; *at != '\0'; at++) {
        if (*at <= 0x20 || *at >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

/** What the session's callbacks read and record: the request, the accepted primary as at writes it, the declined. */
typedef struct session_context {
    const runtime_identity_request *request;
    char accepted_primary[RUNTIME_PLACE_TEXT_SIZE];
    runtime_identity_acceptance *acceptance;
} session_context;

int runtime_identity_needs_the_user(naula_status status, const naula_refusal *refusal,
                                    const runtime_identity_acceptance *acceptance) {
    if (acceptance->primary_needed && status == NAULA_REFUSED && strcmp(refusal->code, "primary-changed") == 0) {
        return 1;
    }
    int refused_for_the_number = status == NAULA_REFUSED && refusal->fix != NULL &&
                                 strstr(refusal->fix, "accept_number") != NULL;
    return acceptance->dispute_count > 0 && (status == NAULA_OK || refused_for_the_number);
}

/** Whether `accepted`, a comma-separated list, holds `item` whole. */
static int lists(const char *accepted, const char *item) {
    size_t length = strlen(item);
    for (const char *at = accepted; at != NULL && *at != '\0'; at = strchr(at, ',') == NULL ? NULL : strchr(at, ',') + 1) {
        size_t span = strcspn(at, ",");
        if (span == length && strncmp(at, item, length) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether every item of a comma-separated list is `<account place>=<number>` with an account and digits. */
static int is_accept_number_list(const char *text) {
    for (const char *at = text;; at += strcspn(at, ",") + 1) {
        size_t span = strcspn(at, ",");
        const char *equals = memchr(at, '=', span);
        if (equals == NULL || equals == at || equals + 1 == at + span) {
            return 0;
        }
        for (const char *digit = equals + 1; digit < at + span; digit++) {
            if (*digit < '0' || *digit > '9') {
                return 0;
            }
        }
        if (at[span] == '\0') {
            return 1;
        }
    }
}

static int equals_ignoring_case(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        char a = *left >= 'A' && *left <= 'Z' ? (char)(*left - 'A' + 'a') : *left;
        char b = *right >= 'A' && *right <= 'Z' ? (char)(*right - 'A' + 'a') : *right;
        if (a != b) {
            return 0;
        }
    }
    return *left == *right;
}

static const char *token_for(void *context, const char *provider, const char *host) {
    const session_context *session = context;
    const runtime_identity_request *request = session->request;
    const char *named = host != NULL ? host : strcmp(provider, GITHUB_WORD) == 0 ? GITHUB_HOST : NULL;
    if (request->token == NULL || request->token_host == NULL || named == NULL) {
        return NULL;
    }
    return equals_ignoring_case(named, request->token_host) ? request->token : NULL;
}

static int accept_primary(void *context, const char *id, const naula_place *was, const naula_place *becomes) {
    session_context *session = context;
    char offered[RUNTIME_PLACE_TEXT_SIZE];
    (void)id;
    if (!runtime_identity_place_text(becomes, offered, sizeof offered)) {
        return 0;
    }
    if (session->accepted_primary[0] != '\0' && strcmp(offered, session->accepted_primary) == 0) {
        return 1;
    }
    session->acceptance->primary_needed = 1;
    snprintf(session->acceptance->offered_primary, sizeof session->acceptance->offered_primary, "%s", offered);
    if (was == NULL ||
        !runtime_identity_place_text(was, session->acceptance->recorded_primary,
                                     sizeof session->acceptance->recorded_primary)) {
        session->acceptance->recorded_primary[0] = '\0';
    }
    return 0;
}

static int is_kept(const runtime_identity_acceptance *acceptance, const char *accept, const char *recorded) {
    for (size_t index = 0; index < acceptance->dispute_count; index++) {
        if (strcmp(acceptance->disputes[index].accept, accept) == 0 &&
            strcmp(acceptance->disputes[index].recorded_number, recorded) == 0) {
            return 1;
        }
    }
    return 0;
}

static int accept_number(void *context, const char *id, const naula_home *account, uint64_t was, uint64_t becomes) {
    session_context *session = context;
    runtime_identity_disputed dispute;
    (void)id;
    account_text(account, dispute.account, sizeof dispute.account);
    snprintf(dispute.accept, sizeof dispute.accept, "%s=%llu", dispute.account, (unsigned long long)becomes);
    if (session->request->accept_number != NULL && lists(session->request->accept_number, dispute.accept)) {
        return 1;
    }
    number_text(account, was, dispute.recorded_number, sizeof dispute.recorded_number);
    number_text(account, becomes, dispute.offered_number, sizeof dispute.offered_number);
    runtime_identity_acceptance *acceptance = session->acceptance;
    if (!is_kept(acceptance, dispute.accept, dispute.recorded_number) && acceptance->dispute_count < RUNTIME_DISPUTE_LIMIT) {
        acceptance->disputes[acceptance->dispute_count++] = dispute;
    }
    return 0;
}

static naula_status refuse_malformed(naula_refusal *refusal, const char *code, const char *fix, const char *what,
                                     const char *text) {
    naula_refuse(refusal, code, fix, "%s %s is not usable", what, text);
    return NAULA_MALFORMED;
}

static void start_place(naula_place *place) {
    memset(place, 0, sizeof *place);
    place->size = sizeof *place;
}

/** Writes the accepted place text as at writes it, normalised by naula's reading; empty for none. */
static naula_status normalise_accepted(const char *text, const char *what, char *normalised, size_t size,
                                       naula_refusal *refusal) {
    naula_place place;
    normalised[0] = '\0';
    if (text == NULL) {
        return NAULA_OK;
    }
    start_place(&place);
    naula_status status = naula_place_parse(text, &place, NULL);
    if (status == NAULA_OK && !runtime_identity_place_text(&place, normalised, size)) {
        status = NAULA_MALFORMED;
    }
    naula_place_release(&place);
    if (status == NAULA_OUT_OF_MEMORY) {
        return status;
    }
    return status == NAULA_OK ? NAULA_OK : refuse_malformed(refusal, "invalid_accepted", AT_FIX, what, text);
}

static void release_places(naula_place *places, size_t count) {
    for (size_t index = 0; index < count; index++) {
        naula_place_release(&places[index]);
    }
    naula_memory_release(places);
}

/** The places of `at`, read as naula reads them; NULL with the status saying why. */
static naula_place *places_of(const runtime_identity_request *request, naula_status *status, naula_refusal *refusal) {
    if (request->at == NULL || request->at_count == 0) {
        *status = refuse_malformed(refusal, "no_at", AT_FIX, "the reference", "with no at");
        return NULL;
    }
    naula_place *places = naula_memory_allocate(request->at_count * sizeof *places);
    *status = places == NULL ? NAULA_OUT_OF_MEMORY : NAULA_OK;
    for (size_t index = 0; places != NULL && index < request->at_count; index++) {
        start_place(&places[index]);
    }
    for (size_t index = 0; *status == NAULA_OK && index < request->at_count; index++) {
        const char *text = request->at[index];
        *status = text == NULL ? NAULA_MALFORMED : naula_place_parse(text, &places[index], NULL);
        if (*status != NAULA_OK && *status != NAULA_OUT_OF_MEMORY) {
            *status = refuse_malformed(refusal, "invalid_at", AT_FIX, "the place", text == NULL ? "(none)" : text);
        }
    }
    if (*status != NAULA_OK && places != NULL) {
        release_places(places, request->at_count);
        places = NULL;
    }
    return places;
}

static naula_status open_session(session_context *context, naula_session **session, naula_refusal *refusal) {
    naula_providers *providers = NULL;
    naula_status status = runtime_identity_providers(&providers);
    if (status != NAULA_OK) {
        return status;
    }
    naula_session_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.cache_directory = context->request->cache;
    options.providers = providers;
    options.token_for = token_for;
    options.accept_primary = accept_primary;
    options.accept_number = accept_number;
    options.context = context;
    status = naula_session_open(&options, session, refusal);
    return status == NAULA_REFUSED ? NAULA_MALFORMED : status;
}

naula_status runtime_identity_resolve(const runtime_identity_request *request, naula_resolution *resolution,
                                      runtime_identity_acceptance *acceptance, naula_refusal *refusal) {
    session_context context;
    memset(acceptance, 0, sizeof *acceptance);
    memset(&context, 0, sizeof context);
    context.request = request;
    context.acceptance = acceptance;
    naula_status status = normalise_accepted(request->accept_primary, "the accepted primary", context.accepted_primary,
                                             sizeof context.accepted_primary, refusal);
    if (status == NAULA_OK && request->accept_number != NULL && !is_accept_number_list(request->accept_number)) {
        status = refuse_malformed(refusal, "invalid_accepted", ACCEPT_NUMBER_FIX, "the accepted number",
                                  request->accept_number);
    }
    naula_place *places = status == NAULA_OK ? places_of(request, &status, refusal) : NULL;
    naula_session *session = NULL;
    if (status == NAULA_OK) {
        status = open_session(&context, &session, refusal);
    }
    if (status == NAULA_OK) {
        naula_reference reference;
        memset(&reference, 0, sizeof reference);
        reference.size = sizeof reference;
        reference.id = (char *)request->id;
        reference.at = places;
        reference.at_count = request->at_count;
        status = naula_resolve(session, &reference, resolution, refusal);
    }
    naula_session_close(session);
    if (places != NULL) {
        release_places(places, request->at_count);
    }
    return status;
}
