#ifndef RUNTIME_PUBLIC_DETECT_H
#define RUNTIME_PUBLIC_DETECT_H

/**
 * @file
 * runtime's `detect` service: where a command name resolves on a PATH, found by reading directories only and never by
 * launching anything. A size-prefixed struct is read only up to its `size`. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the provider's until the
 * release for its kind.
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
 * A bare command name, such as "claude", and the environment whose PATH (and on Windows PATHEXT) it is looked up on.
 * First shape ends at `environment_count`.
 */
typedef struct runtime_detect_on_path_argument {
    uint32_t size;
    const char *binary;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_detect_on_path_argument;

/** The executable the name resolves to, an absolute path. First shape ends at `storage`. */
typedef struct runtime_detect_on_path_answer {
    uint32_t size;
    const char *path;
    void *storage;
} runtime_detect_on_path_answer;

/** The detect service. First shape ends at `release_refusal`. */
typedef struct runtime_detect_definition {
    uint32_t size;
    void *context;
    /**
     * Resolves the name under this OS's rules. On Windows PATH and PATHEXT are found ignoring the case of their names,
     * the name is tried with each PATHEXT extension in order (.COM;.EXE;.BAT;.CMD when PATHEXT is unset or empty) and
     * then bare, every name matched ignoring ASCII case; elsewhere the entry must be a regular file carrying an execute
     * bit.
     *
     * @param answer the consumer's empty answer. Required.
     * @param refusal the consumer's empty refusal, or NULL for none.
     * @return OK; NONE when nothing on the PATH answers to the name, the answer left empty; MALFORMED for no binary or
     * a variable without a name or a value; OUT_OF_MEMORY.
     */
    spisor_status (*on_path)(void *context, const runtime_detect_on_path_argument *argument,
                             runtime_detect_on_path_answer *answer, spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_detect_on_path_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_detect_definition;

/** The static runtime_detect_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_detect_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
