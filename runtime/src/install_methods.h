#ifndef RUNTIME_INSTALL_METHODS_H
#define RUNTIME_INSTALL_METHODS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * One install method built into this runtime. detect answers {installed, version?} for the package a
 * mapping {name, options?} names, reading the method's own records and launching nothing; operate runs
 * install, update or remove of it and answers 1. Both answer NULL or 0 with *refusal set to
 * {message, fix}, or with no refusal when memory runs out. places carries the caller's places.
 */
typedef struct runtime_install_method {
    const char *name;
    cJSON *(*detect)(const cJSON *places, const cJSON *mapped, cJSON **refusal);
    int (*operate)(const cJSON *places, const char *operation, const cJSON *mapped, cJSON **refusal);
} runtime_install_method;

/**
 * The method called name, or NULL when none is built in.
 *
 * @implNote Plan seven adds the official methods; until then the test build alone carries fake.
 */
const runtime_install_method *runtime_install_method_named(const char *name);

/**
 * One detection of item through method, as forebay/host's detectThroughMethods answered one:
 * {method, item, package, options?, answer} or {method, item, refused}. mapped is {name, options?}.
 * NULL when memory runs out.
 */
cJSON *runtime_install_detection(const cJSON *places, const char *item, const runtime_install_method *method,
                                 const cJSON *mapped);

/**
 * Runs operation of the package mapped names through the method called method_name: 1, or 0 with
 * *refusal set, a method that is not built in being refused with the methods to choose instead.
 */
int runtime_install_operate(const cJSON *places, const char *method_name, const char *operation, const cJSON *mapped,
                            cJSON **refusal);

/**
 * The detections of item through every built-in method its merged package mapping names, as
 * install.detect answers them; NULL with *refusal set when the mapping cannot be read.
 */
cJSON *runtime_install_detections(const cJSON *places, const char *item, const cJSON *manifest_packages,
                                  cJSON **refusal);

/**
 * install.detect: { places: { portable, cache }, item, manifestPackages? } answering { detections }, one
 * for each built-in method the merged package mapping (install.packages) names, in the table's order.
 *
 * @implNote Never launches a program and never reaches the network (shared-host spec 8): a method reads
 * its package manager's own records.
 */
RUNTIME_EXPORT int32_t runtime_install_detect(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
