#include "interface_choice.h"

#include <string.h>

#include "catalog_text.h"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static int is_interface(const cJSON *item) {
    const char *kind = text_of(item, "kind");
    return kind != NULL && strcmp(kind, "interface") == 0;
}

/** The ids of every item the catalog lists of kind interface, in its order; NULL when memory runs out. */
static cJSON *interface_items(const cJSON *catalog) {
    cJSON *ids = cJSON_CreateArray();
    const cJSON *items = ids == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(catalog, "items");
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        cJSON *id = is_interface(item) ? cJSON_CreateString(item->string) : NULL;
        if (is_interface(item) && (id == NULL || !cJSON_AddItemToArray(ids, id))) {
            cJSON_Delete(id);
            cJSON_Delete(ids);
            return NULL;
        }
    }
    return ids;
}

runtime_interface_choice runtime_choose_interface(const cJSON *values, const cJSON *catalog) {
    runtime_interface_choice choice = {RUNTIME_INTERFACE_OUT_OF_MEMORY, NULL, interface_items(catalog)};
    const char *chosen = text_of(values, RUNTIME_INTERFACE_SETTING);
    int count = cJSON_GetArraySize(choice.listed);
    if (choice.listed == NULL) {
        return choice;
    }
    if (chosen != NULL) {
        choice.kind = strcmp(chosen, RUNTIME_HEADLESS) == 0 ? RUNTIME_INTERFACE_HEADLESS : RUNTIME_INTERFACE_CHOSEN;
        choice.id = choice.kind == RUNTIME_INTERFACE_CHOSEN ? chosen : NULL;
    } else if (count == 1) {
        choice.kind = RUNTIME_INTERFACE_CHOSEN;
        choice.id = choice.listed->child->valuestring;
    } else {
        choice.kind = count == 0 ? RUNTIME_INTERFACE_HEADLESS : RUNTIME_INTERFACE_UNCHOSEN;
    }
    return choice;
}

void runtime_free_interface_choice(runtime_interface_choice *choice) {
    cJSON_Delete(choice->listed);
    choice->listed = NULL;
    choice->id = NULL;
}

char *runtime_joined_interface_ids(const cJSON *ids) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, "");
    for (const cJSON *id = ids->child; id != NULL; id = id->next) {
        runtime_catalog_text_add(&text, id == ids->child ? "" : ", ");
        runtime_catalog_text_add(&text, id->valuestring);
    }
    return runtime_catalog_text_finish(&text);
}
