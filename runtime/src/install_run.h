#ifndef RUNTIME_INSTALL_RUN_H
#define RUNTIME_INSTALL_RUN_H

#include "cJSON.h"

/** One install.ecosystem call: what it was given, the catalog it read and the report it is filling. */
typedef struct runtime_install_run {
    const cJSON *places;
    const char *portable;
    const char *state;
    const char *cache;
    const cJSON *environment;
    const cJSON *choices;
    const char *platform;
    cJSON *catalog;
    cJSON *report;
    cJSON *required_plugins;
} runtime_install_run;

/** The id ecosystem installs run under and record their references for. */
#define RUNTIME_ECOSYSTEM_HOST_ID "forebay-host"

/**
 * Settles every catalog application into the report's applications, one row each, installing through a
 * method only the ones choices name, and lists in missing each application with methods to choose
 * from and no choice yet. 1 on success; 0 with *refusal set for a choice naming an unknown application or
 * a method that is not offered, before anything is installed, or with none when memory runs out.
 */
int runtime_install_resolve_applications(runtime_install_run *run, cJSON **refusal);

/**
 * Installs the loader of every application the report calls installed or detected and registers each as
 * a host of its own with the commands its host.json asks for and the entry its application starts it
 * through, then installs and registers every background host a registered loader names. Installs and enables
 * in each loader's home the plugins its standard requires, recording id and version of each in
 * run->required_plugins; a loader whose required plugin no catalog lists is refused. Answers the ids of the
 * background hosts registered, or NULL when memory runs out; what could not be done is reported.
 */
cJSON *runtime_install_loaders(runtime_install_run *run);

/**
 * Registers loader id at version, installed in run's cache, as install registers it: its application
 * from the host.json that version carries, the provenance its registration already records kept, and the
 * commands its host.json asks for requested again. Answers what hosts.register answers, or NULL with
 * *refusal set, or with none when memory runs out. run's report needs only an applications array.
 */
cJSON *runtime_install_register_installed_loader(const runtime_install_run *run, const char *id, const char *version,
                                                 cJSON **refusal);

#endif
