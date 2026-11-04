#ifndef RUNTIME_HOLDERS_H
#define RUNTIME_HOLDERS_H

#include <stddef.h>

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
 * removing the file once it names neither a process nor a holder. 1 on success.
 *
 * @implNote Replaced at once, so a reader never sees half a record.
 */
int runtime_write_holders(const char *path, const runtime_holders_record *record);

/** Drops every holder whose process is no longer alive, and every one equal to dropped. */
void runtime_keep_live_holders(runtime_holders_record *record, double dropped);

/** Adds holder at the end. 0 when memory runs out. */
int runtime_add_holder(runtime_holders_record *record, double holder);

#endif
