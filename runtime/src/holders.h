#ifndef RUNTIME_HOLDERS_H
#define RUNTIME_HOLDERS_H

#include <stddef.h>

#include "cJSON.h"
#include "spisor/status.h"

/**
 * The process running one background host and the processes holding it, each by its pid. alive_at is
 * when the host last said it runs, in epoch milliseconds, and check_milliseconds how often it says so;
 * pid, alive_at and check_milliseconds are NaN where the file records null.
 */
typedef struct runtime_holders_record {
    double pid;
    double alive_at;
    double check_milliseconds;
    double *holders;
    size_t count;
} runtime_holders_record;

/** A record naming no process and no holder. */
runtime_holders_record runtime_no_holders(void);

void runtime_free_holders(runtime_holders_record *record);

/**
 * state/processes/<host_id> and extension, freed with free; NULL with *problem set (freed with free)
 * when host_id is no plain file name, or with *problem NULL when memory runs out.
 */
char *runtime_holders_path(const char *state, const char *host_id, const char *extension, char **problem);

/** state/processes, freed with free; NULL when memory runs out. */
char *runtime_processes_folder(const char *state);

/**
 * What the holders file at path records, into *record: no holders when there is no file or it is not
 * a holders record, an object whose holders are all whole numbers above zero. 0 when memory runs out.
 */
int runtime_read_holders(const char *path, runtime_holders_record *record);

/**
 * Records record at path as JSON.stringify({pid, aliveAt, checkMilliseconds, holders}) and a newline,
 * removing the file once it names neither a process nor a holder. OK; REFUSED when the file cannot be written or
 * removed; OUT_OF_MEMORY, having written nothing.
 *
 * @implNote Replaced at once, so a reader never sees half a record.
 */
spisor_status runtime_write_holders(const char *path, const runtime_holders_record *record);

/**
 * A holders record naming a host about to be started, its tree and a buffer wide enough for its text whatever the
 * host's pid and stamp, so recording the host once it runs allocates nothing beyond replacing the file.
 */
typedef struct runtime_holders_draft {
    cJSON *tree;
    char *text;
    size_t capacity;
} runtime_holders_draft;

/** Drafts record as it reads once a host it starts is its host, never stamped yet. 0 when memory runs out. */
int runtime_draft_started_host(const runtime_holders_record *record, runtime_holders_draft *draft);

/** Fills the drafted host's pid and the time it was started, and prints the draft's text, allocating nothing. */
int runtime_print_started_host(runtime_holders_draft *draft, double pid, double alive_at);

/** Records the printed draft at path, replaced at once as runtime_write_holders replaces it. OK; REFUSED. */
spisor_status runtime_write_holders_draft(const char *path, const runtime_holders_draft *draft);

void runtime_free_holders_draft(runtime_holders_draft *draft);

/** Drops every holder whose process is no longer alive, and every one equal to dropped. */
void runtime_keep_live_holders(runtime_holders_record *record, double dropped);

/** Adds holder at the end. 0 when memory runs out. */
int runtime_add_holder(runtime_holders_record *record, double holder);

#endif
