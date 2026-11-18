#ifndef RUNTIME_UI_DEFINITION_H
#define RUNTIME_UI_DEFINITION_H

#include "cJSON.h"
#include "runtime/ui.h"
#include "spisor/status.h"
#include "spisor/value.h"

#define RUNTIME_UI_HEADER "the ui header"

/** Whether a well-formed value is a record holding its id as text, as a plugin manifest does. */
int runtime_ui_is_manifest(const spisor_value *value);

/** Whether a well-formed value is a list of manifests. */
int runtime_ui_is_manifest_list(const spisor_value *value);

/**
 * OK for a well-formed value of `kind`, SPISOR_VALUE_NONE meaning any kind but none; MALFORMED for NULL or any other
 * value; OUT_OF_MEMORY when the check cannot hold the tree.
 */
spisor_status runtime_ui_check_value(const spisor_value *value, spisor_value_kind kind);

/** Adds a checked value to `object` as JSON under `name`; 0 when memory runs out. */
int runtime_ui_add_value(cJSON *object, const char *name, const spisor_value *value);

/**
 * Checks a plan or render argument whole before anything is allocated but the checks' own walks.
 *
 * @return OK; MALFORMED with the refusal filled; OUT_OF_MEMORY.
 */
spisor_status runtime_ui_check_screen_argument(const runtime_ui_screen_argument *argument, spisor_refusal *refusal);

/** The JSON call plan and render take for a checked argument; NULL when memory runs out. */
cJSON *runtime_ui_screen_call(const runtime_ui_screen_argument *argument);

/** Fills a runtime_ui_plan_answer from what plan answered, as a runtime_answer_filler. */
spisor_status runtime_ui_fill_plan(const cJSON *answered, void *filled);

/** Fills a runtime_ui_screen from what render or plugin_list answered, as a runtime_answer_filler. */
spisor_status runtime_ui_fill_screen(const cJSON *answered, void *filled);

/** The name a kind has in JSON; NULL for a value outside runtime_ui_kind. */
const char *runtime_ui_kind_name(uint32_t kind);

/** The runtime_ui_kind a JSON name stands for; 0 for none. */
uint32_t runtime_ui_kind_of(const char *name);

#endif
