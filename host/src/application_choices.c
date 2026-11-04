#include "application_choices.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_text.h"

#define SKIP "skip"
#define MOST_ATTEMPTS 3

char *host_method_question(const char *id, const cJSON *methods) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    int built = runtime_catalog_text_add(&text, id) &&
                runtime_catalog_text_add(&text, " is not installed and was not found. Install it through:\n");
    int number = 0;
    char line[64];
    for (const cJSON *method = methods == NULL ? NULL : methods->child; built && method != NULL; method = method->next) {
        snprintf(line, sizeof line, "  %d) ", ++number);
        built = cJSON_IsString(method) && runtime_catalog_text_add(&text, line) &&
                runtime_catalog_text_add(&text, method->valuestring) && runtime_catalog_text_add(&text, "\n");
    }
    snprintf(line, sizeof line, "  %d) " SKIP "\nChoose 1-%d: ", number + 1, number + 1);
    if (!built || !runtime_catalog_text_add(&text, line)) {
        runtime_catalog_text_discard(&text);
        return NULL;
    }
    return runtime_catalog_text_finish(&text);
}

/** The method answer picks among methods and skip, or NULL when it picks none. */
static const char *method_of(const char *answer, const cJSON *methods) {
    char typed[64];
    size_t length = 0;
    for (; answer[length] != 0 && length + 1 < sizeof typed; length++) {
        typed[length] = (char)tolower((unsigned char)answer[length]);
    }
    typed[length] = 0;
    if (typed[0] == 0 || strcmp(typed, SKIP) == 0) {
        return SKIP;
    }
    char *end = NULL;
    long number = strtol(typed, &end, 10);
    int count = cJSON_GetArraySize(methods);
    if (end != typed && *end == 0) {
        return number >= 1 && number <= count ? cJSON_GetArrayItem(methods, (int)number - 1)->valuestring
                                              : number == count + 1 ? SKIP : NULL;
    }
    for (const cJSON *method = methods->child; method != NULL; method = method->next) {
        if (strcmp(method->valuestring, typed) == 0) {
            return method->valuestring;
        }
    }
    return NULL;
}

/** The method chosen for one application; NULL when ask could ask nothing, with *can_ask cleared. */
static const char *ask_for_method(const char *id, const cJSON *methods, host_question ask, void *context, int *can_ask) {
    char *question = host_method_question(id, methods);
    const char *chosen = NULL;
    for (int attempt = 0; question != NULL && chosen == NULL && attempt < MOST_ATTEMPTS; attempt++) {
        char *answer = ask(question, context);
        if (answer == NULL) {
            *can_ask = 0;
            break;
        }
        chosen = method_of(answer, methods);
        free(answer);
    }
    free(question);
    return chosen != NULL || !*can_ask ? chosen : SKIP;
}

cJSON *host_choose_methods(const cJSON *missing, host_question ask, void *context) {
    cJSON *choices = cJSON_CreateObject();
    int can_ask = 1;
    for (const cJSON *row = choices == NULL ? NULL : missing->child; row != NULL && can_ask; row = row->next) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "application");
        const cJSON *methods = cJSON_GetObjectItemCaseSensitive(row, "methods");
        if (!cJSON_IsString(id) || cJSON_GetArraySize(methods) == 0) {
            continue;
        }
        const char *chosen = ask_for_method(id->valuestring, methods, ask, context, &can_ask);
        if (chosen != NULL && cJSON_AddStringToObject(choices, id->valuestring, chosen) == NULL) {
            cJSON_Delete(choices);
            return NULL;
        }
    }
    return choices;
}
