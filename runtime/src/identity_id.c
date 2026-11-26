#include "identity.h"
#include "allocation.h"

#include <string.h>

#include "naula/id.h"
#include "segment.h"

/** Reads text as naula reads an id, `id` released by the caller when the answer is OK. */
static naula_status read_id(const char *text, naula_id *id) {
    memset(id, 0, sizeof *id);
    id->size = sizeof *id;
    if (text == NULL) {
        return NAULA_MALFORMED;
    }
    return naula_id_parse(text, strlen(text), id, NULL);
}

runtime_identity_kind runtime_identity_kind_of(const char *text) {
    naula_id id;
    naula_status status = read_id(text, &id);
    if (status == NAULA_OUT_OF_MEMORY) {
        return RUNTIME_IDENTITY_OUT_OF_MEMORY;
    }
    if (status != NAULA_OK) {
        return RUNTIME_IDENTITY_NOT_AN_ID;
    }
    runtime_identity_kind kind = id.kind == NAULA_ID_GIT ? RUNTIME_IDENTITY_GIT : RUNTIME_IDENTITY_LOCAL;
    naula_id_release(&id);
    return kind;
}

int runtime_identity_names_a_whole_repo(const char *id) {
    return strchr(id, '/') == NULL;
}

char *runtime_identity_repo_of(const char *id) {
    size_t length = strcspn(id, "/");
    char *repo = spisor_memory_allocate(length + 1);
    if (repo != NULL) {
        memcpy(repo, id, length);
        repo[length] = '\0';
    }
    return repo;
}

static char *folder_text(const char *id) {
    size_t length = strlen(id);
    char *folder = spisor_memory_allocate(length + 1);
    for (size_t index = 0; folder != NULL && index <= length; index++) {
        folder[index] = id[index] == ':' ? '+' : id[index] == '/' ? '=' : id[index];
    }
    return folder;
}

/** Whether a local name holds a character the folder mapping or a case-insensitive file system would make ambiguous. */
static int is_ambiguous_local_name(const naula_id *id) {
    for (const char *at = id->local_name; at != NULL && *at != '\0'; at++) {
        if (*at == '+' || *at == '=' || *at == ':' || (*at >= 'A' && *at <= 'Z')) {
            return 1;
        }
    }
    return 0;
}

runtime_identity_folder_status runtime_identity_folder_name(const char *id, char **folder) {
    naula_id parsed;
    *folder = NULL;
    naula_status status = read_id(id, &parsed);
    if (status == NAULA_OUT_OF_MEMORY) {
        return RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY;
    }
    if (status != NAULA_OK) {
        return RUNTIME_IDENTITY_FOLDER_INVALID;
    }
    int ambiguous = is_ambiguous_local_name(&parsed);
    naula_id_release(&parsed);
    if (ambiguous) {
        return RUNTIME_IDENTITY_FOLDER_AMBIGUOUS;
    }
    char *name = folder_text(id);
    if (name == NULL) {
        return RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY;
    }
    if (!runtime_is_portable_segment(name)) {
        spisor_memory_release(name);
        return RUNTIME_IDENTITY_FOLDER_NOT_PORTABLE;
    }
    *folder = name;
    return RUNTIME_IDENTITY_FOLDER_NAMED;
}

static char *copy_text(const char *text) {
    char *copy = spisor_memory_allocate(strlen(text) + 1);
    if (copy != NULL) {
        strcpy(copy, text);
    }
    return copy;
}

char *runtime_item_folder(const char *key, int *unfit) {
    *unfit = 0;
    runtime_identity_kind kind = runtime_identity_kind_of(key);
    if (kind == RUNTIME_IDENTITY_OUT_OF_MEMORY) {
        return NULL;
    }
    if (kind == RUNTIME_IDENTITY_NOT_AN_ID) {
        return copy_text(key);
    }
    char *folder = NULL;
    runtime_identity_folder_status status = runtime_identity_folder_name(key, &folder);
    *unfit = status != RUNTIME_IDENTITY_FOLDER_NAMED && status != RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY;
    return folder;
}

/** 1 when key is an id whose folder is folder, 0 when it is not, -1 when memory runs out. */
static int is_id_of_folder(const char *key, const char *folder) {
    runtime_identity_kind kind = runtime_identity_kind_of(key);
    if (kind == RUNTIME_IDENTITY_OUT_OF_MEMORY) {
        return -1;
    }
    if (kind == RUNTIME_IDENTITY_NOT_AN_ID) {
        return 0;
    }
    char *round_trip = NULL;
    runtime_identity_folder_status status = runtime_identity_folder_name(key, &round_trip);
    int same = status == RUNTIME_IDENTITY_FOLDER_NAMED && strcmp(round_trip, folder) == 0;
    spisor_memory_release(round_trip);
    return status == RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY ? -1 : same;
}

char *runtime_item_key_of_folder(const char *folder) {
    char *key = copy_text(folder);
    for (char *at = key; at != NULL && *at != '\0'; at++) {
        *at = *at == '+' ? ':' : *at == '=' ? '/' : *at;
    }
    int is_id = key == NULL ? 0 : is_id_of_folder(key, folder);
    if (is_id < 0) {
        spisor_memory_release(key);
        return NULL;
    }
    if (key != NULL && !is_id) {
        strcpy(key, folder);
    }
    return key;
}
