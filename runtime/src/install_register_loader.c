#include "install_ecosystem.h"

#include "install_run.h"
#include "install_support.h"
#include "json_service.h"

#define REGISTER_LOADER_FIX                                                                                  \
    "pass the places {\"portable\", \"state\", \"cache\"} as absolute directories, the environment as "      \
    "{<name>: <value>}, and the loader's id and installed version"

cJSON *runtime_install_register_loader(const cJSON *places, const cJSON *environment, const char *id,
                                       const char *version, cJSON **refusal) {
    runtime_install_run run = {places,
                               runtime_install_place(places, "portable"),
                               runtime_install_place(places, "state"),
                               runtime_install_place(places, "cache"),
                               environment,
                               NULL,
                               NULL,
                               NULL,
                               NULL,
                               NULL};
    *refusal = NULL;
    if (run.portable == NULL || run.state == NULL || run.cache == NULL || !cJSON_IsObject(environment) || id == NULL ||
        id[0] == 0 || version == NULL || version[0] == 0) {
        *refusal = runtime_refusal("the call carries no usable places, environment, loader id or version",
                                   REGISTER_LOADER_FIX);
        return NULL;
    }
    run.report = cJSON_Parse("{\"applications\": []}");
    cJSON *registered = run.report == NULL ? NULL : runtime_install_register_installed_loader(&run, id, version, refusal);
    cJSON_Delete(run.report);
    return registered;
}
