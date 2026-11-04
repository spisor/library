#ifndef RUNTIME_UI_ITEMS_H
#define RUNTIME_UI_ITEMS_H

#include "cJSON.h"

/**
 * A copy of items with every row that cannot be used, detected or available, marked { unavailable: true,
 * install: true }; NULL when memory runs out.
 *
 * @implNote A row that cannot be used offers only install, so an `action` it carried is dropped by design;
 * `install` is its own member, so it can never be read as one of the plugin's action ids.
 */
cJSON *runtime_ui_presented_items(const cJSON *items);

#endif
