#ifndef RUNTIME_UI_BINDING_H
#define RUNTIME_UI_BINDING_H

#include "cJSON.h"

/** Whether node carries a bind naming a service and a method. */
int runtime_ui_is_bound(const cJSON *node);

/**
 * The call a bound node of plugin needs, { plugin, binding: { service, method }, arguments }; NULL when
 * memory runs out.
 */
cJSON *runtime_ui_call_of(const char *plugin, const cJSON *node);

/**
 * The check a bound node's answer needs: { against: <ui-data component> }, or for a chart the branch its
 * own `chart` selects, { against: "progress" } for a meter and { against: "chart", members: ["series"] }
 * otherwise; NULL when memory runs out.
 *
 * @implNote The node picks the branch, never the answer, so the branch is checked on its own: an answer
 * whose other branch is valid would otherwise pass the chart definition's anyOf.
 */
cJSON *runtime_ui_check_of(const cJSON *node);

/** Whether two calls or answers name the same plugin, binding and arguments, members compared in any order. */
int runtime_ui_is_same_call(const cJSON *left, const cJSON *right);

/** Whether two checks name the same component and the same members. */
int runtime_ui_is_same_check(const cJSON *left, const cJSON *right);

/**
 * Whether answers is an array of calls each carrying exactly one outcome: { answer, checked? },
 * { error: { detail, fix } } or { unavailable: true }.
 */
int runtime_ui_is_answer_list(const cJSON *answers);

/** The answer among answers to call, or NULL. */
const cJSON *runtime_ui_answer_to(const cJSON *answers, const cJSON *call);

#endif
