#include "install_methods.h"

#include <string.h>

#include "cache_refusal.h"
#include "install.h"
#include "install_support.h"
#include "json_service.h"

#ifdef SPISOR_INSTALL_TEST_HOOKS
#include "install_hooks.h"
#endif

#define DETECT_FIX                                                                                       \
    "send {\"places\": {\"portable\": \"<absolute directory>\", \"cache\": \"<absolute directory>\"}, " \
    "\"item\": \"<id>\", \"manifestPackages\"?: {<method>: <package>}}"

static const runtime_install_method *const METHODS[] = {
#ifdef SPISOR_INSTALL_TEST_HOOKS
    &runtime_install_fake_method,
#endif
    NULL,
};

const runtime_install_method *runtime_install_method_named(const char *name) {
    for (size_t index = 0; name != NULL && METHODS[index] != NULL; index++) {
        if (strcmp(METHODS[index]->name, name) == 0) {
            return METHODS[index];
        }
    }
    return NULL;
}

static cJSON *detection_head(const char *method, const char *item) {
    cJSON *detection = cJSON_CreateObject();
    if (detection == NULL || cJSON_AddStringToObject(detection, "method", method) == NULL ||
        cJSON_AddStringToObject(detection, "item", item) == NULL) {
        cJSON_Delete(detection);
        return NULL;
    }
    return detection;
}

static cJSON *refused_detection(cJSON *detection, cJSON *refusal) {
    const char *message = runtime_install_text(refusal, "message");
    if (message == NULL || cJSON_AddStringToObject(detection, "refused", message) == NULL) {
        cJSON_Delete(detection);
        detection = NULL;
    }
    cJSON_Delete(refusal);
    return detection;
}

static cJSON *answered_detection(cJSON *detection, const cJSON *mapped, cJSON *answer) {
    int complete = runtime_install_copy_into(detection, "package", cJSON_GetObjectItemCaseSensitive(mapped, "name")) &&
                   runtime_install_copy_into(detection, "options", cJSON_GetObjectItemCaseSensitive(mapped, "options")) &&
                   cJSON_AddItemToObject(detection, "answer", answer);
    if (!complete) {
        cJSON_Delete(answer);
        cJSON_Delete(detection);
        return NULL;
    }
    return detection;
}

cJSON *runtime_install_detection(const cJSON *places, const char *item, const runtime_install_method *method,
                                 const cJSON *mapped) {
    cJSON *detection = detection_head(method->name, item);
    if (detection == NULL) {
        return NULL;
    }
    cJSON *refusal = NULL;
    cJSON *answer = method->detect(places, mapped, &refusal);
    if (answer != NULL) {
        return answered_detection(detection, mapped, answer);
    }
    if (refusal != NULL) {
        return refused_detection(detection, refusal);
    }
    cJSON_Delete(detection);
    return NULL;
}

int runtime_install_operate(const cJSON *places, const char *method_name, const char *operation, const cJSON *mapped,
                            cJSON **refusal) {
    const runtime_install_method *method = runtime_install_method_named(method_name);
    if (method == NULL) {
        const char *parts[3] = {"this runtime has no ", method_name, " install method built in"};
        *refusal = runtime_cache_refusal(parts, 3, "choose another method, or use the package manager yourself");
        return 0;
    }
    return method->operate(places, operation, mapped, refusal);
}

static cJSON *packages_call(const cJSON *places, const char *item, const cJSON *manifest_packages) {
    cJSON *call = runtime_install_places_call(places);
    if (call == NULL || cJSON_AddStringToObject(call, "item", item) == NULL ||
        !runtime_install_copy_into(call, "manifestPackages", manifest_packages)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static int add_detections(cJSON *detections, const cJSON *places, const char *item, const cJSON *packages) {
    for (size_t index = 0; METHODS[index] != NULL; index++) {
        const cJSON *mapped = cJSON_GetObjectItemCaseSensitive(packages, METHODS[index]->name);
        cJSON *detection = mapped == NULL ? NULL : runtime_install_detection(places, item, METHODS[index], mapped);
        if (mapped != NULL && (detection == NULL || !cJSON_AddItemToArray(detections, detection))) {
            cJSON_Delete(detection);
            return 0;
        }
    }
    return 1;
}

cJSON *runtime_install_detections(const cJSON *places, const char *item, const cJSON *manifest_packages,
                                  cJSON **refusal) {
    cJSON *merged = runtime_call_or_refuse(runtime_install_packages, packages_call(places, item, manifest_packages), refusal);
    cJSON *detections = merged == NULL ? NULL : cJSON_CreateArray();
    if (detections != NULL &&
        !add_detections(detections, places, item, cJSON_GetObjectItemCaseSensitive(merged, "packages"))) {
        cJSON_Delete(detections);
        detections = NULL;
    }
    cJSON_Delete(merged);
    return detections;
}

static cJSON *detect_for(const cJSON *argument, cJSON **refusal) {
    const char *item = runtime_install_text(argument, "item");
    const cJSON *manifest_packages = cJSON_GetObjectItemCaseSensitive(argument, "manifestPackages");
    if (item == NULL || item[0] == 0 || runtime_install_place(argument, "cache") == NULL ||
        (manifest_packages != NULL && !cJSON_IsObject(manifest_packages))) {
        *refusal = runtime_refusal("the argument carries no usable places, item or manifestPackages", DETECT_FIX);
        return NULL;
    }
    cJSON *detections = runtime_install_detections(cJSON_GetObjectItemCaseSensitive(argument, "places"), item,
                                                   manifest_packages, refusal);
    cJSON *answer = detections == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL || !cJSON_AddItemToObject(answer, "detections", detections)) {
        cJSON_Delete(answer);
        cJSON_Delete(detections);
        return NULL;
    }
    return answer;
}

RUNTIME_EXPORT int32_t runtime_install_detect(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, detect_for);
}
