#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <weivel/descriptions.h>

#include "check.h"
#include "described_checks.h"
#include "scope_fixture.h"
#include "spisor/json_allocation.h"
#include "spisor/json_file.h"

/**
 * @file
 * `weivel.scope` as runtime serves it: a plugin's scope opened from its manifest, refused at open for a manifest one
 * typo breaks, and its `config` answering that plugin's merged settings flattened to the standard's kinds.
 */

static scope_place place;

static weivel_status open_from(const char *manifest, weivel_standard_scope *scope,
                               weivel_standard_service_refusal *refusal) {
    const weivel_standard_scopes *scopes = scopes_definition();
    return scopes->open(scopes->context, manifest, scope, refusal);
}

static void release(weivel_standard_scope *scope, weivel_standard_service_refusal *refusal) {
    const weivel_standard_scopes *scopes = scopes_definition();
    scopes->release_scope(scopes->context, scope);
    scopes->release_refusal(scopes->context, refusal);
}

static const weivel_standard_config *open_viewer(weivel_standard_scope *scope) {
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_status status = open_from(place.manifest, scope, &refusal);
    if (status != WEIVEL_STATUS_OK) {
        fprintf(stderr, "the viewer's scope did not open: %d \"%s\"\n", (int)status,
                refusal.detail == NULL ? "" : refusal.detail);
        check_failures++;
    }
    scopes_definition()->release_refusal(NULL, &refusal);
    return scope->config;
}

static int holds(const char *text, const char *part) {
    return text != NULL && strstr(text, part) != NULL;
}

static void the_scope_refuses_to_open_before_the_host_starts(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    runtime_scopes_end();
    CHECK(open_from(place.manifest, &scope, &refusal) == WEIVEL_STATUS_REFUSED);
    CHECK(holds(refusal.detail, "no host is started") && holds(refusal.fix, "weivel.host"));
    CHECK(scope.storage == NULL && scope.config == NULL);
    release(&scope, &refusal);
}

static void a_malformed_open_changes_nothing(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_standard_service_refusal held = WEIVEL_INIT(weivel_standard_service_refusal);
    scope.size = (uint32_t)(offsetof(weivel_standard_scope, storage));
    CHECK(open_from(place.manifest, &scope, &refusal) == WEIVEL_STATUS_MALFORMED);
    CHECK(holds(refusal.detail, "short of its first shape") && scope.config == NULL);
    release(&scope, &refusal);
    scope.size = (uint32_t)sizeof scope;
    CHECK(open_from(NULL, &scope, &refusal) == WEIVEL_STATUS_MALFORMED);
    CHECK(holds(refusal.detail, "without a manifest path") && scope.storage == NULL);
    release(&scope, &refusal);
    held.storage = &held;
    CHECK(open_from(place.manifest, &scope, &held) == WEIVEL_STATUS_MALFORMED);
    CHECK(scope.storage == NULL && held.detail == NULL);
    CHECK(open_from(place.manifest, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
}

/** Opens a manifest of `text` and requires REFUSED naming `detail` with nothing filled. */
static void refuses_manifest(const char *name, const char *text, const char *detail) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    char *manifest = text == NULL ? joined_path(place.base, name) : write_manifest(&place, name, text);
    weivel_status status = open_from(manifest, &scope, &refusal);
    if (status != WEIVEL_STATUS_REFUSED || !holds(refusal.detail, detail) || refusal.fix == NULL ||
        scope.storage != NULL || scope.config != NULL) {
        fprintf(stderr, "%s: expected REFUSED naming \"%s\", got %d \"%s\"\n", name, detail, (int)status,
                refusal.detail == NULL ? "" : refusal.detail);
        check_failures++;
    }
    release(&scope, &refusal);
    spisor_memory_release(manifest);
}

static void a_manifest_one_typo_breaks_is_refused_at_open(void) {
    refuses_manifest("layer.json",
                     "{\"id\": \"viewer\", \"settings\": {\"theme\": {\"type\": \"text\", \"layer\": \"aplication\"}}}",
                     "declares the setting \"theme\" in the layer \"aplication\", which runtime does not know");
    refuses_manifest("layer-number.json", "{\"id\": \"viewer\", \"settings\": {\"theme\": {\"layer\": 2}}}",
                     "declares the setting \"theme\" with a layer that is not text");
    refuses_manifest("declaration.json", "{\"id\": \"viewer\", \"settings\": {\"theme\": \"text\"}}",
                     "declares the setting \"theme\" as something other than a record");
    refuses_manifest("settings.json", "{\"id\": \"viewer\", \"settings\": [\"theme\"]}",
                     "declares settings that are not a record");
    refuses_manifest("config.json", "{\"id\": \"viewer\", \"config\": \"viewer\"}",
                     "declares a config that is not a record");
    refuses_manifest("no-id.json", "{\"config\": {}}", "carries no id");
    refuses_manifest("list.json", "[]", "is not a JSON object");
    refuses_manifest("broken.json", "{\"id\": ", "cannot be read as JSON");
    refuses_manifest("absent.json", NULL, "does not exist");
    refuses_manifest("name.json", "{\"id\": \"viewer\", \"config\": {\"name\": \"a/b\"}}",
                     "the plugin's config name is not a usable file name");
}

static const weivel_standard_setting *setting_named(const weivel_standard_settings *settings, const char *key) {
    for (size_t index = 0; index < settings->count; index++) {
        if (strcmp(settings->settings[index].key, key) == 0) {
            return &settings->settings[index];
        }
    }
    return NULL;
}

static void check_number(const weivel_standard_settings *settings, const char *key, double number) {
    const weivel_standard_setting *setting = setting_named(settings, key);
    if (setting == NULL || setting->value.kind != WEIVEL_STANDARD_SETTING_NUMBER || setting->value.number != number) {
        fprintf(stderr, "%s is not the number %g\n", key, number);
        check_failures++;
    }
}

static void check_setting_text(const weivel_standard_settings *settings, const char *key, const char *text) {
    const weivel_standard_setting *setting = setting_named(settings, key);
    if (setting == NULL || setting->value.kind != WEIVEL_STANDARD_SETTING_TEXT ||
        setting->value.text_length != strlen(text) || strcmp(setting->value.text, text) != 0) {
        fprintf(stderr, "%s is not the text %s\n", key, text);
        check_failures++;
    }
}

static void check_boolean(const weivel_standard_settings *settings, const char *key, uint32_t boolean) {
    const weivel_standard_setting *setting = setting_named(settings, key);
    if (setting == NULL || setting->value.kind != WEIVEL_STANDARD_SETTING_BOOLEAN ||
        setting->value.boolean != boolean) {
        fprintf(stderr, "%s is not the boolean %u\n", key, (unsigned)boolean);
        check_failures++;
    }
}

static void all_offers_every_setting_flattened(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_settings settings = WEIVEL_INIT(weivel_standard_settings);
    const weivel_standard_config *config = open_viewer(&scope);
    if (config == NULL) {
        return;
    }
    CHECK(config->all(config->context, &settings, NULL) == WEIVEL_STATUS_OK);
    CHECK(settings.count == 6 && settings.storage != NULL);
    check_setting_text(&settings, "theme", "light");
    check_number(&settings, "depth", 3);
    check_boolean(&settings, "debug", 0);
    check_boolean(&settings, "cache.enabled", 1);
    check_number(&settings, "cache.ttl", 30);
    check_setting_text(&settings, "cache.inner.deep", "x");
    CHECK(setting_named(&settings, "token") == NULL && setting_named(&settings, "nothing") == NULL &&
          setting_named(&settings, "list") == NULL && setting_named(&settings, "cache") == NULL);
    config->release_settings(config->context, &settings);
    CHECK(settings.storage == NULL && settings.count == 0 && settings.settings == NULL);
    release(&scope, NULL);
}

static weivel_status get_into(const weivel_standard_config *config, const char *key,
                              weivel_standard_setting_answer *answer) {
    return config->get(config->context, key, answer, NULL);
}

static void get_answers_what_is_offered_and_none_for_the_rest(void) {
    static const char *const unoffered[] = {"cache", "list", "nothing", "token", "missing", "dot.ted", "a..b",
                                            "token.inside"};
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_setting_answer answer = WEIVEL_INIT(weivel_standard_setting_answer);
    const weivel_standard_config *config = open_viewer(&scope);
    if (config == NULL) {
        return;
    }
    CHECK(get_into(config, "theme", &answer) == WEIVEL_STATUS_OK);
    CHECK(answer.value.kind == WEIVEL_STANDARD_SETTING_TEXT && answer.value.text_length == 5 &&
          strcmp(answer.value.text, "light") == 0 && answer.storage != NULL);
    config->release_answer(config->context, &answer);
    CHECK(answer.storage == NULL && answer.value.kind == WEIVEL_STANDARD_SETTING_NONE && answer.value.text == NULL);
    CHECK(get_into(config, "cache.ttl", &answer) == WEIVEL_STATUS_OK);
    CHECK(answer.value.kind == WEIVEL_STANDARD_SETTING_NUMBER && answer.value.number == 30);
    config->release_answer(config->context, &answer);
    CHECK(get_into(config, "cache.enabled", &answer) == WEIVEL_STATUS_OK);
    CHECK(answer.value.kind == WEIVEL_STANDARD_SETTING_BOOLEAN && answer.value.boolean == 1);
    config->release_answer(config->context, &answer);
    for (size_t index = 0; index < sizeof unoffered / sizeof unoffered[0]; index++) {
        if (get_into(config, unoffered[index], &answer) != WEIVEL_STATUS_NONE ||
            answer.value.kind != WEIVEL_STANDARD_SETTING_NONE || answer.storage != NULL) {
            fprintf(stderr, "get(\"%s\") offered what the scope must not\n", unoffered[index]);
            check_failures++;
        }
    }
    CHECK(get_into(config, NULL, &answer) == WEIVEL_STATUS_MALFORMED);
    answer.size = (uint32_t)offsetof(weivel_standard_setting_answer, storage);
    CHECK(get_into(config, "theme", &answer) == WEIVEL_STATUS_MALFORMED);
    release(&scope, NULL);
}

static weivel_status set_value(const weivel_standard_config *config, const char *key,
                               weivel_standard_setting_value value, weivel_standard_service_refusal *refusal) {
    return config->set(config->context, key, &value, refusal);
}

static weivel_standard_setting_value number_value(double number) {
    weivel_standard_setting_value value = {WEIVEL_STANDARD_SETTING_NUMBER, 0, number, NULL, 0};
    return value;
}

static weivel_standard_setting_value text_value(const char *text, size_t length) {
    weivel_standard_setting_value value = {WEIVEL_STANDARD_SETTING_TEXT, 0, 0.0, text, length};
    return value;
}

/** Whether the JSON file at directory/name holds `key` with the number `number`. */
static int file_holds_number(const char *directory, const char *name, const char *key, double number) {
    char *path = joined_path(directory, name);
    spisor_json_file_state state = SPISOR_JSON_FILE_ABSENT;
    cJSON *file = path == NULL ? NULL : spisor_read_json_file(path, &state);
    const cJSON *found = cJSON_GetObjectItemCaseSensitive(file, key);
    int holds_number = cJSON_IsNumber(found) && found->valuedouble == number;
    cJSON_Delete(file);
    spisor_memory_release(path);
    return holds_number;
}

static void set_writes_the_declared_layer_and_get_reads_it_back(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_setting_answer answer = WEIVEL_INIT(weivel_standard_setting_answer);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    char *plugin_folder = joined_path(place.portable, "plugins/viewer");
    const weivel_standard_config *config = open_viewer(&scope);
    if (config == NULL) {
        return;
    }
    CHECK(set_value(config, "depth", number_value(7), &refusal) == WEIVEL_STATUS_OK && refusal.detail == NULL);
    CHECK(file_holds_number(plugin_folder, "config.json", "depth", 7));
    CHECK(set_value(config, "width", number_value(2), &refusal) == WEIVEL_STATUS_OK);
    CHECK(file_holds_number(place.config, "viewer.json", "width", 2));
    CHECK(get_into(config, "depth", &answer) == WEIVEL_STATUS_OK && answer.value.number == 7);
    config->release_answer(config->context, &answer);
    CHECK(set_value(config, "theme", text_value("blue", 4), &refusal) == WEIVEL_STATUS_OK);
    CHECK(get_into(config, "theme", &answer) == WEIVEL_STATUS_OK && strcmp(answer.value.text, "blue") == 0);
    config->release_answer(config->context, &answer);
    CHECK(set_value(config, "token", text_value("s", 1), &refusal) == WEIVEL_STATUS_REFUSED);
    CHECK(holds(refusal.detail, "declares a secret") && holds(refusal.fix, "weivel.secrets"));
    config->release_refusal(config->context, &refusal);
    CHECK(set_value(config, "token.inside", number_value(1), &refusal) == WEIVEL_STATUS_REFUSED);
    config->release_refusal(config->context, &refusal);
    CHECK(set_value(config, "a..b", number_value(1), &refusal) == WEIVEL_STATUS_REFUSED);
    CHECK(holds(refusal.detail, "usable key"));
    config->release_refusal(config->context, &refusal);
    spisor_memory_release(plugin_folder);
    release(&scope, &refusal);
}

/** Requires the set of `value` to answer `expected`, naming `detail`, and to write nothing. */
static void set_refuses(const weivel_standard_config *config, weivel_standard_setting_value value,
                        weivel_status expected, const char *detail) {
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_status status = set_value(config, "debug", value, &refusal);
    if (status != expected || !holds(refusal.detail, detail)) {
        fprintf(stderr, "a set naming \"%s\" answered %d \"%s\"\n", detail, (int)status,
                refusal.detail == NULL ? "" : refusal.detail);
        check_failures++;
    }
    config->release_refusal(config->context, &refusal);
}

static void set_refuses_what_services_h_does_not_allow(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_setting_value boolean = {WEIVEL_STANDARD_SETTING_BOOLEAN, 2, 0.0, NULL, 0};
    weivel_standard_setting_value none = {WEIVEL_STANDARD_SETTING_NONE, 0, 0.0, NULL, 0};
    weivel_standard_setting_value unknown = {9, 0, 0.0, NULL, 0};
    const weivel_standard_config *config = open_viewer(&scope);
    if (config == NULL) {
        return;
    }
    set_refuses(config, boolean, WEIVEL_STATUS_MALFORMED, "a boolean other than 0 or 1");
    set_refuses(config, none, WEIVEL_STATUS_MALFORMED, "a value of no kind it knows");
    set_refuses(config, unknown, WEIVEL_STATUS_MALFORMED, "a value of no kind it knows");
    set_refuses(config, number_value(INFINITY), WEIVEL_STATUS_MALFORMED, "a number that is not finite");
    set_refuses(config, number_value(NAN), WEIVEL_STATUS_MALFORMED, "a number that is not finite");
    set_refuses(config, text_value(NULL, 0), WEIVEL_STATUS_MALFORMED, "a text value without its text");
    set_refuses(config, text_value("\xff", 1), WEIVEL_STATUS_MALFORMED, "not well-formed UTF-8");
    set_refuses(config, text_value("a\0b", 3), WEIVEL_STATUS_REFUSED, "a text holding a NUL byte");
    CHECK(config->set(config->context, "debug", NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(set_value(config, NULL, number_value(1), NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(!file_holds_number(place.config, "viewer.json", "debug", 1));
    release(&scope, NULL);
}

#define LONG_RECORD "a_record_whose_name_is_longer_than_every_key_a_leaf_of_this_plugin_has"
#define LONG_HOLDER "a_record_whose_name_is_long_and_which_holds_one_short_leaf_below_it"

/** Records whose dotted keys outgrow every leaf key, one holding no leaf at all, so the key buffer must fit each. */
static void all_measures_every_key_it_builds(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_settings settings = WEIVEL_INIT(weivel_standard_settings);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    char *manifest = write_manifest(&place, "long-keys.json",
                                    "{\"id\": \"long-keys\", \"config\": {\"defaults\": {\"a\": 1, \"" LONG_RECORD
                                    "\": {\"x\": null}, \"" LONG_HOLDER "\": {\"y\": 2}}}}");
    CHECK(open_from(manifest, &scope, &refusal) == WEIVEL_STATUS_OK);
    if (scope.config != NULL) {
        CHECK(scope.config->all(scope.config->context, &settings, NULL) == WEIVEL_STATUS_OK);
        CHECK(settings.count == 2);
        check_number(&settings, "a", 1);
        check_number(&settings, LONG_HOLDER ".y", 2);
        scope.config->release_settings(scope.config->context, &settings);
    }
    release(&scope, &refusal);
    spisor_memory_release(manifest);
}

/** A consumer compiled against a later scope, whose struct is longer than runtime's. */
typedef struct later_scope {
    weivel_standard_scope scope;
    unsigned char appended[16];
} later_scope;

static void a_later_consumer_has_its_appended_bytes_zeroed(void) {
    later_scope later;
    memset(&later, 0, sizeof later);
    later.scope.size = (uint32_t)sizeof later;
    memset(later.appended, 0xab, sizeof later.appended);
    open_viewer(&later.scope);
    for (size_t index = 0; index < sizeof later.appended; index++) {
        CHECK(later.appended[index] == 0);
    }
    CHECK(later.scope.size == sizeof later && later.scope.secrets == NULL && later.scope.paths == NULL &&
          later.scope.log == NULL);
    release(&later.scope, NULL);
}

#define LATER_BYTES 16

/** A struct a later consumer hands in, `bytes` long with LATER_BYTES appended beyond runtime's, those set nonzero. */
static void *later_struct(size_t bytes) {
    unsigned char *later = calloc(1, bytes + LATER_BYTES);
    uint32_t size = (uint32_t)(bytes + LATER_BYTES);
    CHECK(later != NULL);
    if (later != NULL) {
        memcpy(later, &size, sizeof size);
        memset(later + bytes, 0xab, LATER_BYTES);
    }
    return later;
}

static void check_appended_zero(const void *later, size_t bytes, const char *what) {
    const unsigned char *appended = (const unsigned char *)later + bytes;
    for (size_t index = 0; index < LATER_BYTES; index++) {
        if (appended[index] != 0) {
            fprintf(stderr, "%s kept byte %zu a later consumer appended\n", what, index);
            check_failures++;
            return;
        }
    }
}

static void a_later_consumer_has_its_answers_and_refusal_zeroed_beyond(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_settings *settings = later_struct(sizeof *settings);
    weivel_standard_setting_answer *answer = later_struct(sizeof *answer);
    weivel_standard_service_refusal *refusal = later_struct(sizeof *refusal);
    const weivel_standard_config *config = open_viewer(&scope);
    if (config != NULL && settings != NULL && answer != NULL && refusal != NULL) {
        CHECK(config->all(config->context, settings, NULL) == WEIVEL_STATUS_OK);
        check_appended_zero(settings, sizeof *settings, "all's settings");
        config->release_settings(config->context, settings);
        CHECK(get_into(config, "theme", answer) == WEIVEL_STATUS_OK);
        check_appended_zero(answer, sizeof *answer, "get's answer");
        config->release_answer(config->context, answer);
        CHECK(set_value(config, "token", text_value("s", 1), refusal) == WEIVEL_STATUS_REFUSED);
        check_appended_zero(refusal, sizeof *refusal, "set's refusal");
        config->release_refusal(config->context, refusal);
    }
    free(settings);
    free(answer);
    free(refusal);
    release(&scope, NULL);
}

static void a_scope_outlives_the_host_and_release_empties_it(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_setting_answer answer = WEIVEL_INIT(weivel_standard_setting_answer);
    runtime_scope_host host = {NULL, NULL, NULL};
    const weivel_standard_config *config = open_viewer(&scope);
    if (config == NULL) {
        return;
    }
    runtime_scopes_end();
    CHECK(get_into(config, "theme", &answer) == WEIVEL_STATUS_OK);
    config->release_answer(config->context, &answer);
    release(&scope, NULL);
    CHECK(scope.size == sizeof scope && scope.config == NULL && scope.storage == NULL);
    release(&scope, NULL);
    host.home = place.home;
    host.portable = place.portable;
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
}

static void the_host_refuses_to_start_somewhere_unusable(void) {
    runtime_scope_host host = {"relative/home", NULL, NULL};
    cJSON *names = cJSON_CreateArray();
    CHECK(runtime_scopes_begin(&host) == SPISOR_MALFORMED);
    CHECK(runtime_scopes_begin(NULL) == SPISOR_MALFORMED);
    host.home = place.home;
    host.names = names;
    CHECK(runtime_scopes_begin(&host) == SPISOR_MALFORMED);
    host.names = NULL;
    host.portable = "relative";
    CHECK(runtime_scopes_begin(&host) == SPISOR_MALFORMED);
    cJSON_Delete(names);
}

static void the_scope_config_passes_the_described_checks(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    spisor_described_definition described = {NULL, NULL};
    const weivel_standard_config *config = open_viewer(&scope);
    if (config == NULL) {
        return;
    }
    described.description = weivel_standard_config_description();
    described.definition = config;
    CHECK(spisor_check_described(&described, 1, spisor_check_short_shapes) == 0);
    CHECK(spisor_check_described(&described, 1, spisor_check_counted_answers) == 0);
    release(&scope, NULL);
}

int main(void) {
    spisor_json_use_library_allocator();
    start_scope_place(&place, "scope", "fixture");
    the_scope_refuses_to_open_before_the_host_starts();
    scope_place_free(&place);
    start_scope_place(&place, "scope", "started");
    a_malformed_open_changes_nothing();
    a_manifest_one_typo_breaks_is_refused_at_open();
    all_offers_every_setting_flattened();
    get_answers_what_is_offered_and_none_for_the_rest();
    set_refuses_what_services_h_does_not_allow();
    set_writes_the_declared_layer_and_get_reads_it_back();
    all_measures_every_key_it_builds();
    a_later_consumer_has_its_appended_bytes_zeroed();
    a_later_consumer_has_its_answers_and_refusal_zeroed_beyond();
    a_scope_outlives_the_host_and_release_empties_it();
    the_host_refuses_to_start_somewhere_unusable();
    the_scope_config_passes_the_described_checks();
    runtime_scopes_end();
    scope_place_free(&place);
    return check_summary("scope_test");
}
