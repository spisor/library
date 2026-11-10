#ifndef RUNTIME_IDENTITY_H
#define RUNTIME_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/** One part of a parsed id: a view into the id's own text, empty when the id has no such part. */
typedef struct runtime_identity_part {
    const char *start;
    size_t length;
} runtime_identity_part;

/** The parts of a valid id. A local id has only a resolver, "local", and a path, its name and anything below it. */
typedef struct runtime_identity {
    runtime_identity_part resolver;
    runtime_identity_part host;
    runtime_identity_part number;
    runtime_identity_part path;
} runtime_identity;

/**
 * Whether id satisfies the id grammar, filling parsed from it when it does.
 *
 * @implNote Hand-written to `^(local:[^/\s]+|[a-z][a-z0-9-]*(@[a-z0-9.-]+)?:[0-9]+)(/(?!\.{1,2}(/|(?![\s\S])))[a-z0-9._-]+)*$`.
 * The whitespace a local name refuses is ASCII whitespace only; a byte above 0x7F is allowed, so a local name
 * holding a Unicode space is accepted here where the JavaScript engine, whose \s includes it, refuses it.
 */
int runtime_identity_parse_id(const char *id, runtime_identity *parsed);

typedef enum runtime_identity_folder_status {
    RUNTIME_IDENTITY_FOLDER_NAMED,
    RUNTIME_IDENTITY_FOLDER_INVALID,
    RUNTIME_IDENTITY_FOLDER_AMBIGUOUS,
    RUNTIME_IDENTITY_FOLDER_NOT_PORTABLE,
    RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY
} runtime_identity_folder_status;

/** The folder name of id as identity.folder answers it, freed with free; NULL unless NAMED, which is the only status with a folder. */
runtime_identity_folder_status runtime_identity_folder_name(const char *id, char **folder);

/**
 * The folder an item key lives in under the cache, freed with free: the key itself for a name, the id's folder for an
 * id. NULL with *unfit set when the key is an id that cannot name a folder, NULL with it clear when memory runs out.
 */
char *runtime_item_folder(const char *key, int *unfit);

/** The key a folder made by runtime_item_folder stands for, freed with free; NULL when memory runs out. */
char *runtime_item_key_of_folder(const char *folder);

/** identity.parse: {id} answers {valid: false} or {valid: true, resolver, host?, number?, path?}; number is a digit string. */
RUNTIME_EXPORT int32_t runtime_identity_parse(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * identity.folder: {id} answers {folder}, the id with ":" as "+" and "/" as "=", refusing an id that is invalid or
 * whose folder would not be a portable name (a local name may hold characters such as a backslash).
 */
RUNTIME_EXPORT int32_t runtime_identity_folder(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
