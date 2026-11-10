#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "catalog_time.h"
#include "cache_refusal.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_options.h"
#include "host_output.h"
#include "host_service.h"
#include "json_service.h"
#include "marketplace_build.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

static const char *lookup_api_base = NULL;

void host_marketplace_aim_lookups(const char *api_base) {
    lookup_api_base = api_base;
}

static cJSON *refusal_naming(const char *first, const char *second, const char *fix) {
    const char *parts[2] = {first, second};
    return runtime_cache_refusal(parts, 2, fix);
}

static cJSON *read_list(const char *path, cJSON **refusal) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    int was_read = bytes != NULL;
    cJSON *list = was_read ? spisor_parse_json(bytes, length) : NULL;
    free(bytes);
    if (list == NULL) {
        *refusal = refusal_naming(was_read ? "the list is not JSON: " : "the list could not be read: ", path,
                                  "check that --list names a readable marketplace list");
    }
    return list;
}

/** The current UTC time as RFC 3339, to the second, in text; 0 when it cannot be formatted. */
static int current_time(char text[RUNTIME_CATALOG_TIME_SIZE]) {
    return runtime_catalog_format_time(1000.0 * (double)time(NULL), text);
}

static cJSON *build_argument(cJSON *list, const char *built) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || cJSON_AddStringToObject(argument, "built", built) == NULL ||
        (lookup_api_base != NULL && cJSON_AddStringToObject(argument, "apiBase", lookup_api_base) == NULL) ||
        !cJSON_AddItemToObject(argument, "list", list)) {
        cJSON_Delete(argument);
        cJSON_Delete(list);
        return NULL;
    }
    return argument;
}

static cJSON *built_copy(cJSON *list, cJSON **refusal) {
    char built[RUNTIME_CATALOG_TIME_SIZE];
    if (!current_time(built)) {
        cJSON_Delete(list);
        *refusal = runtime_refusal("the current time could not be read", "check the machine's clock");
        return NULL;
    }
    cJSON *answer = host_call(runtime_marketplace_build, build_argument(list, built), refusal);
    cJSON *copy = answer == NULL ? NULL : cJSON_DetachItemFromObjectCaseSensitive(answer, "copy");
    cJSON_Delete(answer);
    return copy;
}

static int write_copy(const cJSON *copy, const char *path) {
    char *pretty = spisor_json_stringify_pretty(copy);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    const char *parent_segments[2] = {path, ".."};
    char *parent = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, parent_segments, 2);
    int written = text != NULL && parent != NULL && spisor_make_directories(parent) == 1 &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1;
    free(parent);
    free(text);
    free(pretty);
    return written;
}

static int print_wrote(const char *path, const cJSON *copy) {
    char *safe_path = host_printable(path);
    char count[32];
    snprintf(count, sizeof count, " listing %d entries\n",
             cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(copy, "entries")));
    host_write(0, "wrote ");
    host_write(0, safe_path == NULL ? "" : safe_path);
    host_write(0, count);
    free(safe_path);
    return HOST_EXIT_DONE;
}

/** The refusal with every name in it escaped for the terminal, so a name the host answered cannot move the cursor. */
static int refuse_printably(cJSON *refusal) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(refusal, "fix");
    char *safe_message = cJSON_IsString(message) ? host_printable(message->valuestring) : NULL;
    char *safe_fix = cJSON_IsString(fix) ? host_printable(fix->valuestring) : NULL;
    cJSON *safe = safe_message == NULL || safe_fix == NULL ? NULL : runtime_refusal(safe_message, safe_fix);
    int code = host_refuse(safe);
    cJSON_Delete(refusal);
    free(safe_message);
    free(safe_fix);
    return code;
}

static int build_into(const char *list_path, const char *out_path) {
    cJSON *refusal = NULL;
    char *absolute = host_absolute_path(out_path);
    cJSON *list = read_list(list_path, &refusal);
    cJSON *copy = list == NULL ? NULL : built_copy(list, &refusal);
    int code = HOST_EXIT_DONE;
    if (copy != NULL && !write_copy(copy, absolute == NULL ? out_path : absolute)) {
        refusal = refusal_naming("the copy could not be written to ", out_path, "check that --out names a writable file");
        cJSON_Delete(copy);
        copy = NULL;
    }
    if (copy == NULL) {
        code = refuse_printably(refusal);
    } else {
        code = print_wrote(out_path, copy);
        cJSON_Delete(copy);
    }
    free(absolute);
    return code;
}

/** marketplace build: the generated copy of the marketplace list at --list, written to --out. */
int host_command_marketplace_build(int count, char **arguments) {
    host_option options[2] = {{"list", NULL}, {"out", NULL}};
    char *problem = NULL;
    int code = HOST_EXIT_DONE;
    if (!host_parse_options(count, arguments, options, 2, NULL, &problem)) {
        code = host_misuse(problem == NULL ? "forebay-host ran out of memory" : problem);
    } else if (host_last_value(&options[0]) == NULL || host_last_value(&options[1]) == NULL) {
        code = host_misuse("--list <path> and --out <path> are both required");
    } else {
        code = build_into(host_last_value(&options[0]), host_last_value(&options[1]));
    }
    free(problem);
    host_free_options(options, 2);
    return code;
}
