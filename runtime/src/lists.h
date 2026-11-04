#ifndef RUNTIME_LISTS_H
#define RUNTIME_LISTS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * One list of three groups of rows, each an object with an id: installed, then detected, then available,
 * each group sorted by id and each row given its group's state. An id appears once, in the strongest
 * group that holds it, carrying the members of its first occurrence there. NULL when memory runs out.
 */
cJSON *runtime_three_state_list(const cJSON *installed, const cJSON *detected, const cJSON *available);

/**
 * lists.plugins: { installed, detected, available }, each an array of rows { id, ... }, answering
 * { items }, the three-state list of them. The caller passes as detected the plugins its detectors
 * found; a detector that failed has not found its plugin.
 */
RUNTIME_EXPORT int32_t runtime_lists_plugins(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * lists.applications: { catalog, places: { state }, environment, detections } answering { items }, a row
 * { id, label, state } for the application of every host item of catalog: installed when a registered
 * host carries its id, detected when its binary is on the PATH of environment or a detection
 * { method, item, package, answer: { installed: true } } names it, available otherwise.
 *
 * @implNote A refused detection { method, item, refused } never detects: a method that could not answer
 * has not said the package is here. Nothing is launched and nothing reaches the network.
 */
RUNTIME_EXPORT int32_t runtime_lists_applications(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
