#include "marketplace_name_lookup.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_service.h"
#include "marketplace_check.h"

#define MESSAGE_SIZE 2048
#define PROBLEM_SIZE 1024

/** A name as the caller wrote it: a bare name, or a marketplace and a name split at the first slash. */
typedef struct wanted_name {
    const char *marketplace;
    size_t marketplace_length;
    const char *name;
} wanted_name;

typedef struct match {
    const char *marketplace;
    const cJSON *entry;
} match;

static cJSON *refuse(cJSON **refusal, const char *fix, const char *format, ...) {
    char message[MESSAGE_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static wanted_name wanted_from(const char *text) {
    const char *slash = strchr(text, '/');
    wanted_name wanted = {NULL, 0, text};
    if (slash != NULL) {
        wanted.marketplace = text;
        wanted.marketplace_length = (size_t)(slash - text);
        wanted.name = slash + 1;
    }
    return wanted;
}

static int is_in_marketplace(const wanted_name *wanted, const char *marketplace) {
    return wanted->marketplace == NULL ||
           (strlen(marketplace) == wanted->marketplace_length && strncmp(marketplace, wanted->marketplace, wanted->marketplace_length) == 0);
}

static int is_one_source_twice(const cJSON *copies, const cJSON *pair, const char *marketplace) {
    for (const cJSON *earlier = copies->child; earlier != pair; earlier = earlier->next) {
        if (strcmp(text_of(cJSON_GetObjectItemCaseSensitive(earlier, "source"), "id"), marketplace) == 0) {
            return 1;
        }
    }
    return 0;
}

/** The id of the source pair names when pair holds a usable source and a copy that satisfies its schema; otherwise NULL and *refusal. */
static const char *checked_source(const cJSON *copies, const cJSON *pair, cJSON **refusal) {
    const char *marketplace = text_of(cJSON_GetObjectItemCaseSensitive(pair, "source"), "id");
    const cJSON *copy = cJSON_GetObjectItemCaseSensitive(pair, "copy");
    char problem[PROBLEM_SIZE];
    if (marketplace == NULL) {
        refuse(refusal, "send each copy with its source as {source: {id}, copy}", "a copy has no source id");
    } else if (is_one_source_twice(copies, pair, marketplace)) {
        refuse(refusal, "send one copy for each source", "two copies came from the source %s", marketplace);
    } else if (!runtime_marketplace_check_copy(copy, problem, sizeof problem)) {
        refuse(refusal, "send the copy marketplace.read answered for that source", "the copy of %s is not a marketplace copy: %s",
               marketplace, problem);
    } else {
        return marketplace;
    }
    return NULL;
}

static match *grown(match *matches, size_t count) {
    return realloc(matches, (count + 1) * sizeof *matches);
}

/** Adds every entry of the pair's copy named as wanted; 0 when memory ran out. */
static int collects_matches(const wanted_name *wanted, const char *marketplace, const cJSON *copy, match **matches, size_t *count) {
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(copy, "entries");
    for (const cJSON *entry = entries->child; entry != NULL; entry = entry->next) {
        if (strcmp(text_of(entry, "name"), wanted->name) != 0) {
            continue;
        }
        match *more = grown(*matches, *count);
        if (more == NULL) {
            return 0;
        }
        *matches = more;
        (*matches)[*count].marketplace = marketplace;
        (*matches)[*count].entry = entry;
        (*count)++;
    }
    return 1;
}

static int finds_matches(const cJSON *copies, const wanted_name *wanted, match **matches, size_t *count, cJSON **refusal) {
    for (const cJSON *pair = copies->child; pair != NULL; pair = pair->next) {
        const char *marketplace = checked_source(copies, pair, refusal);
        if (marketplace == NULL) {
            return 0;
        }
        if (is_in_marketplace(wanted, marketplace) &&
            !collects_matches(wanted, marketplace, cJSON_GetObjectItemCaseSensitive(pair, "copy"), matches, count)) {
            return 0;
        }
    }
    return 1;
}

static cJSON *found_answer(const match *found) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddStringToObject(answer, "id", text_of(found->entry, "id")) == NULL ||
        cJSON_AddStringToObject(answer, "marketplace", found->marketplace) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int adds_qualified_form(cJSON *forms, const match *found) {
    char form[MESSAGE_SIZE];
    snprintf(form, sizeof form, "%s/%s", found->marketplace, text_of(found->entry, "name"));
    cJSON *item = cJSON_CreateString(form);
    if (item == NULL || !cJSON_AddItemToArray(forms, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

static cJSON *ambiguous_answer(const match *matches, size_t count) {
    cJSON *answer = cJSON_CreateObject();
    cJSON *forms = answer == NULL ? NULL : cJSON_AddArrayToObject(answer, "ambiguous");
    for (size_t index = 0; forms != NULL && index < count; index++) {
        if (!adds_qualified_form(forms, &matches[index])) {
            forms = NULL;
        }
    }
    if (forms == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *answer_for(const match *matches, size_t count) {
    if (count == 0) {
        cJSON *answer = cJSON_CreateObject();
        if (answer != NULL && cJSON_AddTrueToObject(answer, "notFound") == NULL) {
            cJSON_Delete(answer);
            return NULL;
        }
        return answer;
    }
    return count == 1 ? found_answer(&matches[0]) : ambiguous_answer(matches, count);
}

static cJSON *look_up(const cJSON *argument, cJSON **refusal) {
    const cJSON *copies = cJSON_GetObjectItemCaseSensitive(argument, "copies");
    const char *name = text_of(argument, "name");
    if (!cJSON_IsArray(copies)) {
        return refuse(refusal, "send copies as an array of {source, copy}", "the argument carries no usable copies");
    }
    if (name == NULL || name[0] == '\0') {
        return refuse(refusal, "send name as a plugin name, or marketplace/name", "the argument carries no usable name");
    }
    wanted_name wanted = wanted_from(name);
    match *matches = NULL;
    size_t count = 0;
    cJSON *answer = NULL;
    if (finds_matches(copies, &wanted, &matches, &count, refusal)) {
        answer = answer_for(matches, count);
    }
    free(matches);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_marketplace_lookup(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, look_up);
}
