#include "interface_start.h"
#include "allocation.h"

#include <string.h>

#include "cache_refusal.h"
#include "hosts_layer_start.h"
#include "hosts_layers.h"
#include "install_run.h"
#include "interface_choice.h"
#include "places.h"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

/** "set spisor-host's interface setting to one of <ids>, then run spisor-host install again", or how to list one. */
static char *choosing_fix(const cJSON *listed) {
    char *ids = cJSON_GetArraySize(listed) == 0 ? NULL : runtime_joined_interface_ids(listed);
    const char *parts[3] = {"set spisor-host's " RUNTIME_INTERFACE_SETTING " setting to one of ", ids,
                            ", then run spisor-host install again"};
    const char *none = "add a catalog source listing an interface item, set spisor-host's " RUNTIME_INTERFACE_SETTING
                       " setting to its id, then run spisor-host install again";
    char *fix = cJSON_GetArraySize(listed) == 0 ? runtime_cache_text(&none, 1)
                                                : ids == NULL ? NULL : runtime_cache_text(parts, 3);
    spisor_memory_release(ids);
    return fix;
}

/** The refusal of a start the choice cannot make, with the message parts given; NULL when memory runs out. */
static cJSON *choice_refusal(const runtime_interface_choice *choice, const char *const *parts, size_t count) {
    char *fix = choosing_fix(choice->listed);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, count, fix);
    spisor_memory_release(fix);
    return refusal;
}

static cJSON *headless_refusal(const runtime_interface_choice *choice) {
    const char *setting = "spisor-host's " RUNTIME_INTERFACE_SETTING " setting is " RUNTIME_HEADLESS
                          ", so this install has no interface to start";
    const char *unlisted = "the catalog lists no interface item, so this install has no interface to start";
    return choice_refusal(choice, cJSON_GetArraySize(choice->listed) == 0 ? &unlisted : &setting, 1);
}

static cJSON *unchosen_refusal(const runtime_interface_choice *choice) {
    char *ids = runtime_joined_interface_ids(choice->listed);
    const char *parts[3] = {"the catalog lists more than one interface, ", ids,
                            ", and spisor-host's " RUNTIME_INTERFACE_SETTING " setting chooses none"};
    cJSON *refusal = ids == NULL ? NULL : choice_refusal(choice, parts, 3);
    spisor_memory_release(ids);
    return refusal;
}

static cJSON *unlisted_refusal(const runtime_interface_choice *choice) {
    const char *parts[3] = {"spisor-host's " RUNTIME_INTERFACE_SETTING " setting names ", choice->id,
                            ", which the catalog lists as no interface item"};
    return choice_refusal(choice, parts, 3);
}

/** The catalog's interface item the choice names, NULL with *refusal set when it names none or none it lists. */
static const cJSON *chosen_item(const runtime_interface_choice *choice, const cJSON *catalog, cJSON **refusal) {
    const cJSON *item = choice->kind != RUNTIME_INTERFACE_CHOSEN
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"),
                                                               choice->id);
    const char *kind = text_of(item, "kind");
    if (kind == NULL || strcmp(kind, "interface") != 0) {
        item = NULL;
        *refusal = choice->kind == RUNTIME_INTERFACE_HEADLESS   ? headless_refusal(choice)
                   : choice->kind == RUNTIME_INTERFACE_UNCHOSEN ? unchosen_refusal(choice)
                   : choice->kind == RUNTIME_INTERFACE_CHOSEN   ? unlisted_refusal(choice)
                                                                : NULL;
    }
    return item;
}

static int add_text(cJSON *array, const char *text) {
    cJSON *item = cJSON_CreateString(text);
    if (item == NULL || !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

/** `ui <binary> [<loader id>] [--window]`; NULL when memory runs out. */
static cJSON *interface_arguments(const char *binary, const char *loader_id, int window) {
    cJSON *arguments = cJSON_CreateArray();
    int built = arguments != NULL && add_text(arguments, RUNTIME_INTERFACE_WORD) && add_text(arguments, binary) &&
                (loader_id == NULL || add_text(arguments, loader_id)) &&
                (!window || add_text(arguments, RUNTIME_INTERFACE_WINDOW_FLAG));
    if (!built) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

/** The start of the chosen item, as runtime_interface_start answers it. */
static cJSON *start_of(const cJSON *environment, const char *cache, const char *id, const cJSON *item,
                       const char *loader_id, int window, cJSON **refusal) {
    cJSON *hint = runtime_layer_hint(cache, id, refusal);
    const char *binary = hint == NULL ? NULL : runtime_hinted_binary(hint, id, refusal);
    cJSON *arguments = binary == NULL ? NULL : interface_arguments(binary, loader_id, window);
    runtime_entry_start_request request = {cache, environment, id, text_of(hint, "host"), text_of(item, "runtime"),
                                           NULL, arguments};
    cJSON *start = arguments == NULL ? NULL : runtime_entry_start(&request, refusal);
    cJSON_Delete(arguments);
    cJSON_Delete(hint);
    return start;
}

cJSON *runtime_interface_start(const cJSON *environment, const char *loader_id, int window, cJSON **refusal) {
    runtime_loader_layers layers;
    cJSON *places = runtime_places_of_environment(environment, refusal);
    cJSON *start = NULL;
    memset(&layers, 0, sizeof layers);
    if (places != NULL && runtime_read_loader_layers(places, RUNTIME_ECOSYSTEM_HOST_ID, &layers, refusal)) {
        runtime_interface_choice choice = runtime_choose_interface(layers.settings, layers.catalog);
        const cJSON *item = chosen_item(&choice, layers.catalog, refusal);
        start = item == NULL ? NULL
                             : start_of(environment, text_of(places, "cache"), choice.id, item, loader_id, window,
                                        refusal);
        runtime_free_interface_choice(&choice);
        runtime_free_loader_layers(&layers);
    }
    cJSON_Delete(places);
    return start;
}
