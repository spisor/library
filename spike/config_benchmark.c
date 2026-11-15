#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "measure.h"
#include "runtime/config.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "value_tree.h"

#if defined(_WIN32)
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

static const spisor_value colors[] = {
    {SPISOR_VALUE_TEXT, 0, 0, "#101010", 7, NULL, NULL, 0},
    {SPISOR_VALUE_TEXT, 0, 0, "#f0f0f0", 7, NULL, NULL, 0},
    {SPISOR_VALUE_TEXT, 0, 0, "#3366ff", 7, NULL, NULL, 0},
};
static const spisor_entry left_panel[] = {
    {"width", 5, {SPISOR_VALUE_NUMBER, 0, 240, NULL, 0, NULL, NULL, 0}},
    {"visible", 7, {SPISOR_VALUE_BOOLEAN, 1, 0, NULL, 0, NULL, NULL, 0}},
};
static const spisor_entry panels[] = {
    {"left", 4, {SPISOR_VALUE_RECORD, 0, 0, NULL, 0, NULL, left_panel, 2}},
    {"right", 5, {SPISOR_VALUE_NULL, 0, 0, NULL, 0, NULL, NULL, 0}},
};
static const spisor_entry layout[] = {
    {"theme", 5, {SPISOR_VALUE_TEXT, 0, 0, "dark", 4, NULL, NULL, 0}},
    {"fontSize", 8, {SPISOR_VALUE_NUMBER, 0, 13, NULL, 0, NULL, NULL, 0}},
    {"ligatures", 9, {SPISOR_VALUE_BOOLEAN, 1, 0, NULL, 0, NULL, NULL, 0}},
    {"colors", 6, {SPISOR_VALUE_LIST, 0, 0, NULL, 0, colors, NULL, 3}},
    {"panels", 6, {SPISOR_VALUE_RECORD, 0, 0, NULL, 0, NULL, panels, 2}},
};
static const spisor_value layout_value = {SPISOR_VALUE_RECORD, 0, 0, NULL, 0, NULL, layout, 5};

#define PLUGIN "spike-plugin"
#define KEY "editor.layout"

static char home[1024];
static char *set_text;
static size_t set_length;
static int failures = 0;

static void fail(const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static cJSON *tree_of(const spisor_value *value) {
    switch (value->kind) {
    case SPISOR_VALUE_NULL:
        return cJSON_CreateNull();
    case SPISOR_VALUE_BOOLEAN:
        return cJSON_CreateBool(value->boolean);
    case SPISOR_VALUE_NUMBER:
        return cJSON_CreateNumber(value->number);
    case SPISOR_VALUE_TEXT:
        return cJSON_CreateString(value->text);
    case SPISOR_VALUE_LIST: {
        cJSON *list = cJSON_CreateArray();
        for (size_t index = 0; index < value->count; index++) {
            cJSON_AddItemToArray(list, tree_of(&value->items[index]));
        }
        return list;
    }
    case SPISOR_VALUE_RECORD: {
        cJSON *record = cJSON_CreateObject();
        for (size_t index = 0; index < value->count; index++) {
            cJSON_AddItemToObject(record, value->entries[index].key, tree_of(&value->entries[index].value));
        }
        return record;
    }
    default:
        return NULL;
    }
}

static char *set_text_of(const spisor_value *value) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "home", home);
    cJSON_AddStringToObject(request, "plugin", PLUGIN);
    cJSON_AddStringToObject(request, "key", KEY);
    cJSON_AddItemToObject(request, "value", tree_of(value));
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    return text;
}

static runtime_config_set_argument set_argument(void) {
    runtime_config_set_argument argument;
    memset(&argument, 0, sizeof argument);
    argument.size = sizeof argument;
    argument.home = home;
    argument.plugin = PLUGIN;
    argument.key = KEY;
    argument.value = &layout_value;
    return argument;
}

static cJSON *json_set(const char *text, size_t length) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    int32_t status = runtime_config_set((const uint8_t *)text, length, &out, &out_len);
    cJSON *answer = status == 0 ? spisor_parse_json(out, out_len) : NULL;
    spisor_release(out);
    return answer;
}

static void check(void) {
    const runtime_config_definition *config = runtime_config_definition_of();
    cJSON *first = json_set(set_text, set_length);
    cJSON *second = json_set(set_text, set_length);
    runtime_config_set_argument argument = set_argument();
    runtime_config_set_answer answer = {.size = sizeof answer};
    spisor_status status = config->set(config->context, &argument, &answer, NULL);
    cJSON *wanted = tree_of(&layout_value);
    cJSON *answered = status == SPISOR_OK ? tree_of(&answer.previous) : NULL;
    if (first == NULL || cJSON_GetObjectItemCaseSensitive(first, "previous") != NULL) {
        fail("the first JSON set answers a previous value");
    }
    if (!cJSON_Compare(cJSON_GetObjectItemCaseSensitive(second, "previous"), wanted, 1)) {
        fail("the second JSON set does not answer the value");
    }
    if (status != SPISOR_OK || !cJSON_Compare(answered, wanted, 1)) {
        fail("the struct set does not answer the value");
    }
    config->release_set(config->context, &answer);
    runtime_config_set_argument bad = set_argument();
    spisor_value infinite = {SPISOR_VALUE_NUMBER, 0, 0, NULL, 0, NULL, NULL, 0};
    infinite.number = HUGE_VAL;
    bad.value = &infinite;
    spisor_refusal refusal = {.size = sizeof refusal};
    runtime_config_set_answer refused = {.size = sizeof refused};
    if (config->set(config->context, &bad, &refused, &refusal) != SPISOR_MALFORMED || refusal.detail == NULL) {
        fail("an infinite number is not refused as malformed");
    }
    config->release_refusal(config->context, &refusal);
    cJSON_Delete(first);
    cJSON_Delete(second);
    cJSON_Delete(wanted);
    cJSON_Delete(answered);
}

static int set_json(void *data) {
    (void)data;
    uint8_t *out = NULL;
    size_t out_len = 0;
    int32_t status = runtime_config_set((const uint8_t *)set_text, set_length, &out, &out_len);
    spisor_release(out);
    return status == 0;
}

static int set_json_round_trip(void *data) {
    (void)data;
    char *text = set_text_of(&layout_value);
    cJSON *answer = json_set(text, strlen(text));
    int answered = answer != NULL && cJSON_GetObjectItemCaseSensitive(answer, "previous") != NULL;
    cJSON_Delete(answer);
    cJSON_free(text);
    return answered;
}

static int set_struct(void *data) {
    (void)data;
    const runtime_config_definition *config = runtime_config_definition_of();
    runtime_config_set_argument argument = set_argument();
    runtime_config_set_answer answer = {.size = sizeof answer};
    int answered = config->set(config->context, &argument, &answer, NULL) == SPISOR_OK &&
                   answer.previous.kind == SPISOR_VALUE_RECORD;
    config->release_set(config->context, &answer);
    return answered;
}

static cJSON *previous_tree;

/** What the JSON boundary alone costs: the argument read from text, the answer printed to text. */
static int boundary_json(void *data) {
    (void)data;
    cJSON *argument = spisor_parse_json((const uint8_t *)set_text, set_length);
    cJSON *answer = cJSON_CreateObject();
    cJSON_AddItemReferenceToObject(answer, "previous", previous_tree);
    char *printed = cJSON_PrintUnformatted(answer);
    int done = argument != NULL && printed != NULL;
    cJSON_Delete(argument);
    cJSON_Delete(answer);
    cJSON_free(printed);
    return done;
}

/**
 * What the struct boundary alone costs: the value tree made the cJSON tree the settings file needs, and the answer's
 * tree made a value tree, over a copy, since the definition takes the tree it answers from.
 */
static int boundary_struct(void *data) {
    (void)data;
    int malformed = 0;
    cJSON *value = runtime_tree_of_value(&layout_value, &malformed);
    spisor_value previous;
    runtime_value_storage *storage = runtime_value_of_tree(cJSON_Duplicate(previous_tree, 1), &previous);
    int done = value != NULL && storage != NULL;
    cJSON_Delete(value);
    runtime_value_storage_free(storage);
    return done;
}

static int measure(void) {
    previous_tree = tree_of(&layout_value);
    spike_path boundary[] = {
        {"json", boundary_json, NULL},
        {"json-again", boundary_json, NULL},
        {"struct", boundary_struct, NULL},
    };
    int measured = spike_measure("config.set boundary only (no file)", boundary, 3);
    cJSON_Delete(previous_tree);
    spike_path paths[] = {
        {"json", set_json, NULL},
        {"json-again", set_json, NULL},
        {"struct-with-value-tree", set_struct, NULL},
        {"json-with-encode-decode", set_json_round_trip, NULL},
    };
    return spike_measure("config.set (a five-member record, file written each call)", paths, 4) && measured;
}

int main(int argc, char **argv) {
    snprintf(home, sizeof home, "%s/config-spike-%d", SPIKE_SCRATCH_PATH, (int)CURRENT_PROCESS_ID);
    if (!spisor_make_directories(home)) {
        fail("the scratch home cannot be made");
        return 1;
    }
    set_text = set_text_of(&layout_value);
    set_length = strlen(set_text);
    check();
    if (failures == 0 && argc > 1 && strcmp(argv[1], "measure") == 0 && !measure()) {
        fail("a measured call failed");
    }
    cJSON_free(set_text);
    if (failures == 0) {
        printf("config definition spike: %s\n", argc > 1 ? "checked and measured" : "checked");
    }
    return failures == 0 ? 0 : 1;
}
