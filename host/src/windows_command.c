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

/** Adds count backslashes; 0 when memory runs out. */
static int add_backslashes(runtime_catalog_text *text, size_t count) {
    for (size_t added = 0; added < count; added++) {
        if (!runtime_catalog_text_add_character(text, '\\')) {
            return 0;
        }
    }
    return 1;
}

/**
 * value quoted as CommandLineToArgvW reads it back: a run of backslashes before a quote is doubled and the
 * quote escaped, and a run ending value is doubled, since the closing quote follows it.
 */
static char *quoted(const char *value) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    int built = runtime_catalog_text_add_character(&text, '"');
    for (const char *at = value; built && *at != 0;) {
        size_t run = 0;
        while (at[run] == '\\') {
            run++;
        }
        if (at[run] == '"') {
            built = add_backslashes(&text, 2 * run + 1) && runtime_catalog_text_add_character(&text, '"');
            at += run + 1;
        } else if (at[run] == 0) {
            built = add_backslashes(&text, 2 * run);
            at += run;
        } else {
            built = add_backslashes(&text, run) && runtime_catalog_text_add_character(&text, at[run]);
            at += run + 1;
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
