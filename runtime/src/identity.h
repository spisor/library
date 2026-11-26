#ifndef RUNTIME_IDENTITY_H
#define RUNTIME_IDENTITY_H

#include <stddef.h>

typedef enum runtime_identity_kind {
    RUNTIME_IDENTITY_NOT_AN_ID,
    RUNTIME_IDENTITY_GIT,
    RUNTIME_IDENTITY_LOCAL,
    RUNTIME_IDENTITY_OUT_OF_MEMORY
} runtime_identity_kind;

/**
 * What text is as naula reads an id: `git:<40 or 64 lowercase hex>` or `local:<name>`, then any `/<segment>`.
 *
 * @implNote naula checks the whole grammar before it allocates, so OUT_OF_MEMORY says the text is an id whose kind
 * could not be told; a caller answers it as running out of memory, never as no id.
 */
runtime_identity_kind runtime_identity_kind_of(const char *text);

/** Whether text is an id with no folder below its repo. Only for a text runtime_identity_kind_of answered an id. */
int runtime_identity_names_a_whole_repo(const char *id);

/** The id's repo part, freed: the id up to its first folder. NULL when memory runs out. */
char *runtime_identity_repo_of(const char *id);

typedef enum runtime_identity_folder_status {
    RUNTIME_IDENTITY_FOLDER_NAMED,
    RUNTIME_IDENTITY_FOLDER_INVALID,
    RUNTIME_IDENTITY_FOLDER_AMBIGUOUS,
    RUNTIME_IDENTITY_FOLDER_NOT_PORTABLE,
    RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY
} runtime_identity_folder_status;

/** The folder name of id as identity.folder answers it, freed; NULL unless NAMED, which is the only status with a folder. */
runtime_identity_folder_status runtime_identity_folder_name(const char *id, char **folder);

/**
 * The folder an item key lives in under the cache, freed: the key itself for a name, the id's folder for an
 * id. NULL with *unfit set when the key is an id that cannot name a folder, NULL with it clear when memory runs out.
 */
char *runtime_item_folder(const char *key, int *unfit);

/** The key a folder made by runtime_item_folder stands for, freed; NULL when memory runs out. */
char *runtime_item_key_of_folder(const char *folder);

#endif
