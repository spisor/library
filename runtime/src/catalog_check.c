#include "catalog_check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_order.h"
#include "catalog_text.h"

#define ITEM_ID_PATTERN "^(?!(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?![\\s\\S]))[a-z0-9]+(-[a-z0-9]+)*(?![\\s\\S])"
#define VERSION_PATTERN                                                                                          \
    "^(?![\\s\\S]*\\.\\.)[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]*[0-9A-Za-z-])?(\\+[0-9A-Za-z.-]*[0-9A-Za-z-])?(?!" \
    "[\\s\\S])"
#define FILE_KEY_PATTERN "^[^/\\\\:]*[^/\\\\:. ](?:/[^/\\\\:]*[^/\\\\:. ])*(?![\\s\\S])"
#define SHA256_PATTERN "^[0-9a-f]{64}(?![\\s\\S])"
#define NAME_PATTERN "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}(?![\\s\\S])"
#define CANDIDATE_PATTERN "^(?:~/|/|[A-Za-z]:[\\/])"

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
#define FIX_LIBRARY_RUNTIME "remove runtime, which a library item may not carry: a library runs in its host's runtime"
#define FIX_FILES_OR_PLATFORMS "list files, platforms, or both"
#define FIX_PACKAGE "give the package as a name, or as an object with a name and optional options"

#define ITEM "/definitions/item"
#define APPLICATION "/definitions/application/properties"

const runtime_catalog_schema_value runtime_catalog_schema_patterns[] = {
    {"/properties/items/propertyNames/pattern", ITEM_ID_PATTERN},
    {ITEM "/properties/versions/propertyNames/pattern", VERSION_PATTERN},
    {ITEM "/properties/latest/pattern", VERSION_PATTERN},
    {"/definitions/files/propertyNames/pattern", FILE_KEY_PATTERN},
    {"/definitions/fileRef/properties/sha256/pattern", SHA256_PATTERN},
    {APPLICATION "/binary/pattern", NAME_PATTERN},
    {APPLICATION "/home/properties/xdgSubdir/pattern", NAME_PATTERN},
    {APPLICATION "/home/properties/candidates/items/pattern", CANDIDATE_PATTERN},
    {APPLICATION "/packages/propertyNames/pattern", NAME_PATTERN},
};
const size_t runtime_catalog_schema_pattern_count =
    sizeof runtime_catalog_schema_patterns / sizeof runtime_catalog_schema_patterns[0];

static const char *const KINDS[] = {"plugin", "host", "library", NULL};
static const char *const RUNTIMES[] = {"javascript", "jvm", "native", NULL};

const runtime_catalog_schema_value runtime_catalog_schema_enum_values[] = {
    {ITEM "/properties/kind/enum", "plugin"},
    {ITEM "/properties/kind/enum", "host"},
    {ITEM "/properties/kind/enum", "library"},
    {ITEM "/properties/runtime/enum", "javascript"},
    {ITEM "/properties/runtime/enum", "jvm"},
    {ITEM "/properties/runtime/enum", "native"},
};
const size_t runtime_catalog_schema_enum_value_count =
    sizeof runtime_catalog_schema_enum_values / sizeof runtime_catalog_schema_enum_values[0];

typedef struct checker {
    runtime_catalog_text pointer;
    runtime_catalog_refusal *refusal;
} checker;

typedef int (*key_rule)(const char *key);
typedef int (*value_rule)(checker *check, const cJSON *value);

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

static int fail_pattern(checker *check, const char *pattern, const char *key) {
    runtime_catalog_text message;
    runtime_catalog_text_start(&message);
    runtime_catalog_text_add(&message, "must match pattern \"");
    runtime_catalog_text_add(&message, pattern);
    runtime_catalog_text_add_character(&message, '"');
    char *finished = runtime_catalog_text_finish(&message);
    int result = fail(check, finished == NULL ? "must match a pattern" : finished, FIX_PATTERN, key);
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

static int is_ascii_digit(char character) {
    return character >= '0' && character <= '9';
}

static int is_ascii_letter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

static int is_reserved_device_name(const char *key) {
    static const char *const names[] = {"con", "prn", "aux", "nul"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        if (strcmp(key, names[index]) == 0) {
            return 1;
        }
    }
    int numbered = strncmp(key, "com", 3) == 0 || strncmp(key, "lpt", 3) == 0;
    return numbered && key[3] >= '1' && key[3] <= '9' && key[4] == 0;
}

static int is_item_id(const char *key) {
    size_t length = strlen(key);
    if (length == 0 || key[0] == '-' || key[length - 1] == '-' || strstr(key, "--") != NULL) {
        return 0;
    }
    for (const char *at = key; *at != 0; at++) {
        if (!((*at >= 'a' && *at <= 'z') || is_ascii_digit(*at) || *at == '-')) {
            return 0;
        }
    }
    return !is_reserved_device_name(key);
}

static const char *after_digits(const char *at) {
    if (!is_ascii_digit(*at)) {
        return NULL;
    }
    while (is_ascii_digit(*at)) {
        at++;
    }
    return at;
}

static int is_identifier_character(char character) {
    return is_ascii_letter(character) || is_ascii_digit(character) || character == '.' || character == '-';
}

/** A run of [0-9A-Za-z.-] that does not end in a dot, stopping before stop or the end. */
static const char *after_identifiers(const char *at, char stop) {
    const char *start = at;
    while (*at != 0 && *at != stop) {
        if (!is_identifier_character(*at)) {
            return NULL;
        }
        at++;
    }
    return at == start || at[-1] == '.' ? NULL : at;
}

static int is_version(const char *key) {
    if (strstr(key, "..") != NULL) {
        return 0;
    }
    const char *at = key;
    for (int part = 0; part < 3; part++) {
        at = after_digits(at);
        if (at == NULL || (part < 2 && *at++ != '.')) {
            return 0;
        }
    }
    if (*at == '-') {
        at = after_identifiers(at + 1, '+');
        if (at == NULL) {
            return 0;
        }
    }
    if (*at == '+') {
        at = after_identifiers(at + 1, 0);
        if (at == NULL) {
            return 0;
        }
    }
    return *at == 0;
}

static int is_file_key_segment(const char *start, const char *end) {
    if (end == start || end[-1] == '.' || end[-1] == ' ') {
        return 0;
    }
    for (const char *at = start; at < end; at++) {
        if (*at == '\\' || *at == ':') {
            return 0;
        }
    }
    return 1;
}

static int is_file_key(const char *key) {
    const char *start = key;
    for (;;) {
        const char *end = strchr(start, '/');
        if (end == NULL) {
            return is_file_key_segment(start, start + strlen(start));
        }
        if (!is_file_key_segment(start, end)) {
            return 0;
        }
        start = end + 1;
    }
}

static int is_sha256(const char *text) {
    size_t length = 0;
    for (const char *at = text; *at != 0; at++, length++) {
        if (!(is_ascii_digit(*at) || (*at >= 'a' && *at <= 'f'))) {
            return 0;
        }
    }
    return length == 64;
}

static int is_name(const char *text) {
    if (!(is_ascii_letter(text[0]) || is_ascii_digit(text[0]))) {
        return 0;
    }
    size_t length = 1;
    for (const char *at = text + 1; *at != 0; at++, length++) {
        if (!(is_ascii_letter(*at) || is_ascii_digit(*at) || *at == '.' || *at == '_' || *at == '-')) {
            return 0;
        }
    }
    return length <= 64;
}

static int is_candidate(const char *text) {
    if ((text[0] == '~' && text[1] == '/') || text[0] == '/') {
        return 1;
    }
    return is_ascii_letter(text[0]) && text[1] == ':' && text[2] == '/';
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

static const cJSON *member_of(const cJSON *object, const char *name) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
}

static int has_required(checker *check, const cJSON *object, const char *const *names) {
    for (size_t index = 0; names[index] != NULL; index++) {
        if (member_of(object, names[index]) == NULL) {
            return fail_required(check, names[index]);
        }
    }
    return 1;
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

/** propertyNames then additionalProperties, each over every key in JavaScript's order. */
static int check_map(checker *check, const cJSON *object, key_rule key_valid, const char *key_pattern,
                     value_rule value_valid) {
    size_t count;
    const cJSON **members = runtime_catalog_ordered_members(object, &count);
    if (members == NULL && count > 0) {
        return runtime_catalog_refuse(check->refusal, NULL, FIX_OTHER, NULL);
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

static int check_object(checker *check, const cJSON *value) {
    return cJSON_IsObject(value) ? 1 : fail_type(check, "object");
}

static int check_string(checker *check, const cJSON *value) {
    return cJSON_IsString(value) ? 1 : fail_type(check, "string");
}

static int check_non_empty_string(checker *check, const cJSON *value) {
    if (!check_string(check, value)) {
        return 0;
    }
    return value->valuestring[0] != 0 ? 1 : fail(check, "must NOT have fewer than 1 characters", FIX_MIN_LENGTH, NULL);
}

static int check_name(checker *check, const cJSON *value) {
    if (!check_string(check, value)) {
        return 0;
    }
    return is_name(value->valuestring) ? 1 : fail_pattern(check, NAME_PATTERN, NULL);
}

static int check_size(checker *check, const cJSON *value) {
    int integer = cJSON_IsNumber(value) && isfinite(value->valuedouble) && floor(value->valuedouble) == value->valuedouble;
    if (!integer) {
        return fail_type(check, "integer");
    }
    return value->valuedouble >= 0 ? 1 : fail(check, "must be >= 0", FIX_MINIMUM, NULL);
}

static int check_sha256(checker *check, const cJSON *value) {
    if (!check_string(check, value)) {
        return 0;
    }
    return is_sha256(value->valuestring) ? 1 : fail_pattern(check, SHA256_PATTERN, NULL);
}

static int check_file_reference(checker *check, const cJSON *value) {
    static const char *const required[] = {"url", "sha256", "size", NULL};
    return check_object(check, value) && has_required(check, value, required) &&
           check_member(check, value, "url", check_non_empty_string) &&
           check_member(check, value, "sha256", check_sha256) && check_member(check, value, "size", check_size);
}

static int check_files(checker *check, const cJSON *value) {
    return check_object(check, value) &&
           check_map(check, value, is_file_key, FILE_KEY_PATTERN, check_file_reference);
}

static int check_platform(checker *check, const cJSON *value) {
    static const char *const required[] = {"files", NULL};
    return check_object(check, value) && has_required(check, value, required) &&
           check_member(check, value, "files", check_files);
}

static int check_platforms(checker *check, const cJSON *value) {
    return check_object(check, value) && check_map(check, value, NULL, NULL, check_platform);
}

static int check_version(checker *check, const cJSON *value) {
    if (cJSON_IsObject(value) && member_of(value, "files") == NULL && member_of(value, "platforms") == NULL) {
        return fail(check, "must match a schema in anyOf", FIX_FILES_OR_PLATFORMS, NULL);
    }
    return check_object(check, value) && check_member(check, value, "files", check_files) &&
           check_member(check, value, "platforms", check_platforms) &&
           check_member(check, value, "packages", check_object);
}

static int check_versions(checker *check, const cJSON *value) {
    return check_object(check, value) && check_map(check, value, is_version, VERSION_PATTERN, check_version);
}

static int check_latest(checker *check, const cJSON *value) {
    if (!check_string(check, value)) {
        return 0;
    }
    return is_version(value->valuestring) ? 1 : fail_pattern(check, VERSION_PATTERN, NULL);
}

static int check_kind(checker *check, const cJSON *value) {
    return is_one_of(value, KINDS) ? 1 : fail(check, "must be equal to one of the allowed values", FIX_ENUM, NULL);
}

static int check_runtime(checker *check, const cJSON *value) {
    return is_one_of(value, RUNTIMES) ? 1 : fail(check, "must be equal to one of the allowed values", FIX_ENUM, NULL);
}

static int check_candidate(checker *check, const cJSON *value) {
    if (!check_string(check, value)) {
        return 0;
    }
    return is_candidate(value->valuestring) ? 1 : fail_pattern(check, CANDIDATE_PATTERN, NULL);
}

static int check_candidates(checker *check, const cJSON *value) {
    if (!cJSON_IsArray(value)) {
        return fail_type(check, "array");
    }
    if (cJSON_GetArraySize(value) < 1) {
        return fail(check, "must NOT have fewer than 1 items", FIX_OTHER, NULL);
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

static int check_home(checker *check, const cJSON *value) {
    static const char *const required[] = {"candidates", NULL};
    return check_object(check, value) && has_required(check, value, required) &&
           check_member(check, value, "envOverride", check_non_empty_string) &&
           check_member(check, value, "nativeEnv", check_non_empty_string) &&
           check_member(check, value, "xdgSubdir", check_name) &&
           check_member(check, value, "candidates", check_candidates);
}

static int is_package(const cJSON *value) {
    if (cJSON_IsString(value)) {
        return 1;
    }
    const cJSON *name = member_of(value, "name");
    const cJSON *options = member_of(value, "options");
    return cJSON_IsObject(value) && cJSON_IsString(name) && (options == NULL || cJSON_IsObject(options));
}

static int check_package(checker *check, const cJSON *value) {
    return is_package(value) ? 1 : fail(check, "must match a schema in anyOf", FIX_PACKAGE, NULL);
}

static int check_packages(checker *check, const cJSON *value) {
    return check_object(check, value) && check_map(check, value, is_name, NAME_PATTERN, check_package);
}

static int check_application(checker *check, const cJSON *value) {
    static const char *const required[] = {"id", "label", "binary", "home", "packages", NULL};
    return check_object(check, value) && has_required(check, value, required) &&
           check_member(check, value, "id", check_non_empty_string) &&
           check_member(check, value, "label", check_non_empty_string) &&
           check_member(check, value, "binary", check_name) && check_member(check, value, "home", check_home) &&
           check_member(check, value, "packages", check_packages);
}

/**
 * Whether an item's if clause holds: an item that is not an object satisfies every object keyword, so
 * its then applies, which is why such an item is refused by a not before its type.
 */
static int kind_is_one_of(const cJSON *item, const char *const *kinds) {
    return !cJSON_IsObject(item) || is_one_of(member_of(item, "kind"), kinds);
}

static int check_kind_rules(checker *check, const cJSON *item) {
    static const char *const plugin_or_library[] = {"plugin", "library", NULL};
    static const char *const host[] = {"host", NULL};
    static const char *const library[] = {"library", NULL};
    if (kind_is_one_of(item, plugin_or_library) && (!cJSON_IsObject(item) || member_of(item, "application") != NULL)) {
        return fail(check, "must NOT be valid", FIX_APPLICATION, NULL);
    }
    if (cJSON_IsObject(item) && kind_is_one_of(item, host) && member_of(item, "runtime") == NULL) {
        return fail_required(check, "runtime");
    }
    if (kind_is_one_of(item, library) && (!cJSON_IsObject(item) || member_of(item, "runtime") != NULL)) {
        return fail(check, "must NOT be valid", FIX_LIBRARY_RUNTIME, NULL);
    }
    return 1;
}

static int check_item(checker *check, const cJSON *value) {
    static const char *const required[] = {"kind", "versions", "latest", NULL};
    return check_kind_rules(check, value) && check_object(check, value) && has_required(check, value, required) &&
           check_member(check, value, "kind", check_kind) && check_member(check, value, "versions", check_versions) &&
           check_member(check, value, "latest", check_latest) &&
           check_member(check, value, "application", check_application) &&
           check_member(check, value, "installMethod", check_non_empty_string) &&
           check_member(check, value, "runtime", check_runtime);
}

static int check_items(checker *check, const cJSON *value) {
    return check_object(check, value) && check_map(check, value, is_item_id, ITEM_ID_PATTERN, check_item);
}

static int check_format(checker *check, const cJSON *value) {
    return cJSON_IsNumber(value) && value->valuedouble == 1 ? 1
                                                             : fail(check, "must be equal to constant", FIX_CONST, NULL);
}

int runtime_catalog_check_schema(const cJSON *document, runtime_catalog_refusal *refusal) {
    static const char *const required[] = {"format", "items", NULL};
    checker check;
    runtime_catalog_text_start(&check.pointer);
    check.refusal = refusal;
    int valid = check_object(&check, document) && has_required(&check, document, required) &&
                check_member(&check, document, "format", check_format) &&
                check_member(&check, document, "items", check_items);
    runtime_catalog_text_discard(&check.pointer);
    return valid;
}
