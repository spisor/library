#ifndef RUNTIME_INSTALL_REFERENCES_H
#define RUNTIME_INSTALL_REFERENCES_H

#include "cJSON.h"

/**
 * Under the registry lock, sets each member of versions, an id-to-version object, into what host_id runs
 * in its references, keeping every other member, next and rollback. 1 on success; 0 with *refusal set,
 * or with none when memory runs out.
 *
 * @implNote An install calls this before it stores a version and again once it is stored, so a sweep
 * keeps the version while its files arrive unlocked (Ruling L); the lock is held only around the read
 * and the write, never around a download.
 */
int runtime_install_reference_versions(const char *state, const char *host_id, const cJSON *versions, cJSON **refusal);

/**
 * Under the registry lock, replaces what host_id runs in its references with run, keeping next and
 * rollback, as forebay/host's recordReferences did after an install. 1 on success, as above.
 */
int runtime_install_replace_run(const char *state, const char *host_id, const cJSON *run, cJSON **refusal);

#endif
