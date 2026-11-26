#ifndef RUNTIME_UI_COMPONENTS_H
#define RUNTIME_UI_COMPONENTS_H

#include <stdint.h>

/** The runtime_ui_component a view's node names by its value's name in lower snake case; 0 for any other name. */
uint32_t runtime_ui_component_of(const char *name);

/** The name a node gives a runtime_ui_component, NULL for none. */
const char *runtime_ui_component_name(uint32_t component);

#endif
