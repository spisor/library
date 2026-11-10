#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_commands.h"
#include "host_output.h"

#if defined(_WIN32)
#include "host_arguments.h"
#endif

typedef int (*host_command)(int count, char **arguments);

typedef struct named_command {
    const char *name;
    host_command run;
} named_command;

static const named_command COMMANDS[] = {
    {"install", host_command_install}, {"status", host_command_status},  {"update", host_command_update},
    {"remove", host_command_remove},   {"launch", host_command_launch},  {"hook", host_command_hook},
    {"background", host_command_background}, {"action", host_command_action},
};

static int unknown_command(int count, char **arguments) {
    size_t length = 32;
    for (int index = 0; index < count; index++) {
        length += strlen(arguments[index]) + 1;
    }
    char *message = malloc(length);
    if (message == NULL) {
        return host_misuse("unknown command");
    }
    strcpy(message, "unknown command: ");
    for (int index = 0; index < count; index++) {
        strcat(message, arguments[index]);
        strcat(message, index + 1 < count ? " " : "");
    }
    if (count == 0) {
        strcat(message, "(none)");
    }
    int code = host_misuse(message);
    free(message);
    return code;
}

int host_run_command_line(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[0], "catalog") == 0 && strcmp(arguments[1], "build") == 0) {
        return host_command_catalog_build(count - 2, arguments + 2);
    }
    if (count >= 2 && strcmp(arguments[0], "marketplace") == 0 && strcmp(arguments[1], "build") == 0) {
        return host_command_marketplace_build(count - 2, arguments + 2);
    }
    for (size_t index = 0; count >= 1 && index < sizeof COMMANDS / sizeof COMMANDS[0]; index++) {
        if (strcmp(arguments[0], COMMANDS[index].name) == 0) {
            return COMMANDS[index].run(count - 1, arguments + 1);
        }
    }
    return unknown_command(count, arguments);
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = host_utf8_arguments(count, wide);
    if (arguments == NULL) {
        return host_misuse("an argument is not valid text");
    }
    int code = host_run_command_line(count - 1, arguments + 1);
    host_free_arguments(arguments, count);
    return code;
}

#else

int main(int count, char **arguments) {
    return host_run_command_line(count - 1, arguments + 1);
}

#endif
