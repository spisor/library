#ifndef RUNTIME_UI_SETTINGS_VIEW_H
#define RUNTIME_UI_SETTINGS_VIEW_H

#include "cJSON.h"

/** The title of every generated settings screen and of its navigation entry. */
#define RUNTIME_UI_SETTINGS_TITLE "Settings"

/** The view that edits description's settings, { title, nodes }; NULL when memory runs out. */
cJSON *runtime_ui_settings_view_of(const cJSON *description);

#endif
