#ifndef RUNTIME_INSTALL_REPORT_H
#define RUNTIME_INSTALL_REPORT_H

#include "cJSON.h"

/**
 * An empty install report: {installed, cached, unavailable, enabled, applications, loaders, missing}, each
 * an empty array. NULL when memory runs out.
 */
cJSON *runtime_install_new_report(void);

/** Appends {id, message, fix} to the report's unavailable; 0 when memory runs out. */
int runtime_install_report_unavailable(cJSON *report, const char *id, const char *message, const char *fix);

/** Appends refusal, which it frees, to the report's unavailable under id; 0 when refusal is NULL or memory runs out. */
int runtime_install_report_refusal(cJSON *report, const char *id, cJSON *refusal);

/** The version target {id, pin?} installs: its pin, else the catalog item's latest; NULL when the catalog lists no item of kind under its id. */
const char *runtime_install_target_version(const cJSON *catalog, const cJSON *target, const char *kind);

/**
 * Installs target {id, pin?} of kind into cache at its version, recording on the report what was fetched
 * (installed), what was already there (cached, as id@version) or why it could not be (unavailable), and
 * answers the version now installed, borrowed from catalog or target; NULL when it is not, with
 * *out_of_memory set when memory ran out.
 */
const char *runtime_install_item(const cJSON *catalog, const cJSON *target, const char *kind, const char *cache,
                                 cJSON *report, int *out_of_memory);

/** {id} as an install target; NULL when memory runs out. */
cJSON *runtime_install_target(const char *id);

#endif
