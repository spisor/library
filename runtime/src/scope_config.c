#include <math.h>
#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "plugin_scope.h"
#include "spisor/definition.h"
#include "spisor/json_value.h"
#include "standard_refusal.h"

#define SETTINGS_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_settings, storage)
#define ANSWER_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_setting_answer, storage)

/**
 * One walk over a merged settings record: counting when `settings` is NULL, else filling them, each key built in
 * `path` and copied with each text into `texts`. The counting walk measures `longest_key` over every key it builds, a
 * record's as well as a leaf's, since the filling walk writes each into `path`.
 */
typedef struct flattening {
    weivel_standard_setting *settings;
    char *texts;
    char *path;
    size_t count;
    size_t text_bytes;
    size_t longest_key;
} flattening;

/**
 * Whether a member's name can be one segment of a dotted key. A name that is empty or holds a dot is skipped with
 * everything below it, never offered under a key: runtime's config reads a key by its dot-separated segments, so a get
 * could never read such a member back, and `all` offers only what `get` answers.
 */
static int is_key_segment(const char *name) {
    return name != NULL && name[0] != '\0' && strchr(name, '.') == NULL;
}

/** The standard kind a settings value is offered as, or NONE for a value not offered: a null, a list, a record. */
static weivel_standard_setting_kind kind_of(const cJSON *value) {
    if (cJSON_IsString(value)) {
        return WEIVEL_STANDARD_SETTING_TEXT;
    }
    if (cJSON_IsNumber(value) && isfinite(value->valuedouble)) {
        return WEIVEL_STANDARD_SETTING_NUMBER;
    }
    return cJSON_IsBool(value) ? WEIVEL_STANDARD_SETTING_BOOLEAN : WEIVEL_STANDARD_SETTING_NONE;
}

static char *copy_into(char **texts, const char *text, size_t length) {
    char *copy = *texts;
    memcpy(copy, text, length);
    copy[length] = '\0';
    *texts += length + 1;
    return copy;
}

/** A setting's value with its text copied into `texts`, whose room the counting walk measured. */
static weivel_standard_setting_value value_of(const cJSON *value, char **texts) {
    weivel_standard_setting_value filled = {WEIVEL_STANDARD_SETTING_NONE, 0, 0.0, NULL, 0};
    filled.kind = kind_of(value);
    if (filled.kind == WEIVEL_STANDARD_SETTING_TEXT) {
        filled.text_length = strlen(value->valuestring);
        filled.text = copy_into(texts, value->valuestring, filled.text_length);
    } else if (filled.kind == WEIVEL_STANDARD_SETTING_NUMBER) {
        filled.number = value->valuedouble;
    } else {
        filled.boolean = cJSON_IsTrue(value) ? 1 : 0;
    }
    return filled;
}

static void add_leaf(flattening *walk, const cJSON *value, size_t key_length) {
    if (walk->settings == NULL) {
        walk->text_bytes += key_length + 1 + (cJSON_IsString(value) ? strlen(value->valuestring) + 1 : 0);
    } else {
        weivel_standard_setting *setting = &walk->settings[walk->count];
        setting->key = copy_into(&walk->texts, walk->path, key_length);
        setting->value = value_of(value, &walk->texts);
    }
    walk->count++;
}

/** Walks a record whose dotted key so far is `prefix_length` bytes, the record at the top having none. */
static void flatten(flattening *walk, const cJSON *record, size_t prefix_length) {
    const cJSON *member;
    cJSON_ArrayForEach(member, record) {
        size_t name_length;
        size_t key_length;
        if (!is_key_segment(member->string) ||
            (!cJSON_IsObject(member) && kind_of(member) == WEIVEL_STANDARD_SETTING_NONE)) {
            continue;
        }
        name_length = strlen(member->string);
        key_length = prefix_length + (prefix_length == 0 ? 0 : 1) + name_length;
        if (walk->path == NULL) {
            walk->longest_key = key_length > walk->longest_key ? key_length : walk->longest_key;
        } else {
            if (prefix_length != 0) {
                walk->path[prefix_length] = '.';
            }
            memcpy(walk->path + key_length - name_length, member->string, name_length + 1);
        }
        if (cJSON_IsObject(member)) {
            flatten(walk, member, key_length);
        } else {
            add_leaf(walk, member, key_length);
        }
    }
}

/** Fills an empty answer with every setting `merged` offers, in one block its `storage` owns. */
static spisor_status fill_settings(const cJSON *merged, weivel_standard_settings *settings) {
    flattening walk = {NULL, NULL, NULL, 0, 0, 0};
    size_t array_bytes;
    flatten(&walk, merged, 0);
    if (walk.count == 0) {
        return SPISOR_OK;
    }
    array_bytes = walk.count * sizeof(weivel_standard_setting);
    walk.settings = spisor_memory_allocate(array_bytes + walk.text_bytes);
    walk.path = spisor_memory_allocate(walk.longest_key + 1);
    if (walk.settings == NULL || walk.path == NULL) {
        spisor_memory_release(walk.settings);
        spisor_memory_release(walk.path);
        return SPISOR_OUT_OF_MEMORY;
    }
    walk.texts = (char *)walk.settings + array_bytes;
    walk.count = 0;
    flatten(&walk, merged, 0);
    spisor_memory_release(walk.path);
    settings->settings = walk.settings;
    settings->count = walk.count;
    settings->storage = walk.settings;
    return SPISOR_OK;
}

/**
 * What runtime's config service answered, as the scope answers it: the call passed every check of its own before
 * runtime's, so a MALFORMED from runtime is what the host or the manifest holds, which the caller can only be refused.
 */
static spisor_status as_scope_answer(spisor_status status) {
    return status == SPISOR_MALFORMED ? SPISOR_REFUSED : status;
}

static weivel_status all(void *context, weivel_standard_settings *settings,
                         weivel_standard_service_refusal *refusal) {
    runtime_config_request request = runtime_plugin_scope_request(context);
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    cJSON *merged = NULL;
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (!SPISOR_IS_EMPTY_COUNTED_ANSWER(settings, SETTINGS_SHAPE, settings->count)) {
        return runtime_answer_service(spisor_refuse_shape(&filled, "config's all"), &filled, refusal);
    }
    status = as_scope_answer(runtime_config_all_values(&request, &merged, &filled));
    if (status == SPISOR_OK) {
        status = fill_settings(merged, settings);
    }
    cJSON_Delete(merged);
    if (status == SPISOR_OK) {
        runtime_zero_beyond(settings, sizeof *settings);
    }
    return runtime_answer_service(status, &filled, refusal);
}

static int is_empty_answer(const weivel_standard_setting_answer *answer) {
    return SPISOR_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE) && answer->value.kind == WEIVEL_STANDARD_SETTING_NONE;
}

/** Fills an empty answer with the value `found`, NONE for one the scope does not offer. */
static spisor_status fill_answer(const cJSON *found, weivel_standard_setting_answer *answer) {
    weivel_standard_setting_kind kind = kind_of(found);
    char *text;
    char *cursor;
    if (kind == WEIVEL_STANDARD_SETTING_NONE) {
        return SPISOR_NONE;
    }
    if (kind != WEIVEL_STANDARD_SETTING_TEXT) {
        answer->value = value_of(found, NULL);
        return SPISOR_OK;
    }
    text = spisor_memory_allocate(strlen(found->valuestring) + 1);
    if (text == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cursor = text;
    answer->value = value_of(found, &cursor);
    answer->storage = text;
    return SPISOR_OK;
}

static spisor_status refuse_missing_key(spisor_refusal *refusal, const char *operation) {
    char detail[64];
    snprintf(detail, sizeof detail, "config's %s was called without a key", operation);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, "pass the setting's key, such as \"a.b\"");
}

static weivel_status get(void *context, const char *key, weivel_standard_setting_answer *answer,
                         weivel_standard_service_refusal *refusal) {
    runtime_config_request request = runtime_plugin_scope_request(context);
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    cJSON *found = NULL;
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (!is_empty_answer(answer)) {
        return runtime_answer_service(spisor_refuse_shape(&filled, "config's get"), &filled, refusal);
    }
    if (key == NULL) {
        return runtime_answer_service(refuse_missing_key(&filled, "get"), &filled, refusal);
    }
    request.key = key;
    status = as_scope_answer(runtime_config_get_value(&request, &found, &filled));
    if (status == SPISOR_OK) {
        status = fill_answer(found, answer);
    }
    cJSON_Delete(found);
    if (status == SPISOR_OK) {
        runtime_zero_beyond(answer, sizeof *answer);
    }
    return runtime_answer_service(status, &filled, refusal);
}

static spisor_status refuse_value(spisor_refusal *refusal, spisor_status status, const char *problem) {
    char detail[160];
    snprintf(detail, sizeof detail, "config's set was given %s", problem);
    return spisor_refuse_as(refusal, status, detail,
                            "pass a value of kind text, number or boolean, as weivel/services.h defines each");
}

/** The value as the JSON runtime's config writes, or the refusal of one weivel/services.h does not allow. */
static spisor_status json_of_setting(const weivel_standard_setting_value *value, cJSON **json,
                                     spisor_refusal *refusal) {
    spisor_value text = {SPISOR_VALUE_TEXT, 0, 0.0, NULL, 0, NULL, NULL, 0};
    *json = NULL;
    switch (value->kind) {
    case WEIVEL_STANDARD_SETTING_TEXT:
        if (value->text == NULL) {
            return refuse_value(refusal, SPISOR_MALFORMED, "a text value without its text");
        }
        if (memchr(value->text, '\0', value->text_length) != NULL) {
            return refuse_value(refusal, SPISOR_REFUSED, "a text holding a NUL byte, which no settings file keeps");
        }
        text.text = value->text;
        text.length = value->text_length;
        if (spisor_json_of_value(&text, json) == SPISOR_MALFORMED) {
            return refuse_value(refusal, SPISOR_MALFORMED, "a text that is not well-formed UTF-8");
        }
        break;
    case WEIVEL_STANDARD_SETTING_NUMBER:
        if (!isfinite(value->number)) {
            return refuse_value(refusal, SPISOR_MALFORMED, "a number that is not finite");
        }
        *json = cJSON_CreateNumber(value->number);
        break;
    case WEIVEL_STANDARD_SETTING_BOOLEAN:
        if (value->boolean > 1) {
            return refuse_value(refusal, SPISOR_MALFORMED, "a boolean other than 0 or 1");
        }
        *json = cJSON_CreateBool(value->boolean == 1);
        break;
    default:
        return refuse_value(refusal, SPISOR_MALFORMED, "a value of no kind it knows");
    }
    return *json == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status refuse_secret(spisor_refusal *refusal, const char *key) {
    char detail[4096];
    snprintf(detail, sizeof detail, "config's set was given \"%s\", which the plugin declares a secret", key);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, "set a secret through weivel.secrets");
}

static weivel_status set(void *context, const char *key, const weivel_standard_setting_value *value,
                         weivel_standard_service_refusal *refusal) {
    runtime_config_request request = runtime_plugin_scope_request(context);
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    cJSON *json = NULL;
    cJSON *previous = NULL;
    int secret = 0;
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (key == NULL) {
        return runtime_answer_service(refuse_missing_key(&filled, "set"), &filled, refusal);
    }
    if (value == NULL) {
        return runtime_answer_service(refuse_value(&filled, SPISOR_MALFORMED, "no value"), &filled, refusal);
    }
    status = json_of_setting(value, &json, &filled);
    if (status == SPISOR_OK && runtime_config_is_secret_setting(&request, key)) {
        status = refuse_secret(&filled, key);
    }
    if (status == SPISOR_OK) {
        request.key = key;
        request.value = json;
        status = as_scope_answer(runtime_config_set_value(&request, &previous, &secret, &filled));
    }
    cJSON_Delete(previous);
    cJSON_Delete(json);
    return runtime_answer_service(status, &filled, refusal);
}

static void release_settings(void *context, weivel_standard_settings *settings) {
    (void)context;
    if (settings == NULL || !spisor_has_shape(settings->size, SETTINGS_SHAPE)) {
        return;
    }
    spisor_memory_release(settings->storage);
    SPISOR_CLEAR_ANSWER(settings, SETTINGS_SHAPE);
}

static void release_answer(void *context, weivel_standard_setting_answer *answer) {
    (void)context;
    if (answer == NULL || !spisor_has_shape(answer->size, ANSWER_SHAPE)) {
        return;
    }
    spisor_memory_release(answer->storage);
    SPISOR_CLEAR_ANSWER(answer, ANSWER_SHAPE);
}

void runtime_scope_config_fill(runtime_plugin_scope *scope) {
    weivel_standard_config *config = &scope->config;
    config->size = (uint32_t)sizeof *config;
    config->context = scope;
    config->all = all;
    config->get = get;
    config->set = set;
    config->release_settings = release_settings;
    config->release_answer = release_answer;
    config->release_refusal = runtime_release_service_refusal;
}
