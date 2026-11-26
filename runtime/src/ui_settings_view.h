#ifndef RUNTIME_UI_SETTINGS_VIEW_H
#define RUNTIME_UI_SETTINGS_VIEW_H

#include "cJSON.h"

/** The title of every generated settings screen and of its navigation entry. */
#define RUNTIME_UI_SETTINGS_TITLE "Settings"

/**
 * The view that edits description's settings, { title, nodes }, each field node carrying its kind's name as `type`, its
 * declaration's `choices` and its `default` from `config.defaults` when it has them; NULL when memory runs out.
 */
cJSON *runtime_ui_settings_view_of(const cJSON *description);

/**
 * Whether description's settings are absent or a record of declarations each a record whose type, when it has one,
 * names a runtime_ui_field_kind and whose choices are a list of texts.
 */
int runtime_ui_are_settings_readable(const cJSON *description);

#endif
