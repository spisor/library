#include <stdio.h>

#include "runtime/paths.h"
#include "runtime/places.h"
#include "spisor/json_kind.h"
#include "spisor/status.h"
#include "spisor/value.h"

/**
 * @file
 * Prints the layout of every public struct the library's headers declare, as this compiler lays it out, so a language
 * that declares the same layouts by hand compares its own against the C compiler's rather than against a guess. The
 * format is weivel/standard's layouts program's:
 *
 *     <struct> <field> <offset> <size>
 *     <struct> sizeof <size>
 *
 * A struct's field lines come in declaration order and are followed by its sizeof line. A field of struct type is one
 * line; that struct is listed by its own name. An opaque struct has no layout and is not listed. Lines end in CRLF on
 * Windows, where stdout is a text stream, and in LF elsewhere, so a consumer strips a trailing carriage return.
 */

static void print_field(const char *type, const char *field, const void *instance, const void *member,
                        size_t size) {
    printf("%s %s %zu %zu\n", type, field, (size_t)((const char *)member - (const char *)instance), size);
}

static void print_size(const char *type, size_t size) {
    printf("%s sizeof %zu\n", type, size);
}

#define FIELD(type, member)                                                               \
    do {                                                                                  \
        static type instance;                                                             \
        print_field(#type, #member, &instance, &instance.member, sizeof instance.member); \
    } while (0)

#define SIZE(type) print_size(#type, sizeof(type))

static void print_status_layouts(void) {
    FIELD(spisor_refusal, size);
    FIELD(spisor_refusal, detail);
    FIELD(spisor_refusal, fix);
    FIELD(spisor_refusal, storage);
    SIZE(spisor_refusal);
}

static void print_value_layouts(void) {
    FIELD(spisor_value, kind);
    FIELD(spisor_value, boolean);
    FIELD(spisor_value, number);
    FIELD(spisor_value, text);
    FIELD(spisor_value, length);
    FIELD(spisor_value, items);
    FIELD(spisor_value, entries);
    FIELD(spisor_value, count);
    SIZE(spisor_value);
    FIELD(spisor_entry, key);
    FIELD(spisor_entry, key_length);
    FIELD(spisor_entry, value);
    SIZE(spisor_entry);
    FIELD(spisor_value_tree, size);
    FIELD(spisor_value_tree, root);
    FIELD(spisor_value_tree, storage);
    SIZE(spisor_value_tree);
}

static void print_json_kind_layouts(void) {
    FIELD(spisor_call, argument);
    FIELD(spisor_call, context);
    SIZE(spisor_call);
}

static void print_paths_layouts(void) {
    FIELD(runtime_plugin_places, portable);
    FIELD(runtime_plugin_places, cache);
    SIZE(runtime_plugin_places);
    FIELD(runtime_paths_resolve_argument, size);
    FIELD(runtime_paths_resolve_argument, home);
    FIELD(runtime_paths_resolve_argument, names);
    FIELD(runtime_paths_resolve_argument, plugin);
    FIELD(runtime_paths_resolve_argument, places);
    SIZE(runtime_paths_resolve_argument);
    FIELD(runtime_paths_answer, size);
    FIELD(runtime_paths_answer, home);
    FIELD(runtime_paths_answer, repos);
    FIELD(runtime_paths_answer, plugin);
    FIELD(runtime_paths_answer, cache);
    FIELD(runtime_paths_answer, config);
    FIELD(runtime_paths_answer, portable);
    FIELD(runtime_paths_answer, plugin_cache);
    FIELD(runtime_paths_answer, storage);
    SIZE(runtime_paths_answer);
    FIELD(runtime_paths_definition, size);
    FIELD(runtime_paths_definition, context);
    FIELD(runtime_paths_definition, resolve);
    FIELD(runtime_paths_definition, release_answer);
    FIELD(runtime_paths_definition, release_refusal);
    SIZE(runtime_paths_definition);
}

static void print_places_layouts(void) {
    FIELD(runtime_variable, name);
    FIELD(runtime_variable, value);
    SIZE(runtime_variable);
    FIELD(runtime_places_resolve_argument, size);
    FIELD(runtime_places_resolve_argument, home);
    FIELD(runtime_places_resolve_argument, environment);
    FIELD(runtime_places_resolve_argument, environment_count);
    SIZE(runtime_places_resolve_argument);
    FIELD(runtime_places_answer, size);
    FIELD(runtime_places_answer, home);
    FIELD(runtime_places_answer, portable);
    FIELD(runtime_places_answer, state);
    FIELD(runtime_places_answer, cache);
    FIELD(runtime_places_answer, secrets_file);
    FIELD(runtime_places_answer, force_file_store);
    FIELD(runtime_places_answer, storage);
    SIZE(runtime_places_answer);
    FIELD(runtime_places_definition, size);
    FIELD(runtime_places_definition, context);
    FIELD(runtime_places_definition, resolve);
    FIELD(runtime_places_definition, release_answer);
    FIELD(runtime_places_definition, release_refusal);
    SIZE(runtime_places_definition);
}

int main(void) {
    print_status_layouts();
    print_value_layouts();
    print_json_kind_layouts();
    print_paths_layouts();
    print_places_layouts();
    return 0;
}
