#include "marketplace_check.h"

#include <stdio.h>
#include <string.h>

#include "identity.h"
#include "import_name.h"
#include "naula/home.h"

#define LIST_SCHEMA "marketplace.schema.json"
#define COPY_SCHEMA "marketplace-copy.schema.json"
#define ID_PATTERN                                                                                               \
    "^(?:git:(?:[0-9a-f]{40}|[0-9a-f]{64})|local:[^/\\u0000\\t\\n\\u000b\\f\\r ]+)(?:/(?!\\.{1,2}(?:/|(?![\\s\\S])))" \
    "[a-z0-9._-]+)*(?![\\s\\S])"
#define PLACE_PATTERN                                                                                            \
    "^(?:(?!web[@:])[a-z][a-z0-9-]{0,31}(?:@[A-Za-z0-9][A-Za-z0-9.-]{0,254}(?::[0-9]{1,5})?)?:(?=[0-9]{1,19}"     \
    "(?![\\s\\S]))0*[1-9][0-9]*|web:[A-Za-z0-9][A-Za-z0-9.-]{0,254}|https://[\\x21-\\x7e]{1,2040}|ssh://"          \
    "[\\x21-\\x7e]{1,2042})(?![\\s\\S])"
#define IMPORT_NAME_PATTERN                                                                                       \
    "^(?!package(?![\\s\\S]))(?!(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:[\\.]|(?![\\s\\S])))[a-z0-9]+([-.][a-z0-9]+)*" \
    "(?![\\s\\S])"
#define FULL_NAME_PATTERN "^[A-Za-z0-9][A-Za-z0-9._-]*/(?!\\.{1,2}(?![\\s\\S]))[A-Za-z0-9._-]+(?![\\s\\S])"
#define BUILT_PATTERN                                                                                             \
    "^[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])[Tt]([01][0-9]|2[0-3]):[0-5][0-9]:([0-5][0-9]|60)(\\.[0-9]+)?" \
    "([Zz]|[+-]([01][0-9]|2[0-3]):[0-5][0-9])(?![\\s\\S])"
#define ENTRY_PROPERTY "properties/entries/items/properties/"
#define CATEGORY_ITEM "categories/items/pattern"

const runtime_marketplace_held_value runtime_marketplace_held_values[] = {
    {LIST_SCHEMA, "required", "[\"format\",\"entries\"]"},
    {LIST_SCHEMA, "additionalProperties", "false"},
    {LIST_SCHEMA, "properties/format/const", "2"},
    {LIST_SCHEMA, "properties/entries/maxItems", "10000"},
    {LIST_SCHEMA, "properties/entries/items/required", "[\"id\",\"at\"]"},
    {LIST_SCHEMA, "properties/entries/items/additionalProperties", "false"},
    {LIST_SCHEMA, ENTRY_PROPERTY "id/pattern", ID_PATTERN},
    {LIST_SCHEMA, ENTRY_PROPERTY "at/minItems", "1"},
    {LIST_SCHEMA, ENTRY_PROPERTY "at/items/pattern", PLACE_PATTERN},
    {LIST_SCHEMA, ENTRY_PROPERTY "alias/pattern", IMPORT_NAME_PATTERN},
    {LIST_SCHEMA, ENTRY_PROPERTY CATEGORY_ITEM, IMPORT_NAME_PATTERN},
    {LIST_SCHEMA, ENTRY_PROPERTY "categories/uniqueItems", "true"},
    {COPY_SCHEMA, "required", "[\"format\",\"built\",\"entries\"]"},
    {COPY_SCHEMA, "additionalProperties", "false"},
    {COPY_SCHEMA, "properties/format/const", "2"},
    {COPY_SCHEMA, "properties/built/pattern", BUILT_PATTERN},
    {COPY_SCHEMA, "properties/entries/maxItems", "10000"},
    {COPY_SCHEMA, "properties/entries/items/required", "[\"id\",\"at\",\"name\",\"fullName\"]"},
    {COPY_SCHEMA, "properties/entries/items/additionalProperties", "false"},
    {COPY_SCHEMA, ENTRY_PROPERTY "id/pattern", ID_PATTERN},
    {COPY_SCHEMA, ENTRY_PROPERTY "at/minItems", "1"},
    {COPY_SCHEMA, ENTRY_PROPERTY "at/items/pattern", PLACE_PATTERN},
    {COPY_SCHEMA, ENTRY_PROPERTY "name/pattern", IMPORT_NAME_PATTERN},
    {COPY_SCHEMA, ENTRY_PROPERTY "fullName/pattern", FULL_NAME_PATTERN},
    {COPY_SCHEMA, ENTRY_PROPERTY CATEGORY_ITEM, IMPORT_NAME_PATTERN},
    {COPY_SCHEMA, ENTRY_PROPERTY "categories/uniqueItems", "true"},
};

const size_t runtime_marketplace_held_value_count = sizeof runtime_marketplace_held_values / sizeof runtime_marketplace_held_values[0];

static int is_digit(char character) {
    return character >= '0' && character <= '9';
}

static int are_digits(const char *text, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (!is_digit(text[index])) {
            return 0;
        }
    }
    return 1;
}

static int is_two_digits_between(const char *text, int low, int high) {
    if (!are_digits(text, 2)) {
        return 0;
    }
    int value = (text[0] - '0') * 10 + (text[1] - '0');
    return value >= low && value <= high;
}

static int is_date(const char *text) {
    return are_digits(text, 4) && text[4] == '-' && is_two_digits_between(text + 5, 1, 12) && text[7] == '-' &&
           is_two_digits_between(text + 8, 1, 31);
}

static int is_time_of_day(const char *text) {
    return is_two_digits_between(text, 0, 23) && text[2] == ':' && is_two_digits_between(text + 3, 0, 59) && text[5] == ':' &&
           is_two_digits_between(text + 6, 0, 60);
}

/** The text after the optional fraction, or NULL when a "." is not followed by a digit. */
static const char *after_fraction(const char *text) {
    if (*text != '.') {
        return text;
    }
    size_t digits = 0;
    while (is_digit(text[1 + digits])) {
        digits++;
    }
    return digits == 0 ? NULL : text + 1 + digits;
}

static int is_zone(const char *text) {
    if ((text[0] == 'Z' || text[0] == 'z') && text[1] == '\0') {
        return 1;
    }
    return (text[0] == '+' || text[0] == '-') && is_two_digits_between(text + 1, 0, 23) && text[3] == ':' &&
           is_two_digits_between(text + 4, 0, 59) && text[6] == '\0';
}

int runtime_marketplace_is_built_time(const char *text) {
    if (strlen(text) < 19 || !is_date(text) || (text[10] != 'T' && text[10] != 't') || !is_time_of_day(text + 11)) {
        return 0;
    }
    const char *zone = after_fraction(text + 19);
    return zone != NULL && is_zone(zone);
}

static int is_name_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || is_digit(character) ||
           character == '.' || character == '_' || character == '-';
}

static int is_run_of_name_characters(const char *text, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (!is_name_character(text[index])) {
            return 0;
        }
    }
    return length > 0;
}

int runtime_marketplace_is_full_name(const char *text) {
    const char *slash = strchr(text, '/');
    if (slash == NULL || slash == text || is_name_character(text[0]) == 0 || text[0] == '.' || text[0] == '_' ||
        text[0] == '-') {
        return 0;
    }
    const char *name = slash + 1;
    size_t name_length = strlen(name);
    int is_dots_only = strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
    return is_run_of_name_characters(text, (size_t)(slash - text)) && is_run_of_name_characters(name, name_length) && !is_dots_only;
}

static int refuse(char *problem, size_t size, const char *format, size_t position, const char *member) {
    snprintf(problem, size, format, position, member);
    return 0;
}

static int is_string_matching(const cJSON *value, int (*matches)(const char *)) {
    return cJSON_IsString(value) && matches(value->valuestring);
}

/** 1 for a text that is a repository id as naula reads one, 0 for any other value, -1 when memory runs out. */
static int is_repository_id(const cJSON *value) {
    if (!cJSON_IsString(value)) {
        return 0;
    }
    runtime_identity_kind kind = runtime_identity_kind_of(value->valuestring);
    return kind == RUNTIME_IDENTITY_OUT_OF_MEMORY ? -1 : kind != RUNTIME_IDENTITY_NOT_AN_ID;
}

/** 1 for a text that is a place as naula reads one, 0 for any other value, -1 when memory runs out. */
static int is_place(const cJSON *value) {
    if (!cJSON_IsString(value)) {
        return 0;
    }
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    naula_status status = naula_place_parse(value->valuestring, &place, NULL);
    naula_place_release(&place);
    return status == NAULA_OUT_OF_MEMORY ? -1 : status == NAULA_OK;
}

/** 1 for a list of at least one place, 0 for anything else, -1 when memory runs out. */
static int has_places(const cJSON *at) {
    if (!cJSON_IsArray(at) || at->child == NULL) {
        return 0;
    }
    for (const cJSON *place = at->child; place != NULL; place = place->next) {
        int verdict = is_place(place);
        if (verdict != 1) {
            return verdict;
        }
    }
    return 1;
}

static int has_only_members(const cJSON *object, const char *const *allowed, size_t count) {
    for (const cJSON *member = object->child; member != NULL; member = member->next) {
        int known = 0;
        for (size_t index = 0; index < count && !known; index++) {
            known = strcmp(member->string, allowed[index]) == 0;
        }
        if (!known) {
            return 0;
        }
    }
    return 1;
}

static int has_text_members(const cJSON *object, const char *const *names, size_t count) {
    for (size_t index = 0; index < count; index++) {
        const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, names[index]);
        if (member != NULL && !cJSON_IsString(member)) {
            return 0;
        }
    }
    return 1;
}

static int has_unique_names(const cJSON *categories) {
    for (const cJSON *left = categories->child; left != NULL; left = left->next) {
        for (const cJSON *right = left->next; right != NULL; right = right->next) {
            if (strcmp(left->valuestring, right->valuestring) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int has_import_name_categories(const cJSON *categories) {
    if (!cJSON_IsArray(categories)) {
        return 0;
    }
    for (const cJSON *category = categories->child; category != NULL; category = category->next) {
        if (!is_string_matching(category, runtime_is_import_name)) {
            return 0;
        }
    }
    return has_unique_names(categories);
}

static int has_optional_import_name(const cJSON *entry, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, member);
    return value == NULL || is_string_matching(value, runtime_is_import_name);
}

static int check_shared_members(const cJSON *entry, size_t position, char *problem, size_t size) {
    static const char *const text_members[] = {"description"};
    const cJSON *categories = cJSON_GetObjectItemCaseSensitive(entry, "categories");
    if (!has_text_members(entry, text_members, 1)) {
        return refuse(problem, size, "entries[%zu].%s is not text", position, "description");
    }
    if (categories != NULL && !has_import_name_categories(categories)) {
        return refuse(problem, size, "entries[%zu].%s is not a list of distinct import names", position, "categories");
    }
    int verdict = is_repository_id(cJSON_GetObjectItemCaseSensitive(entry, "id"));
    if (verdict != 1) {
        return verdict < 0 ? verdict : refuse(problem, size, "entries[%zu].%s is not a repository id", position, "id");
    }
    verdict = has_places(cJSON_GetObjectItemCaseSensitive(entry, "at"));
    if (verdict != 1) {
        return verdict < 0 ? verdict : refuse(problem, size, "entries[%zu].%s is not a list of places", position, "at");
    }
    return 1;
}

static int has_members(const cJSON *entry, const char *const *required, size_t count, size_t position, char *problem,
                       size_t size) {
    for (size_t index = 0; index < count; index++) {
        if (cJSON_GetObjectItemCaseSensitive(entry, required[index]) == NULL) {
            return refuse(problem, size, "entries[%zu] has no %s", position, required[index]);
        }
    }
    return 1;
}

static int check_list_entry(const cJSON *entry, size_t position, char *problem, size_t size) {
    static const char *const allowed[] = {"id", "at", "alias", "description", "categories"};
    static const char *const required[] = {"id", "at"};
    if (!cJSON_IsObject(entry) || !has_only_members(entry, allowed, 5)) {
        return refuse(problem, size, "entries[%zu] is not an object with only %s", position,
                      "id, at, alias, description and categories");
    }
    if (!has_members(entry, required, 2, position, problem, size)) {
        return 0;
    }
    if (!has_optional_import_name(entry, "alias")) {
        return refuse(problem, size, "entries[%zu].%s is not an import name", position, "alias");
    }
    return check_shared_members(entry, position, problem, size);
}

static int has_full_name(const cJSON *entry, const char *member) {
    return is_string_matching(cJSON_GetObjectItemCaseSensitive(entry, member), runtime_marketplace_is_full_name);
}

static int check_copy_entry(const cJSON *entry, size_t position, char *problem, size_t size) {
    static const char *const allowed[] = {"id", "at", "name", "fullName", "description", "categories"};
    static const char *const required[] = {"id", "at", "name", "fullName"};
    if (!cJSON_IsObject(entry) || !has_only_members(entry, allowed, 6)) {
        return refuse(problem, size, "entries[%zu] is not an object with only %s", position,
                      "id, at, name, fullName, description and categories");
    }
    if (!has_members(entry, required, 4, position, problem, size)) {
        return 0;
    }
    if (!is_string_matching(cJSON_GetObjectItemCaseSensitive(entry, "name"), runtime_is_import_name)) {
        return refuse(problem, size, "entries[%zu].%s is not an import name", position, "name");
    }
    if (!has_full_name(entry, "fullName")) {
        return refuse(problem, size, "entries[%zu].%s is not owner/name", position, "fullName");
    }
    return check_shared_members(entry, position, problem, size);
}

typedef int (*entry_check)(const cJSON *entry, size_t position, char *problem, size_t size);

static int check_entries(const cJSON *entries, entry_check check, char *problem, size_t size) {
    if (!cJSON_IsArray(entries)) {
        snprintf(problem, size, "entries is not a list");
        return 0;
    }
    if (cJSON_GetArraySize(entries) > RUNTIME_MARKETPLACE_MAXIMUM_ENTRIES) {
        snprintf(problem, size, "entries holds more than %d entries", RUNTIME_MARKETPLACE_MAXIMUM_ENTRIES);
        return 0;
    }
    size_t position = 0;
    for (const cJSON *entry = entries->child; entry != NULL; entry = entry->next, position++) {
        int verdict = check(entry, position, problem, size);
        if (verdict != 1) {
            return verdict;
        }
    }
    return 1;
}

static int check_document_head(const cJSON *document, const char *const *allowed, size_t count, char *problem, size_t size) {
    static const char *const text_members[] = {"$schema", "title"};
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(document, "format");
    if (!cJSON_IsObject(document) || !has_only_members(document, allowed, count)) {
        snprintf(problem, size, "the document is not an object holding only the members its schema lists");
        return 0;
    }
    if (!cJSON_IsNumber(format) || format->valuedouble != RUNTIME_MARKETPLACE_FORMAT) {
        snprintf(problem, size, "format is not %d", RUNTIME_MARKETPLACE_FORMAT);
        return 0;
    }
    if (!has_text_members(document, text_members, 2)) {
        snprintf(problem, size, "$schema and title must be text");
        return 0;
    }
    return 1;
}

int runtime_marketplace_check_list(const cJSON *list, char *problem, size_t size) {
    static const char *const allowed[] = {"$schema", "format", "title", "entries"};
    if (!check_document_head(list, allowed, 4, problem, size)) {
        return 0;
    }
    return check_entries(cJSON_GetObjectItemCaseSensitive(list, "entries"), check_list_entry, problem, size);
}

int runtime_marketplace_check_copy(const cJSON *copy, char *problem, size_t size) {
    static const char *const allowed[] = {"$schema", "format", "title", "built", "entries"};
    if (!check_document_head(copy, allowed, 5, problem, size)) {
        return 0;
    }
    if (!is_string_matching(cJSON_GetObjectItemCaseSensitive(copy, "built"), runtime_marketplace_is_built_time)) {
        snprintf(problem, size, "built is not an RFC 3339 timestamp");
        return 0;
    }
    return check_entries(cJSON_GetObjectItemCaseSensitive(copy, "entries"), check_copy_entry, problem, size);
}
