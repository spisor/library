#include "marketplace_sources.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "import_name.h"
#include "json_service.h"

#define MESSAGE_SIZE 1024
#define ID_FIX "give the source an id that is an import name: lowercase letters and digits joined by single hyphens or dots"

typedef struct accepted_source {
    const cJSON *entry;
    int index;
} accepted_source;

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static int has_scheme(const char *url, const char *scheme) {
    size_t length = strlen(scheme);
    for (size_t position = 0; position < length; position++) {
        if (url[position] == '\0' || (url[position] | 0x20) != scheme[position]) {
            return 0;
        }
    }
    return url[length] != '\0';
}

int runtime_marketplace_is_usable_url(const char *url) {
    return has_scheme(url, "file:") || has_scheme(url, "https:");
}

static const char *location_of(const cJSON *entry) {
    const char *url = text_of(entry, "url");
    return url != NULL ? url : text_of(entry, "provider");
}

/** Whether entry is no source, writing why into problem. */
static int finds_problem(const cJSON *entry, char *problem, size_t size) {
    const char *id = text_of(entry, "id");
    const char *url = text_of(entry, "url");
    const char *provider = text_of(entry, "provider");
    int has_url = cJSON_GetObjectItemCaseSensitive(entry, "url") != NULL;
    int has_provider = cJSON_GetObjectItemCaseSensitive(entry, "provider") != NULL;
    if (!cJSON_IsObject(entry)) {
        snprintf(problem, size, "a marketplace source must be an object with an id and a url or a provider");
    } else if (id == NULL || !runtime_is_import_name(id)) {
        snprintf(problem, size, "the source id \"%s\" is not an import name; %s", id == NULL ? "(none)" : id, ID_FIX);
    } else if (has_url == has_provider) {
        snprintf(problem, size, "the source %s must have exactly one of url and provider", id);
    } else if (has_url && (url == NULL || !runtime_marketplace_is_usable_url(url))) {
        snprintf(problem, size, "the url of source %s is not a file: or https: url", id);
    } else if (has_provider && (provider == NULL || provider[0] == '\0')) {
        snprintf(problem, size, "the provider of source %s is not the name of a plugin", id);
    } else {
        return 0;
    }
    return 1;
}

static int adds_skipped(cJSON *skipped, int index, const char *problem) {
    cJSON *row = cJSON_CreateObject();
    int added = row != NULL && cJSON_AddNumberToObject(row, "index", index) != NULL &&
                cJSON_AddStringToObject(row, "detail", problem) != NULL && cJSON_AddItemToArray(skipped, row);
    if (!added) {
        cJSON_Delete(row);
    }
    return added;
}

static int sorts_entries(const cJSON *configured, accepted_source *accepted, size_t *count, cJSON *skipped) {
    int index = 0;
    *count = 0;
    for (const cJSON *entry = configured == NULL ? NULL : configured->child; entry != NULL; entry = entry->next) {
        char problem[MESSAGE_SIZE];
        if (!finds_problem(entry, problem, sizeof problem)) {
            accepted[*count].entry = entry;
            accepted[*count].index = index;
            (*count)++;
        } else if (!adds_skipped(skipped, index, problem)) {
            return 0;
        }
        index++;
    }
    return 1;
}

static cJSON *refused_duplicate(const accepted_source *first, const accepted_source *second) {
    char message[MESSAGE_SIZE];
    snprintf(message, sizeof message, "marketplaces[%d] (%s) and marketplaces[%d] (%s) are both called %s", first->index,
             location_of(first->entry), second->index, location_of(second->entry), text_of(first->entry, "id"));
    return runtime_refusal(message, "give each marketplace its own id: the id is the marketplace in marketplace/name");
}

static const accepted_source *duplicate_of(const accepted_source *accepted, size_t index) {
    for (size_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(text_of(accepted[earlier].entry, "id"), text_of(accepted[index].entry, "id")) == 0) {
            return &accepted[earlier];
        }
    }
    return NULL;
}

static cJSON *source_row(const cJSON *entry) {
    const char *member = text_of(entry, "url") != NULL ? "url" : "provider";
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", text_of(entry, "id")) == NULL ||
        cJSON_AddStringToObject(row, member, text_of(entry, member)) == NULL) {
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

static int adds_sources(cJSON *sources, const accepted_source *accepted, size_t count, cJSON **refusal) {
    for (size_t index = 0; index < count; index++) {
        const accepted_source *earlier = duplicate_of(accepted, index);
        if (earlier != NULL) {
            *refusal = refused_duplicate(earlier, &accepted[index]);
            return 0;
        }
        cJSON *row = source_row(accepted[index].entry);
        if (row == NULL || !cJSON_AddItemToArray(sources, row)) {
            cJSON_Delete(row);
            return 0;
        }
    }
    return 1;
}

static const cJSON *configured_marketplaces(const cJSON *settings) {
    const cJSON *configured = cJSON_GetObjectItemCaseSensitive(settings, "marketplaces");
    return cJSON_IsArray(configured) ? configured : NULL;
}

static cJSON *answer_of(const cJSON *configured, cJSON **refusal) {
    cJSON *result = cJSON_CreateObject();
    cJSON *sources = result == NULL ? NULL : cJSON_AddArrayToObject(result, "sources");
    cJSON *skipped = result == NULL ? NULL : cJSON_AddArrayToObject(result, "skipped");
    accepted_source *accepted = malloc(((size_t)cJSON_GetArraySize(configured) + 1) * sizeof *accepted);
    size_t count = 0;
    int built = sources != NULL && skipped != NULL && accepted != NULL && sorts_entries(configured, accepted, &count, skipped) &&
                adds_sources(sources, accepted, count, refusal);
    free(accepted);
    if (!built) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *read_sources(const cJSON *argument, cJSON **refusal) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(argument, "settings");
    if (!cJSON_IsObject(settings)) {
        *refusal = runtime_refusal("the argument carries no usable settings", "send settings as the merged settings object");
        return NULL;
    }
    return answer_of(configured_marketplaces(settings), refusal);
}

RUNTIME_EXPORT int32_t runtime_marketplace_sources(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, read_sources);
}
