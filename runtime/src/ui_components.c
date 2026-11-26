#include "ui_components.h"

#include <string.h>

#include "runtime/ui.h"

static const char *const COMPONENT_NAMES[] = {
    NULL,        "stack",      "grid",    "split",         "tabs",         "section", "card",     "divider",
    "text",      "list",       "table",   "tree",          "key_value",    "chart",   "log_stream", "code_block",
    "diff",      "badge",      "icon",    "field",         "search",       "form",    "command_entry",
    "confirmation", "toast",   "progress", "spinner",      "empty",        "loading", "error",    "unavailable"};

#define COMPONENT_COUNT (sizeof COMPONENT_NAMES / sizeof COMPONENT_NAMES[0])

_Static_assert(COMPONENT_COUNT == RUNTIME_UI_COMPONENT_UNAVAILABLE + 1, "one name per runtime_ui_component");

uint32_t runtime_ui_component_of(const char *name) {
    for (uint32_t component = 1; name != NULL && component < COMPONENT_COUNT; component++) {
        if (strcmp(COMPONENT_NAMES[component], name) == 0) {
            return component;
        }
    }
    return 0;
}

const char *runtime_ui_component_name(uint32_t component) {
    return component < COMPONENT_COUNT ? COMPONENT_NAMES[component] : NULL;
}
