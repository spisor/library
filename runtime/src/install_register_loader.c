#include "install_ecosystem.h"

#include "install_run.h"
#include "install_support.h"
#include "json_service.h"

#define REGISTER_LOADER_FIX                                                                                       \
    "send {\"places\": {\"portable\", \"state\", \"cache\"}, \"environment\": {<name>: <value>}, \"id\": \"<loader id>\", " \
    "\"version\": \"<installed version>\"}"

static cJSON *register_loader(const cJSON *argument, cJSON **refusal) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    const char *id = runtime_install_text(argument, "id");
    const char *version = runtime_install_text(argument, "version");
    runtime_install_run run = {places, runtime_install_place(argument, "portable"), runtime_install_place(argument, "state"),
                               runtime_install_place(argument, "cache"), environment, NULL, NULL, NULL, NULL};
    if (run.portable == NULL || run.state == NULL || run.cache == NULL || !cJSON_IsObject(environment) || id == NULL ||
        id[0] == 0 || version == NULL || version[0] == 0) {
        *refusal = runtime_refusal("the argument carries no usable places, environment, loader id or version", REGISTER_LOADER_FIX);
        return NULL;
    }
    run.report = cJSON_Parse("{\"applications\": []}");
    cJSON *registered = run.report == NULL ? NULL : runtime_install_register_installed_loader(&run, id, version, refusal);
    cJSON_Delete(run.report);
    return registered;
}

RUNTIME_EXPORT int32_t runtime_install_register_loader(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, register_loader);
}
