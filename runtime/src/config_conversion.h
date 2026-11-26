#ifndef RUNTIME_CONFIG_CONVERSION_H
#define RUNTIME_CONFIG_CONVERSION_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "config.h"
#include "definition_support.h"
#include "runtime/config.h"
#include "spisor/status.h"
#include "spisor/value.h"

/** Whether a consumer's tree can be filled: present, reaching through its first shape, and empty. */
int runtime_is_empty_tree(const spisor_value_tree *tree);

/**
 * The scope's home, names and portable place into a request, `*names` holding the names as the JSON the request
 * points at until the caller deletes it.
 *
 * @return OK; MALFORMED, with the refusal filled, for names that are not a well-formed record; OUT_OF_MEMORY.
 */
spisor_status runtime_scope_request(const runtime_settings_scope *scope, runtime_config_request *request,
                                    cJSON **names, spisor_refusal *refusal);

/** The JSON a plugin's own record holds, NULL for none; MALFORMED, refusing with `what`, for any other kind. */
spisor_status runtime_record_json(const spisor_value *value, const char *what, cJSON **json, spisor_refusal *refusal);

/** The runtime_ui_field_kind a declaration's `type` names in lower kebab case, such as `multi-choice`; 0 for none. */
uint32_t runtime_field_kind_named(const char *name);

/** Whether a declaration's `choices` are absent, NULL, or a list of texts. */
int runtime_are_setting_choices(const cJSON *choices);

/** Whether `name` names a layer a declaration may carry as its `layer`: `application` or `portable`. */
int runtime_is_settings_layer_name(const char *name);

/**
 * The declarations as the object runtime_config_request's `settings` holds, NULL for none.
 *
 * @return OK; MALFORMED for declarations without their array, a declaration without a key, or a layer the settings do
 * not know; OUT_OF_MEMORY.
 */
spisor_status runtime_declarations_json(const runtime_setting_declaration *settings, size_t count, cJSON **json,
                                        spisor_refusal *refusal);

/**
 * A change's key, value and layer into a request, `*value` holding the value as the JSON the request points at until
 * the caller deletes it. A NULL value stays NULL, for the operation to refuse as missing.
 *
 * @return OK; MALFORMED for a value of kind NONE or not well-formed, or a layer the settings do not know;
 * OUT_OF_MEMORY.
 */
/** Whether a change is present and reaches through its first shape. */
int runtime_is_whole_change(const runtime_settings_change *change);

spisor_status runtime_change_request(const runtime_settings_change *change, runtime_config_request *request,
                                     cJSON **value, spisor_refusal *refusal);

/**
 * Fills an empty tree with settings JSON, a NULL `json` leaving it of kind NONE.
 *
 * @return OK; REFUSED when the settings hold what no value tree can; OUT_OF_MEMORY.
 */
spisor_status runtime_tree_of_settings(const cJSON *json, spisor_value_tree *tree, spisor_refusal *refusal);

/** Every settings definition's release for a tree it filled. */
void runtime_release_tree(void *context, spisor_value_tree *tree);

#endif
