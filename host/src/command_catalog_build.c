#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_build.h"
#include "host_commands.h"
#include "host_options.h"
#include "host_output.h"
#include "platform_key.h"

/** directory@platform split at its last @ when what follows is a platform key; else the whole is the directory. */
static char *split_platform(const char *value, const char **platform) {
    const char *at = strrchr(value, '@');
    *platform = at != NULL && at != value && runtime_is_platform_key(at + 1) ? at + 1 : NULL;
    size_t length = *platform == NULL ? strlen(value) : (size_t)(at - value);
    char *directory = malloc(length + 1);
    if (directory != NULL) {
        memcpy(directory, value, length);
        directory[length] = 0;
    }
    return directory;
}

/** Whether value is <id>=<directory>@<platform>, the directory everything up to the last @. */
static int is_library_argument(const char *value) {
    const char *equals = strchr(value, '=');
    const char *at = strrchr(value, '@');
    return equals != NULL && equals != value && at != NULL && at >= equals + 2 && at[1] != 0;
}

/** Reads one --library <id>=<directory>@<platform> is_library_argument accepted. */
static int read_library(catalog_inputs *inputs, const char *value, cJSON **refusal) {
    const char *equals = strchr(value, '=');
    const char *at = strrchr(value, '@');
    char *id = malloc((size_t)(equals - value) + 1);
    char *directory = malloc((size_t)(at - equals));
    int read = id != NULL && directory != NULL;
    if (read) {
        memcpy(id, value, (size_t)(equals - value));
        id[equals - value] = 0;
        memcpy(directory, equals + 1, (size_t)(at - equals - 1));
        directory[at - equals - 1] = 0;
        read = host_read_library_directory(inputs, id, directory, at + 1, refusal);
    }
    free(directory);
    free(id);
    return read;
}

static int read_inputs(catalog_inputs *inputs, const host_option *options, char **problem, cJSON **refusal) {
    const cJSON *value;
    cJSON_ArrayForEach(value, options[2].values) {
        if (!is_library_argument(value->valuestring)) {
            const char *parts[3] = {"--library ", value->valuestring, " is not <id>=<directory>@<platform>"};
            *problem = runtime_cache_text(parts, 3);
            return 0;
        }
    }
    cJSON_ArrayForEach(value, options[0].values) {
        if (!host_read_plugin_directory(inputs, value->valuestring, refusal)) {
            return 0;
        }
    }
    cJSON_ArrayForEach(value, options[1].values) {
        const char *platform = NULL;
        char *directory = split_platform(value->valuestring, &platform);
        int read = directory != NULL && host_read_host_directory(inputs, directory, platform, refusal);
        free(directory);
        if (!read) {
            return 0;
        }
    }
    cJSON_ArrayForEach(value, options[2].values) {
        if (!read_library(inputs, value->valuestring, refusal)) {
            return 0;
        }
    }
    return 1;
}

static int build(const host_option *options, const char *out) {
    catalog_inputs inputs = {NULL, 0, 0};
    char *problem = NULL;
    cJSON *refusal = NULL;
    char *path = read_inputs(&inputs, options, &problem, &refusal) ? host_build_local_catalog(&inputs, out, &refusal) : NULL;
    int code = HOST_EXIT_DONE;
    if (problem != NULL) {
        code = host_misuse(problem);
    } else if (path == NULL) {
        code = host_refuse(refusal);
        refusal = NULL;
    } else {
        char line[64];
        snprintf(line, sizeof line, " listing %zu build%s\n", inputs.count, inputs.count == 1 ? "" : "s");
        host_write(0, "wrote ");
        host_write(0, path);
        host_write(0, line);
    }
    cJSON_Delete(refusal);
    free(problem);
    free(path);
    host_free_catalog_inputs(&inputs);
    return code;
}

/** catalog build: a local catalog of the --plugin, --host and --library directories, written into --out. */
int host_command_catalog_build(int count, char **arguments) {
    host_option options[4] = {{"plugin", NULL}, {"host", NULL}, {"library", NULL}, {"out", NULL}};
    char *problem = NULL;
    int code = HOST_EXIT_DONE;
    if (!host_parse_options(count, arguments, options, 4, NULL, &problem)) {
        code = host_misuse(problem == NULL ? "forebay-host ran out of memory" : problem);
    } else if (host_last_value(&options[3]) == NULL) {
        code = host_misuse("--out <directory> is required");
    } else {
        code = build(options, host_last_value(&options[3]));
    }
    free(problem);
    host_free_options(options, 4);
    return code;
}
