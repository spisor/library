#ifndef RUNTIME_UI_SCREEN_H
#define RUNTIME_UI_SCREEN_H

#include "cJSON.h"

/** Whether description is a plugin manifest with an id. */
int runtime_ui_is_description(const cJSON *description);

/** Whether descriptions is an array of plugin manifests with an id. */
int runtime_ui_is_description_list(const cJSON *descriptions);

/** The screen a navigation entry opens: whose it is, which kind, and the view whose nodes it renders. */
typedef struct ui_screen {
    const char *plugin;
    const char *kind;
    /** The declared view id; NULL for the generated settings screen, which is addressed by its kind. */
    const char *view_id;
    const cJSON *view;
    cJSON *generated;
} ui_screen;

/**
 * Opens the screen argument's screen names among its descriptions: 1, or 0 with *refusal set (NULL when
 * memory ran out). Closed with runtime_ui_close_screen.
 */
int runtime_ui_open_screen(const cJSON *argument, ui_screen *screen, cJSON **refusal);

void runtime_ui_close_screen(ui_screen *screen);

#endif
