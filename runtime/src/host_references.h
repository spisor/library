#ifndef RUNTIME_HOST_REFERENCES_H
#define RUNTIME_HOST_REFERENCES_H

#include "cJSON.h"

#define RUNTIME_PLAIN_HOST_ID_FIX "give the host an id that is a plain file name"

/**
 * The bundle versions one host holds, each an id-to-version object: what it runs now, what it scheduled
 * for its next start, what it ran before a start switched it, kept until a later start, and each version
 * a rollback moved it away from, which is not scheduled again while it is still the catalog's latest.
 * rollback and rolled_back_from may be NULL, meaning none.
 *
 * @implNote rolled_back_from is written as rolledBackFrom, a member the sweep never reads: it keeps what
 * run, next and rollback name, and a version a rollback moved away from is not one of them.
 */
typedef struct runtime_host_references {
    cJSON *run;
    cJSON *next;
    cJSON *rollback;
    cJSON *rolled_back_from;
} runtime_host_references;

/**
 * Reads recorded, the parsed text of state/bundles/<host>.json, borrowing its members: 1 when it is an
 * object whose run and next are version maps, with *rollback the rollback map or NULL when it has none
 * that is one; 0 for any other shape, which names nothing.
 *
 * @implNote The sweep reads the file through this too, so the sweep and updates agree.
 */
int runtime_host_references_of(const cJSON *recorded, const cJSON **run, const cJSON **next, const cJSON **rollback);

/** Whether host_id is a plain file name, which no file recorded by its name can climb out of a folder with. */
int runtime_is_plain_host_id(const char *host_id);

/**
 * folder/<host_id>.json, freed with free; NULL with *problem set (freed with free) when host_id is no
 * plain file name and so could name a file outside folder, *problem naming what it would record there,
 * or with *problem NULL when memory runs out.
 */
char *runtime_host_file_path(const char *folder, const char *host_id, const char *recording, char **problem);

/** state/bundles/<host_id>.json, as runtime_host_file_path answers it. */
char *runtime_host_references_path(const char *state, const char *host_id, char **problem);

/**
 * What host_id recorded, as owned copies, or empty run and next when it recorded nothing readable;
 * rollback and rolled_back_from are NULL when it kept none or an empty one. Every member NULL when memory runs out or host_id
 * is no plain file name. Release with runtime_free_host_references.
 */
runtime_host_references runtime_read_host_references(const char *state, const char *host_id);

/**
 * Records references for host_id as JSON.stringify({run, next, rollback?, rolledBackFrom?}, null, 2) and a
 * newline, leaving rollback and rolledBackFrom out when they are NULL or empty. 1 on success; 0 with *problem set as
 * runtime_host_references_path sets it, or naming the file that could not be written.
 *
 * @implNote Replaced at once, so a sweep never reads half a file and takes it for none. A caller
 * changing what it read holds the registry lock around the read and this write, so two writers of one
 * host never lose each other's next.
 */
int runtime_record_host_references(const char *state, const char *host_id, const runtime_host_references *references,
                                   char **problem);

/** Deletes host_id's file and every run record of its starts; a host that recorded none counts as removed. */
int runtime_remove_host_references(const char *state, const char *host_id);

void runtime_free_host_references(runtime_host_references *references);

#endif
