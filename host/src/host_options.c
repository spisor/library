#include "host_options.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"

static char *problem_of(const char *first, const char *second) {
    const char *parts[2] = {first, second};
    return runtime_cache_text(parts, 2);
}

/** The option arguments names, ignoring an =value part; NULL for an option this command does not take. */
static host_option *option_named(const char *argument, host_option *options, size_t option_count, size_t *name_length) {
    const char *equals = strchr(argument, '=');
    *name_length = equals == NULL ? strlen(argument) : (size_t)(equals - argument);
    for (size_t index = 0; index < option_count; index++) {
        if (strlen(options[index].name) == *name_length && strncmp(argument, options[index].name, *name_length) == 0) {
            return &options[index];
        }
    }
    return NULL;
}

static int add_value(host_option *option, const char *value) {
    if (option->values == NULL) {
        option->values = cJSON_CreateArray();
    }
    cJSON *text = option->values == NULL ? NULL : cJSON_CreateString(value);
    if (text == NULL || !cJSON_AddItemToArray(option->values, text)) {
        cJSON_Delete(text);
        return 0;
    }
    return 1;
}

int host_parse_options(int count, char **arguments, host_option *options, size_t option_count, cJSON *positionals,
                       char **problem) {
    *problem = NULL;
    for (int index = 0; index < count; index++) {
        const char *argument = arguments[index];
        if (strncmp(argument, "--", 2) != 0 || strlen(argument) == 2) {
            if (positionals == NULL) {
                *problem = problem_of("unexpected argument ", argument);
                return 0;
            }
            if (!cJSON_AddItemToArray(positionals, cJSON_CreateString(argument))) {
                return 0;
            }
            continue;
        }
        size_t name_length = 0;
        host_option *option = option_named(argument + 2, options, option_count, &name_length);
        if (option == NULL) {
            *problem = problem_of("unknown option ", argument);
            return 0;
        }
        const char *value = argument[2 + name_length] == '=' ? argument + 3 + name_length : NULL;
        if (value == NULL && (index + 1 >= count || strncmp(arguments[index + 1], "--", 2) == 0)) {
            *problem = problem_of(argument, " needs a value");
            return 0;
        }
        if (!add_value(option, value != NULL ? value : arguments[++index])) {
            return 0;
        }
    }
    return 1;
}

void host_free_options(host_option *options, size_t option_count) {
    for (size_t index = 0; index < option_count; index++) {
        cJSON_Delete(options[index].values);
        options[index].values = NULL;
    }
}

const char *host_last_value(const host_option *option) {
    int size = cJSON_GetArraySize(option->values);
    const cJSON *last = size == 0 ? NULL : cJSON_GetArrayItem(option->values, size - 1);
    return cJSON_IsString(last) ? last->valuestring : NULL;
}
