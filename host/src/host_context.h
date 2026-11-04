#ifndef HOST_CONTEXT_H
#define HOST_CONTEXT_H

#include "cJSON.h"
#include "service_call.h"

/** What every command reads first: this process's environment and the places the runtime resolves from it. */
typedef struct host_context {
    cJSON *environment;
    cJSON *places;
    const char *portable;
    const char *state;
    const char *cache;
} host_context;

/**
 * Reads the environment and resolves the places; 1, or 0 with *refusal set (NULL when memory ran out).
 *
 * @implNote The places come from the environment, never from where this executable lies, because the
 * first install runs it from wherever the one command downloaded it.
 */
int host_open_context(host_context *context, cJSON **refusal);

void host_close_context(host_context *context);

/** This process's environment as an object of strings, every name once; NULL when memory runs out. */
cJSON *host_process_environment(void);

/** Calls the json function with argument, which it frees: the answer, or NULL with *refusal set. */
cJSON *host_call(runtime_json_function function, cJSON *argument, cJSON **refusal);

/** A new argument {places: {portable, state, cache}} for context; NULL when memory runs out. */
cJSON *host_places_argument(const host_context *context);

/** The {message, fix} refusal of the message made of parts and fix; NULL when memory runs out. */
cJSON *host_refusal_of(const char *const *parts, size_t count, const char *fix);

/** The string member name of object, or NULL. */
const char *host_text(const cJSON *object, const char *name);

/** The registration of host id, or NULL with *refusal set naming it as not registered. */
cJSON *host_registration(const host_context *context, const char *id, cJSON **refusal);

/** The ecosystem settings host id reads, its home's layer over the portable one: settings.all's values. */
cJSON *host_settings_of(const host_context *context, const char *home, cJSON **refusal);

/** path made absolute against the current directory, freed with free; NULL when memory runs out. */
char *host_absolute_path(const char *path);

/**
 * Gives the owner of the file at path leave to run it; 1 when it may run. Nothing to do on Windows.
 *
 * @implNote A catalog lists no file modes, so the cache stores a native executable as a plain file, and
 * whatever starts one from the cache makes it runnable first.
 */
int host_make_runnable(const char *path);

/** This process's id. */
double host_process_id(void);

#endif
