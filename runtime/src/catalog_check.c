#include "catalog_check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_patterns.h"
#include "catalog_schema.h"
#include "catalog_text.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"

#define FIX_PATTERN                                                                                               \
    "use the shape the schema's pattern requires: a plugin-style id, a semver version, a 64-character lowercase " \
    "hex sha256, or a file key of non-empty relative segments with / separators"
#define FIX_ENUM "use one of the values the schema lists for this member"
#define FIX_REQUIRED "add the missing member"
#define FIX_CONST "set the member to the value the schema requires"
#define FIX_TYPE "give the member the type the schema requires"
#define FIX_MINIMUM "use a value of at least the schema's minimum"
#define FIX_MIN_LENGTH "give the member a non-empty value"
#define FIX_OTHER "correct the member the path names"
#define FIX_APPLICATION "remove the member, which this kind of item may not carry; only a host item may have an application"
#define FIX_FILES_OR_PLATFORMS "list files, platforms, or both"
#define FIX_PACKAGE "give the package as a name, or as an object with a name and optional options"

struct runtime_catalog_checker {
    runtime_catalog_text pointer;
    runtime_catalog_refusal *refusal;
};

typedef runtime_catalog_checker checker;
typedef int (*key_rule)(const char *key);
typedef int (*value_rule)(checker *check, const cJSON *value);

static const char *const *list(runtime_catalog_list_index index) {
    return runtime_catalog_schema_lists[index].values;
}

static double bound(runtime_catalog_bound_index index) {
    return runtime_catalog_schema_bounds[index].value;
}

static const char *pattern(runtime_catalog_pattern_index index) {
    return runtime_catalog_schema_patterns[index].value;
}

static int fail(checker *check, const char *message, const char *fix, const char *appended) {
    runtime_catalog_text detail;
    runtime_catalog_text_start(&detail);
    runtime_catalog_text_add(&detail, "the catalog index ");
    runtime_catalog_text_add(&detail, message);
    runtime_catalog_text path;
    runtime_catalog_text_start(&path);
    runtime_catalog_text_add_bytes(&path, check->pointer.bytes == NULL ? "" : check->pointer.bytes,
                                   check->pointer.length);
    if (appended != NULL) {
        runtime_catalog_text_add_character(&path, '/');
        runtime_catalog_text_add(&path, appended);
    }
    return runtime_catalog_refuse(check->refusal, runtime_catalog_text_finish(&detail), fix,
                                  runtime_catalog_text_finish(&path));
}

static int fail_out_of_memory(checker *check) {
    return runtime_catalog_refuse(check->refusal, NULL, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX, NULL);
}

static int fail_type(checker *check, const char *type) {
    char message[32];
    snprintf(message, sizeof message, "must be %s", type);
    return fail(check, message, FIX_TYPE, NULL);
}

static int fail_required(checker *check, const char *member) {
    char message[96];
    snprintf(message, sizeof message, "must have required property '%s'", member);
    return fail(check, message, FIX_REQUIRED, member);
}

static int fail_bound(checker *check, const char *shape, runtime_catalog_bound_index index, const char *fix) {
    char message[96];
    snprintf(message, sizeof message, shape, bound(index));
    return fail(check, message, fix, NULL);
}

static int fail_pattern(checker *check, runtime_catalog_pattern_index index, const char *key) {
    runtime_catalog_text message;
    runtime_catalog_text_start(&message);
    runtime_catalog_text_add(&message, "must match pattern \"");
    runtime_catalog_text_add(&message, pattern(index));
    runtime_catalog_text_add_character(&message, '"');
    char *finished = runtime_catalog_text_finish(&message);
    int result = finished == NULL ? fail_out_of_memory(check) : fail(check, finished, FIX_PATTERN, key);
    free(finished);
    return result;
}

static size_t enter(checker *check, const char *key) {
    size_t before = check->pointer.length;
    runtime_catalog_text_add_character(&check->pointer, '/');
    for (const char *at = key; *at != 0; at++) {
        if (*at == '~') {
            runtime_catalog_text_add(&check->pointer, "~0");
        } else if (*at == '/') {
            runtime_catalog_text_add(&check->pointer, "~1");
        } else {
            runtime_catalog_text_add_character(&check->pointer, *at);
        }
    }
    return before;
}

static void leave(checker *check, size_t before) {
    runtime_catalog_text_cut(&check->pointer, before);
}

static int is_one_of(const cJSON *value, const char *const *allowed) {
    if (!cJSON_IsString(value)) {
        return 0;
    }
    for (size_t index = 0; allowed[index] != NULL; index++) {
        if (strcmp(value->valuestring, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int equals_constant(const cJSON *value, runtime_catalog_constant_index index) {
    const char *json = runtime_catalog_schema_constants[index].value;
    cJSON *constant = spisor_parse_json((const uint8_t *)json, strlen(json));
    int equal = value != NULL && constant != NULL && cJSON_Compare(value, constant, 1);
    cJSON_Delete(constant);
    return equal;
}

static const cJSON *member_of(const cJSON *object, const char *name) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
}

/** The first member of names that object lacks, or NULL; a value that is not an object lacks none. */
static const char *first_missing(const cJSON *object, const char *const *names) {
    for (size_t index = 0; cJSON_IsObject(object) && names[index] != NULL; index++) {
        if (member_of(object, names[index]) == NULL) {
            return names[index];
        }
    }
    return NULL;
}

static int has_required(checker *check, const cJSON *object, runtime_catalog_list_index index) {
    const char *missing = first_missing(object, list(index));
    return missing == NULL ? 1 : fail_required(check, missing);
}

static int check_member(checker *check, const cJSON *object, const char *name, value_rule rule) {
    const cJSON *value = member_of(object, name);
    if (value == NULL) {
        return 1;
    }
    size_t before = enter(check, name);
    int valid = rule(check, value);
    leave(check, before);
    return valid;
}

static int check_properties(checker *check, const cJSON *object, const runtime_catalog_property_rule *rules) {
    for (size_t index = 0; rules[index].name != NULL; index++) {
        if (!check_member(check, object, rules[index].name, rules[index].rule)) {
            return 0;
        }
    }
    return 1;
}

/** propertyNames then additionalProperties, each over every key in JavaScript's order. */
static int check_map(checker *check, const cJSON *object, key_rule key_valid, runtime_catalog_pattern_index key_pattern,
                     value_rule value_valid) {
    size_t count;
    const cJSON **members = spisor_json_members_in_javascript_order(object, &count);
    if (members == NULL && count > 0) {
        return fail_out_of_memory(check);
    }
    int valid = 1;
    for (size_t index = 0; valid && key_valid != NULL && index < count; index++) {
        if (!key_valid(members[index]->string)) {
            valid = fail_pattern(check, key_pattern, members[index]->string);
        }
    }
    for (size_t index = 0; valid && index < count; index++) {
        size_t before = enter(check, members[index]->string);
        valid = value_valid(check, members[index]);
        leave(check, before);
    }
    free(members);
    return valid;
}

/** Whether value passes rule, without reporting, as an anyOf branch is tried. */
static int passes_quietly(const cJSON *value, value_rule rule) {
    runtime_catalog_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    checker quiet;
    runtime_catalog_text_start(&quiet.pointer);
    quiet.refusal = &refusal;
    int passes = rule(&quiet, value);
    runtime_catalog_text_discard(&quiet.pointer);
    runtime_catalog_free_refusal(&refusal);
    return passes;
}

static size_t code_point_count(const char *text) {
    size_t count = 0;
    for (const unsigned char *at = (const unsigned char *)text; *at != 0; at++) {
        count += (*at & 0xC0) != 0x80;
    }
    return count;
}

static int check_object(checker *check, const cJSON *value) {
    return cJSON_IsObject(value) ? 1 : fail_type(check, "object");
}

static int check_string(checker *check, const cJSON *value) {
    return cJSON_IsString(value) ? 1 : fail_type(check, "string");
}

static int check_length(checker *check, const cJSON *value, runtime_catalog_bound_index index) {
    if (!check_string(check, value)) {
        return 0;
    }
    return (double)code_point_count(value->valuestring) >= bound(index)
               ? 1
               : fail_bound(check, "must NOT have fewer than %g characters", index, FIX_MIN_LENGTH);
}

static int check_patterned(checker *check, const cJSON *value, key_rule matches, runtime_catalog_pattern_index index) {
    if (!check_string(check, value)) {
        return 0;
    }
    return matches(value->valuestring) ? 1 : fail_pattern(check, index, NULL);
}

static int check_url(checker *check, const cJSON *value) {
    return check_length(check, value, MIN_LENGTH_URL);
}

static int check_install_method(checker *check, const cJSON *value) {
    return check_length(check, value, MIN_LENGTH_INSTALL_METHOD);
}

static int check_application_id(checker *check, const cJSON *value) {
    return check_length(check, value, MIN_LENGTH_APPLICATION_ID);
}

static int check_application_label(checker *check, const cJSON *value) {
    return check_length(check, value, MIN_LENGTH_APPLICATION_LABEL);
}

static int check_env_override(checker *check, const cJSON *value) {
    return check_length(check, value, MIN_LENGTH_ENV_OVERRIDE);
}

static int check_native_env(checker *check, const cJSON *value) {
    return check_length(check, value, MIN_LENGTH_NATIVE_ENV);
}

static int check_binary(checker *check, const cJSON *value) {
    return check_patterned(check, value, runtime_catalog_is_name, PATTERN_BINARY);
}

static int check_xdg_subdir(checker *check, const cJSON *value) {
    return check_patterned(check, value, runtime_catalog_is_name, PATTERN_XDG_SUBDIR);
}

static int check_sha256(checker *check, const cJSON *value) {
    return check_patterned(check, value, runtime_catalog_is_sha256, PATTERN_SHA256);
}

static int check_latest(checker *check, const cJSON *value) {
    return check_patterned(check, value, runtime_catalog_is_version, PATTERN_LATEST);
}

static int check_candidate(checker *check, const cJSON *value) {
    return check_patterned(check, value, runtime_catalog_is_candidate, PATTERN_CANDIDATE);
}

static int check_size(checker *check, const cJSON *value) {
    int integer = cJSON_IsNumber(value) && isfinite(value->valuedouble) && floor(value->valuedouble) == value->valuedouble;
    if (!integer) {
        return fail_type(check, "integer");
    }
    return value->valuedouble >= bound(MINIMUM_SIZE) ? 1 : fail_bound(check, "must be >= %g", MINIMUM_SIZE, FIX_MINIMUM);
}

static int check_kind(checker *check, const cJSON *value) {
    return is_one_of(value, list(ENUM_KIND)) ? 1 : fail(check, "must be equal to one of the allowed values", FIX_ENUM, NULL);
}

static int check_runtime(checker *check, const cJSON *value) {
    return is_one_of(value, list(ENUM_RUNTIME)) ? 1
                                                : fail(check, "must be equal to one of the allowed values", FIX_ENUM, NULL);
}

static int check_format(checker *check, const cJSON *value) {
    return equals_constant(value, CONSTANT_FORMAT) ? 1 : fail(check, "must be equal to constant", FIX_CONST, NULL);
}

static const runtime_catalog_property_rule FILE_REFERENCE_PROPERTIES[] = {
    {"url", check_url}, {"sha256", check_sha256}, {"size", check_size}, {NULL, NULL}};

static int check_file_reference(checker *check, const cJSON *value) {
    return check_object(check, value) && has_required(check, value, REQUIRED_FILE_REFERENCE) &&
           check_properties(check, value, FILE_REFERENCE_PROPERTIES);
}

static int check_files(checker *check, const cJSON *value) {
    return check_object(check, value) &&
           check_map(check, value, runtime_catalog_is_file_key, PATTERN_FILE_KEY, check_file_reference);
}

static const runtime_catalog_property_rule PLATFORM_PROPERTIES[] = {{"files", check_files}, {NULL, NULL}};

static int check_platform(checker *check, const cJSON *value) {
    return check_object(check, value) && has_required(check, value, REQUIRED_PLATFORM) &&
           check_properties(check, value, PLATFORM_PROPERTIES);
}

static int check_platforms(checker *check, const cJSON *value) {
    return check_object(check, value) && check_map(check, value, NULL, PATTERN_COUNT, check_platform);
}

static const runtime_catalog_property_rule VERSION_PROPERTIES[] = {
    {"files", check_files}, {"platforms", check_platforms}, {"packages", check_object}, {NULL, NULL}};

static int check_version(checker *check, const cJSON *value) {
    int either = first_missing(value, list(REQUIRED_VERSION_FILES)) == NULL ||
                 first_missing(value, list(REQUIRED_VERSION_PLATFORMS)) == NULL;
    if (!either) {
        return fail(check, "must match a schema in anyOf", FIX_FILES_OR_PLATFORMS, NULL);
    }
    return check_object(check, value) && check_properties(check, value, VERSION_PROPERTIES);
}

static int check_versions(checker *check, const cJSON *value) {
    return check_object(check, value) &&
           check_map(check, value, runtime_catalog_is_version, PATTERN_VERSION_KEY, check_version);
}

static int check_candidates(checker *check, const cJSON *value) {
    if (!cJSON_IsArray(value)) {
        return fail_type(check, "array");
    }
    if ((double)cJSON_GetArraySize(value) < bound(MIN_ITEMS_CANDIDATES)) {
        return fail_bound(check, "must NOT have fewer than %g items", MIN_ITEMS_CANDIDATES, FIX_OTHER);
    }
    int index = 0;
    for (const cJSON *candidate = value->child; candidate != NULL; candidate = candidate->next, index++) {
        char key[24];
        snprintf(key, sizeof key, "%d", index);
        size_t before = enter(check, key);
        int valid = check_candidate(check, candidate);
        leave(check, before);
        if (!valid) {
            return 0;
        }
    }
    return 1;
}

static const runtime_catalog_property_rule HOME_PROPERTIES[] = {{"envOverride", check_env_override},
                                                                {"nativeEnv", check_native_env},
                                                                {"xdgSubdir", check_xdg_subdir},
                                                                {"candidates", check_candidates},
                                                                {NULL, NULL}};

static int check_home(checker *check, const cJSON *value) {
    return check_object(check, value) && has_required(check, value, REQUIRED_HOME) &&
           check_properties(check, value, HOME_PROPERTIES);
}

static const runtime_catalog_property_rule PACKAGE_PROPERTIES[] = {
    {"name", check_string}, {"options", check_object}, {NULL, NULL}};

static int check_package_object(checker *check, const cJSON *value) {
    return check_object(check, value) && has_required(check, value, REQUIRED_PACKAGE) &&
           check_properties(check, value, PACKAGE_PROPERTIES);
}

static int check_package(checker *check, const cJSON *value) {
    int passes = passes_quietly(value, check_string) || passes_quietly(value, check_package_object);
    return passes ? 1 : fail(check, "must match a schema in anyOf", FIX_PACKAGE, NULL);
}

static int check_packages(checker *check, const cJSON *value) {
    return check_object(check, value) &&
           check_map(check, value, runtime_catalog_is_name, PATTERN_PACKAGE_NAME, check_package);
}

static const runtime_catalog_property_rule APPLICATION_PROPERTIES[] = {{"id", check_application_id},
                                                                       {"label", check_application_label},
                                                                       {"binary", check_binary},
                                                                       {"home", check_home},
                                                                       {"packages", check_packages},
                                                                       {NULL, NULL}};

static int check_application(checker *check, const cJSON *value) {
    return check_object(check, value) && has_required(check, value, REQUIRED_APPLICATION) &&
           check_properties(check, value, APPLICATION_PROPERTIES);
}

/**
 * Whether an item rule's if clause holds: an item that is not an object satisfies every object keyword, so
 * its then applies, which is why such an item is refused by a not before its type.
 */
static int application_rule_applies(const cJSON *item) {
    return first_missing(item, list(REQUIRED_APPLICATION_RULE_IF)) == NULL &&
           (!cJSON_IsObject(item) || is_one_of(member_of(item, "kind"), list(ENUM_APPLICATION_RULE_KINDS)));
}

static int runtime_rule_applies(const cJSON *item) {
    return first_missing(item, list(REQUIRED_RUNTIME_RULE_IF)) == NULL &&
           (!cJSON_IsObject(item) || equals_constant(member_of(item, "kind"), CONSTANT_RUNTIME_RULE_KIND));
}

static int check_item_rules(checker *check, const cJSON *item) {
    if (application_rule_applies(item) && first_missing(item, list(REQUIRED_APPLICATION_RULE_NOT)) == NULL) {
        return fail(check, "must NOT be valid", FIX_APPLICATION, NULL);
    }
    return !runtime_rule_applies(item) || has_required(check, item, REQUIRED_RUNTIME_RULE_THEN);
}

static const runtime_catalog_property_rule ITEM_PROPERTIES[] = {{"kind", check_kind},
                                                                {"versions", check_versions},
                                                                {"latest", check_latest},
                                                                {"application", check_application},
                                                                {"installMethod", check_install_method},
                                                                {"runtime", check_runtime},
                                                                {NULL, NULL}};

static int check_item(checker *check, const cJSON *value) {
    return check_item_rules(check, value) && check_object(check, value) && has_required(check, value, REQUIRED_ITEM) &&
           check_properties(check, value, ITEM_PROPERTIES);
}

static int check_items(checker *check, const cJSON *value) {
    return check_object(check, value) && check_map(check, value, runtime_catalog_is_item_id, PATTERN_ITEM_ID, check_item);
}

static const runtime_catalog_property_rule ROOT_PROPERTIES[] = {
    {"format", check_format}, {"items", check_items}, {NULL, NULL}};

#define PACKAGE_PROPERTIES_POINTER "/definitions/application/properties/packages/additionalProperties/anyOf/1/properties"

const runtime_catalog_schema_properties runtime_catalog_schema_property_sets[] = {
    {"/properties", ROOT_PROPERTIES},
    {"/definitions/item/properties", ITEM_PROPERTIES},
    {"/definitions/version/properties", VERSION_PROPERTIES},
    {"/definitions/version/properties/platforms/additionalProperties/properties", PLATFORM_PROPERTIES},
    {"/definitions/fileRef/properties", FILE_REFERENCE_PROPERTIES},
    {"/definitions/application/properties", APPLICATION_PROPERTIES},
    {"/definitions/application/properties/home/properties", HOME_PROPERTIES},
    {PACKAGE_PROPERTIES_POINTER, PACKAGE_PROPERTIES},
};
const size_t runtime_catalog_schema_property_set_count =
    sizeof runtime_catalog_schema_property_sets / sizeof runtime_catalog_schema_property_sets[0];

int runtime_catalog_check_schema(const cJSON *document, runtime_catalog_refusal *refusal) {
    checker check;
    runtime_catalog_text_start(&check.pointer);
    check.refusal = refusal;
    int valid = check_object(&check, document) && has_required(&check, document, REQUIRED_ROOT) &&
                check_properties(&check, document, ROOT_PROPERTIES);
    runtime_catalog_text_discard(&check.pointer);
    return valid;
}
