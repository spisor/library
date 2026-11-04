#include "windows_command.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_text.h"

#define SHELL_CHARACTERS "()[]%!^\"`<>&|;, *?"

int host_needs_command_shell(const char *binary) {
    size_t length = strlen(binary);
    if (length < 4 || binary[length - 4] != '.') {
        return 1;
    }
    char extension[4] = {(char)tolower((unsigned char)binary[length - 3]), (char)tolower((unsigned char)binary[length - 2]),
                         (char)tolower((unsigned char)binary[length - 1]), 0};
    return strcmp(extension, "exe") != 0 && strcmp(extension, "com") != 0;
}

static int add_shell_escaped(runtime_catalog_text *text, const char *value) {
    for (const char *character = value; *character != 0; character++) {
        if (strchr(SHELL_CHARACTERS, *character) != NULL && !runtime_catalog_text_add_character(text, '^')) {
            return 0;
        }
        if (!runtime_catalog_text_add_character(text, *character)) {
            return 0;
        }
    }
    return 1;
}

/**
 * value quoted as CommandLineToArgvW reads it, by cross-spawn's two rules: a quote and the one backslash
 * before it, if any, become backslash backslash backslash quote, and a backslash ending value is doubled.
 */
static char *quoted(const char *value) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    int built = runtime_catalog_text_add_character(&text, '"');
    size_t length = strlen(value);
    for (size_t index = 0; built && index < length; index++) {
        if (value[index] == '\\' && value[index + 1] == '"') {
            built = runtime_catalog_text_add(&text, "\\\\\\\"");
            index++;
        } else if (value[index] == '"') {
            built = runtime_catalog_text_add(&text, "\\\"");
        } else if (value[index] == '\\' && index + 1 == length) {
            built = runtime_catalog_text_add(&text, "\\\\");
        } else {
            built = runtime_catalog_text_add_character(&text, value[index]);
        }
    }
    if (!built || !runtime_catalog_text_add_character(&text, '"')) {
        runtime_catalog_text_discard(&text);
        return NULL;
    }
    return runtime_catalog_text_finish(&text);
}

static int add_argument(runtime_catalog_text *text, const char *value) {
    char *once = quoted(value);
    runtime_catalog_text escaped;
    runtime_catalog_text_start(&escaped);
    int built = once != NULL && add_shell_escaped(&escaped, once);
    char *escaped_once = built ? runtime_catalog_text_finish(&escaped) : NULL;
    if (!built) {
        runtime_catalog_text_discard(&escaped);
    }
    built = escaped_once != NULL && runtime_catalog_text_add_character(text, ' ') && add_shell_escaped(text, escaped_once);
    free(escaped_once);
    free(once);
    return built;
}

char *host_command_shell_line(const char *binary, const cJSON *arguments) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    int built = runtime_catalog_text_add_character(&text, '"') && add_shell_escaped(&text, binary);
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; built && argument != NULL;
         argument = argument->next) {
        built = cJSON_IsString(argument) && add_argument(&text, argument->valuestring);
    }
    if (!built || !runtime_catalog_text_add_character(&text, '"')) {
        runtime_catalog_text_discard(&text);
        return NULL;
    }
    return runtime_catalog_text_finish(&text);
}
