#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "catalog_time.h"
#include "allocation.h"
#include "cache_refusal.h"
#include "definition_support.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_options.h"
#include "host_output.h"
#include "host_service.h"
#include "identity_naula.h"
#include "json_service.h"
#include "host_definition.h"
#include "spisor/json_value.h"
#include "runtime/marketplace.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

static cJSON *refusal_naming(const char *first, const char *second, const char *fix) {
    const char *parts[2] = {first, second};
    return runtime_cache_refusal(parts, 2, fix);
}

static cJSON *read_list(const char *path, cJSON **refusal) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    int was_read = bytes != NULL;
    int out_of_memory = 0;
    cJSON *list = was_read ? spisor_parse_json_reporting(bytes, length, &out_of_memory) : NULL;
    spisor_memory_release(bytes);
    if (list == NULL && outcome != SPISOR_READ_OUT_OF_MEMORY && !out_of_memory) {
        *refusal = refusal_naming(was_read ? "the list is not JSON: " : "the list could not be read: ", path,
                                  "check that --list names a readable marketplace list");
    }
    return list;
}

/** The current UTC time as RFC 3339, to the second, in text; 0 when it cannot be formatted. */
static int current_time(char text[RUNTIME_CATALOG_TIME_SIZE]) {
    return runtime_catalog_format_time(1000.0 * (double)time(NULL), text);
}

/** What marketplace.build is asked: the list, the copy it replaces when there is one, and the token with the host it is for. All are borrowed. */
typedef struct build_inputs {
    const cJSON *list;
    const cJSON *previous;
    const char *token;
    const char *token_host;
} build_inputs;

/** The entries a build left out, as {id, reason} rows; NULL when memory runs out. */
static cJSON *skipped_rows(const runtime_marketplace_build_answer *answer) {
    cJSON *rows = cJSON_CreateArray();
    for (size_t index = 0; rows != NULL && index < answer->skipped_count; index++) {
        cJSON *row = cJSON_CreateObject();
        if (row == NULL || cJSON_AddStringToObject(row, "id", answer->skipped[index].id) == NULL ||
            cJSON_AddStringToObject(row, "reason", answer->skipped[index].reason) == NULL ||
            !cJSON_AddItemToArray(rows, row)) {
            cJSON_Delete(row);
            cJSON_Delete(rows);
            rows = NULL;
        }
    }
    return rows;
}

/** The copy marketplace's build answered, as JSON with *skipped its rows; NULL when memory runs out. */
static cJSON *copy_of(const runtime_marketplace_build_answer *answer, cJSON **skipped) {
    cJSON *copy = NULL;
    *skipped = skipped_rows(answer);
    if (*skipped != NULL && spisor_json_of_value(answer->copy, &copy) != SPISOR_OK) {
        cJSON_Delete(*skipped);
        *skipped = NULL;
    }
    return copy;
}

/** Builds through marketplace's definition from the inputs, which hold the list and the copy it replaces as JSON. */
static cJSON *copy_built(const build_inputs *inputs, const char *built, cJSON **skipped, cJSON **refusal) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_marketplace_build_argument argument = SPISOR_INIT(runtime_marketplace_build_argument);
    runtime_marketplace_build_answer answer = SPISOR_INIT(runtime_marketplace_build_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    spisor_value_tree list = SPISOR_INIT(spisor_value_tree);
    spisor_value_tree previous = SPISOR_INIT(spisor_value_tree);
    spisor_status status = spisor_value_of_json(inputs->list, &list);
    if (status == SPISOR_OK && inputs->previous != NULL) {
        status = spisor_value_of_json(inputs->previous, &previous);
    }
    argument.list = &list.root;
    argument.built = built;
    argument.previous = inputs->previous == NULL ? NULL : &previous.root;
    argument.token = inputs->token;
    argument.token_host = inputs->token_host;
    if (status == SPISOR_OK) {
        status = marketplace->build(marketplace->context, &argument, &answer, &declined);
        *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    }
    cJSON *copy = status == SPISOR_OK ? copy_of(&answer, skipped) : NULL;
    marketplace->release_build_answer(marketplace->context, &answer);
    marketplace->release_refusal(marketplace->context, &declined);
    spisor_value_release(&previous);
    spisor_value_release(&list);
    return copy;
}

static cJSON *built_copy(const build_inputs *inputs, cJSON **skipped, cJSON **refusal) {
    char built[RUNTIME_CATALOG_TIME_SIZE];
    if (!current_time(built)) {
        *refusal = runtime_refusal("the current time could not be read", "check the machine's clock");
        return NULL;
    }
    *refusal = NULL;
    return copy_built(inputs, built, skipped, refusal);
}

static int write_copy(const cJSON *copy, const char *path) {
    char *pretty = spisor_json_stringify_pretty(copy);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    const char *parent_segments[2] = {path, ".."};
    char *parent = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, parent_segments, 2);
    int written = text != NULL && parent != NULL && spisor_make_directories(parent) == SPISOR_OK &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK;
    spisor_memory_release(parent);
    spisor_memory_release(text);
    spisor_memory_release(pretty);
    return written;
}

static void print_skipped(const cJSON *skipped) {
    const cJSON *row;
    cJSON_ArrayForEach(row, skipped) {
        char *id = host_printable(host_text(row, "id"));
        char *reason = host_printable(host_text(row, "reason"));
        host_write(1, "skipped ");
        host_write(1, id == NULL ? "" : id);
        host_write(1, ": ");
        host_write(1, reason == NULL ? "" : reason);
        host_write(1, "\n");
        spisor_memory_release(reason);
        spisor_memory_release(id);
    }
}

static int print_wrote(const char *path, const cJSON *copy) {
    char *safe_path = host_printable(path);
    char count[32];
    snprintf(count, sizeof count, " listing %d entries\n",
             cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(copy, "entries")));
    host_write(0, "wrote ");
    host_write(0, safe_path == NULL ? "" : safe_path);
    host_write(0, count);
    spisor_memory_release(safe_path);
    return HOST_EXIT_DONE;
}

/** Whether both paths name one file as written, made absolute and normalised first; a link to it is not seen through. */
static int is_same_path(const char *left, const char *right) {
    char *left_absolute = host_absolute_path(left);
    char *right_absolute = host_absolute_path(right);
    const char *left_segments[1] = {left_absolute};
    const char *right_segments[1] = {right_absolute};
    char *left_normal = left_absolute == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, left_segments, 1);
    char *right_normal = right_absolute == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, right_segments, 1);
    int is_same = left_normal != NULL && right_normal != NULL &&
                  (RUNTIME_HOST_IS_WINDOWS ? host_equals_ignoring_case(left_normal, right_normal) : strcmp(left_normal, right_normal) == 0);
    spisor_memory_release(right_normal);
    spisor_memory_release(left_normal);
    spisor_memory_release(right_absolute);
    spisor_memory_release(left_absolute);
    return is_same;
}

/** The value of the environment variable the user named, or NULL with *refusal set; the value is never printed. */
static const char *token_from(const char *variable, cJSON **refusal) {
    const char *token = variable == NULL ? NULL : getenv(variable);
    if (variable != NULL && (token == NULL || token[0] == '\0')) {
        *refusal = refusal_naming("--token-env names a variable that is not set or is empty: ", variable,
                                  "set the variable to an access token, or leave --token-env out");
        return NULL;
    }
    if (variable != NULL && !runtime_identity_is_token_text(token)) {
        *refusal = refusal_naming("--token-env names a variable that holds no usable token: ", variable,
                                  "set the variable to the access token itself, on one line with no spaces");
        return NULL;
    }
    return token;
}

/**
 * The copy already at path, or NULL when there is none, with *out_of_memory set when memory ran out reading it, or
 * with *refusal set when it is not JSON.
 */
static cJSON *read_previous(const char *path, int *out_of_memory, cJSON **refusal) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    cJSON *previous = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, out_of_memory);
    *out_of_memory = *out_of_memory || outcome == SPISOR_READ_OUT_OF_MEMORY;
    if (bytes != NULL && previous == NULL && !*out_of_memory) {
        *refusal = refusal_naming("the copy already at --out is not JSON: ", path,
                                  "remove it, or point --out at a path that holds a generated copy or nothing");
    }
    spisor_memory_release(bytes);
    return previous;
}

typedef struct build_paths {
    const char *list;
    const char *out;
    const char *token_variable;
    const char *token_host;
} build_paths;

static int write_or_refuse(const cJSON *copy, const char *out_path, cJSON **refusal) {
    char *absolute = host_absolute_path(out_path);
    int written = write_copy(copy, absolute == NULL ? out_path : absolute);
    if (!written) {
        *refusal = refusal_naming("the copy could not be written to ", out_path, "check that --out names a writable file");
    }
    spisor_memory_release(absolute);
    return written;
}

/** The copy built from the paths' files, written to --out, or NULL with *refusal set; *skipped is what was left out. */
static cJSON *built_and_written(const build_paths *paths, cJSON **skipped, cJSON **refusal) {
    char *absolute = host_absolute_path(paths->out);
    build_inputs inputs = {NULL, NULL, NULL, NULL};
    inputs.token = token_from(paths->token_variable, refusal);
    inputs.token_host = paths->token_host;
    int has_token = paths->token_variable == NULL || inputs.token != NULL;
    cJSON *list = has_token ? read_list(paths->list, refusal) : NULL;
    int out_of_memory = 0;
    cJSON *previous =
        list == NULL ? NULL : read_previous(absolute == NULL ? paths->out : absolute, &out_of_memory, refusal);
    inputs.list = list;
    inputs.previous = previous;
    cJSON *copy = list == NULL || out_of_memory || *refusal != NULL ? NULL : built_copy(&inputs, skipped, refusal);
    if (copy != NULL && !write_or_refuse(copy, paths->out, refusal)) {
        cJSON_Delete(copy);
        copy = NULL;
    }
    cJSON_Delete(previous);
    cJSON_Delete(list);
    spisor_memory_release(absolute);
    return copy;
}

static int build_into(const build_paths *paths) {
    cJSON *refusal = NULL;
    cJSON *skipped = NULL;
    cJSON *copy = NULL;
    if (is_same_path(paths->list, paths->out)) {
        refusal = refusal_naming("--out is the list itself: ", paths->out,
                                 "write the copy to a different path, so the hand-written list is kept");
    } else {
        copy = built_and_written(paths, &skipped, &refusal);
    }
    int code = HOST_EXIT_DONE;
    if (copy == NULL) {
        code = host_refuse_printably(refusal);
    } else {
        code = print_wrote(paths->out, copy);
        print_skipped(skipped);
        cJSON_Delete(copy);
    }
    cJSON_Delete(skipped);
    return code;
}

/** marketplace build: the generated copy of the marketplace list at --list, written to --out. */
int host_command_marketplace_build(int count, char **arguments) {
    host_option options[4] = {{"list", NULL}, {"out", NULL}, {"token-env", NULL}, {"token-host", NULL}};
    char *problem = NULL;
    int code = HOST_EXIT_DONE;
    if (!host_parse_options(count, arguments, options, 4, NULL, &problem)) {
        code = host_misuse(problem == NULL ? "spisor-host ran out of memory" : problem);
    } else if (host_last_value(&options[0]) == NULL || host_last_value(&options[1]) == NULL) {
        code = host_misuse("--list <path> and --out <path> are both required");
    } else if (host_last_value(&options[2]) != NULL && host_last_value(&options[3]) == NULL) {
        code = host_misuse("--token-env needs --token-host <host>: github.com for places on GitHub, or the instance host");
    } else {
        build_paths paths = {host_last_value(&options[0]), host_last_value(&options[1]), host_last_value(&options[2]),
                             host_last_value(&options[3])};
        code = build_into(&paths);
    }
    spisor_memory_release(problem);
    host_free_options(options, 4);
    return code;
}
