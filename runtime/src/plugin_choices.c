#include "plugin_choices.h"

#include <stdlib.h>
#include <string.h>

static int by_name(const void *left, const void *right) {
    return strcmp((*(const cJSON *const *)left)->string, (*(const cJSON *const *)right)->string);
}

/** The object members of settings.plugins, sorted by id; NULL with *count 0 when there are none. */
static const cJSON **choices_of(const cJSON *settings, size_t *count) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(settings, "plugins");
    *count = 0;
    for (const cJSON *choice = cJSON_IsObject(plugins) ? plugins->child : NULL; choice != NULL; choice = choice->next) {
        *count += cJSON_IsObject(choice);
    }
    const cJSON **choices = *count == 0 ? NULL : malloc(*count * sizeof *choices);
    size_t index = 0;
    for (const cJSON *choice = choices == NULL ? NULL : plugins->child; choice != NULL; choice = choice->next) {
        if (cJSON_IsObject(choice)) {
            choices[index++] = choice;
        }
    }
    if (choices != NULL) {
        qsort(choices, *count, sizeof *choices, by_name);
    }
    return choices;
}

static const cJSON *choice_of(const cJSON *settings, const char *id) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(settings, "plugins");
    const cJSON *choice = cJSON_IsObject(plugins) ? cJSON_GetObjectItemCaseSensitive(plugins, id) : NULL;
    return cJSON_IsObject(choice) ? choice : NULL;
}

static int add_candidate(cJSON *candidates, const char *id, const cJSON *settings) {
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(choice_of(settings, id), "pin");
    cJSON *candidate = cJSON_CreateObject();
    int built = candidate != NULL && cJSON_AddStringToObject(candidate, "id", id) != NULL &&
                (!cJSON_IsString(pin) || cJSON_AddStringToObject(candidate, "pin", pin->valuestring) != NULL);
    if (!built || !cJSON_AddItemToArray(candidates, candidate)) {
        cJSON_Delete(candidate);
        return 0;
    }
    return 1;
}

static int is_candidate(const cJSON *candidates, const char *id) {
    for (const cJSON *candidate = candidates->child; candidate != NULL; candidate = candidate->next) {
        if (strcmp(cJSON_GetObjectItemCaseSensitive(candidate, "id")->valuestring, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int add_enabled(cJSON *candidates, const cJSON *settings) {
    size_t count = 0;
    const cJSON **choices = choices_of(settings, &count);
    int added = count == 0 || choices != NULL;
    for (size_t index = 0; added && index < count; index++) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(choices[index], "enabled"))) {
            added = add_candidate(candidates, choices[index]->string, settings);
        }
    }
    free(choices);
    return added;
}

static int is_disabled(const cJSON *settings, const char *id) {
    return cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(choice_of(settings, id), "enabled"));
}

/** The plugin ids defaults names, once each and sorted, as an array of strings; NULL when memory runs out. */
static cJSON *default_ids(const cJSON *defaults) {
    cJSON *ids = cJSON_CreateObject();
    for (const cJSON *entry = cJSON_IsObject(defaults) ? defaults->child : NULL; ids != NULL && entry != NULL;
         entry = entry->next) {
        if (cJSON_IsString(entry) && cJSON_GetObjectItemCaseSensitive(ids, entry->valuestring) == NULL &&
            cJSON_AddTrueToObject(ids, entry->valuestring) == NULL) {
            cJSON_Delete(ids);
            ids = NULL;
        }
    }
    return ids;
}

static int add_defaults(cJSON *candidates, const cJSON *settings, const cJSON *defaults) {
    cJSON *ids = default_ids(defaults);
    size_t count = 0;
    for (const cJSON *id = ids == NULL ? NULL : ids->child; id != NULL; id = id->next) {
        count++;
    }
    const cJSON **sorted = count == 0 ? NULL : malloc(count * sizeof *sorted);
    size_t index = 0;
    for (const cJSON *id = sorted == NULL ? NULL : ids->child; id != NULL; id = id->next) {
        sorted[index++] = id;
    }
    int added = ids != NULL && (count == 0 || sorted != NULL);
    if (sorted != NULL) {
        qsort(sorted, count, sizeof *sorted, by_name);
    }
    for (index = 0; added && index < count; index++) {
        const char *id = sorted[index]->string;
        if (!is_candidate(candidates, id) && !is_disabled(settings, id)) {
            added = add_candidate(candidates, id, settings);
        }
    }
    free(sorted);
    cJSON_Delete(ids);
    return added;
}

cJSON *runtime_plugin_candidates(const cJSON *settings, const cJSON *defaults) {
    cJSON *candidates = cJSON_CreateArray();
    if (candidates == NULL || !add_enabled(candidates, settings) || !add_defaults(candidates, settings, defaults)) {
        cJSON_Delete(candidates);
        return NULL;
    }
    return candidates;
}

cJSON *runtime_pinned_candidates(const cJSON *ids, const cJSON *settings) {
    cJSON *candidates = cJSON_CreateArray();
    for (const cJSON *id = ids == NULL ? NULL : ids->child; candidates != NULL && id != NULL; id = id->next) {
        if (cJSON_IsString(id) && !is_candidate(candidates, id->valuestring) &&
            !add_candidate(candidates, id->valuestring, settings)) {
            cJSON_Delete(candidates);
            candidates = NULL;
        }
    }
    return candidates;
}
