#ifndef RUNTIME_LISTS_H
#define RUNTIME_LISTS_H

#include <stdint.h>

#include "cJSON.h"

/**
 * One list of three groups of rows, each an object with an id: installed, then detected, then available,
 * each group sorted by id, each entry {state, row}: its group's state and a copy of the row as the caller gave it. An
 * id appears once, in the strongest group that holds it, its row its first occurrence there. NULL when memory runs
 * out.
 */
cJSON *runtime_three_state_list(const cJSON *installed, const cJSON *detected, const cJSON *available);

/**
 * lists' plugins over installed, detected and available, each an array of rows { id, ... }: the three-state list of
 * them, or NULL with *refusal set, or with it NULL when memory runs out. The caller passes as detected the plugins its
 * detectors found; a detector that failed has not found its plugin.
 */
cJSON *runtime_lists_plugins(const cJSON *installed, const cJSON *detected, const cJSON *available,
                             cJSON **refusal);

/** The runtime_list_state a three-state list's state names, 0 for none. */
uint32_t runtime_list_state_of(const char *name);

/**
 * lists' applications over the catalog's items, the state place, an environment and detections: a three-state list
 * of rows { id, label }, one for the application of every host item: installed when a registered
 * host carries its id, detected when its binary is on the PATH of environment or a detection
 * { method, item, package, answer: { installed: true } } names it, available otherwise. NULL with *refusal set, or
 * with it NULL when memory runs out.
 *
 * @implNote A refused detection { method, item, refused } never detects: a method that could not answer
 * has not said the package is here. Nothing is launched and nothing reaches the network.
 */
cJSON *runtime_lists_applications(const cJSON *items, const char *state, const cJSON *environment,
                                  const cJSON *detections, cJSON **refusal);

#endif
