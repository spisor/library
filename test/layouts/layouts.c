#include <stdio.h>

#include "runtime/config.h"
#include "runtime/detect.h"
#include "runtime/fetch.h"
#include "runtime/identity.h"
#include "runtime/log.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"
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
    FIELD(runtime_plugin_places, size);
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

static void print_secrets_layouts(void) {
    FIELD(runtime_secret_places, size);
    FIELD(runtime_secret_places, secrets_file);
    FIELD(runtime_secret_places, force_file_store);
    SIZE(runtime_secret_places);
    FIELD(runtime_secrets_entry_argument, size);
    FIELD(runtime_secrets_entry_argument, places);
    FIELD(runtime_secrets_entry_argument, plugin);
    FIELD(runtime_secrets_entry_argument, name);
    SIZE(runtime_secrets_entry_argument);
    FIELD(runtime_secrets_set_argument, size);
    FIELD(runtime_secrets_set_argument, places);
    FIELD(runtime_secrets_set_argument, plugin);
    FIELD(runtime_secrets_set_argument, name);
    FIELD(runtime_secrets_set_argument, value);
    SIZE(runtime_secrets_set_argument);
    FIELD(runtime_secrets_clear_argument, size);
    FIELD(runtime_secrets_clear_argument, places);
    FIELD(runtime_secrets_clear_argument, plugin);
    SIZE(runtime_secrets_clear_argument);
    FIELD(runtime_secrets_reconcile_argument, size);
    FIELD(runtime_secrets_reconcile_argument, places);
    SIZE(runtime_secrets_reconcile_argument);
    FIELD(runtime_secrets_get_answer, size);
    FIELD(runtime_secrets_get_answer, value);
    FIELD(runtime_secrets_get_answer, length);
    FIELD(runtime_secrets_get_answer, storage);
    SIZE(runtime_secrets_get_answer);
    FIELD(runtime_secrets_set_answer, size);
    FIELD(runtime_secrets_set_answer, location);
    SIZE(runtime_secrets_set_answer);
    FIELD(runtime_secrets_reconcile_answer, size);
    FIELD(runtime_secrets_reconcile_answer, moved);
    SIZE(runtime_secrets_reconcile_answer);
    FIELD(runtime_secrets_definition, size);
    FIELD(runtime_secrets_definition, context);
    FIELD(runtime_secrets_definition, get);
    FIELD(runtime_secrets_definition, set);
    FIELD(runtime_secrets_definition, remove);
    FIELD(runtime_secrets_definition, clear);
    FIELD(runtime_secrets_definition, reconcile);
    FIELD(runtime_secrets_definition, release_get_answer);
    FIELD(runtime_secrets_definition, release_refusal);
    SIZE(runtime_secrets_definition);
}

static void print_settings_layouts(void) {
    FIELD(runtime_settings_scope, size);
    FIELD(runtime_settings_scope, home);
    FIELD(runtime_settings_scope, names);
    FIELD(runtime_settings_scope, portable);
    SIZE(runtime_settings_scope);
    FIELD(runtime_settings_change, size);
    FIELD(runtime_settings_change, key);
    FIELD(runtime_settings_change, value);
    FIELD(runtime_settings_change, layer);
    SIZE(runtime_settings_change);
    FIELD(runtime_settings_definition, size);
    FIELD(runtime_settings_definition, context);
    FIELD(runtime_settings_definition, all);
    FIELD(runtime_settings_definition, set);
    FIELD(runtime_settings_definition, release_tree);
    FIELD(runtime_settings_definition, release_refusal);
    SIZE(runtime_settings_definition);
}

static void print_config_layouts(void) {
    FIELD(runtime_setting_declaration, key);
    FIELD(runtime_setting_declaration, type);
    FIELD(runtime_setting_declaration, layer);
    SIZE(runtime_setting_declaration);
    FIELD(runtime_config_plugin, size);
    FIELD(runtime_config_plugin, home);
    FIELD(runtime_config_plugin, names);
    FIELD(runtime_config_plugin, portable);
    FIELD(runtime_config_plugin, plugin);
    FIELD(runtime_config_plugin, config);
    FIELD(runtime_config_plugin, settings);
    FIELD(runtime_config_plugin, setting_count);
    FIELD(runtime_config_plugin, places);
    SIZE(runtime_config_plugin);
    FIELD(runtime_config_get_argument, size);
    FIELD(runtime_config_get_argument, key);
    SIZE(runtime_config_get_argument);
    FIELD(runtime_config_set_answer, size);
    FIELD(runtime_config_set_answer, secret);
    FIELD(runtime_config_set_answer, previous);
    FIELD(runtime_config_set_answer, storage);
    SIZE(runtime_config_set_answer);
    FIELD(runtime_config_definition, size);
    FIELD(runtime_config_definition, context);
    FIELD(runtime_config_definition, all);
    FIELD(runtime_config_definition, get);
    FIELD(runtime_config_definition, set);
    FIELD(runtime_config_definition, release_tree);
    FIELD(runtime_config_definition, release_set_answer);
    FIELD(runtime_config_definition, release_refusal);
    SIZE(runtime_config_definition);
}

static void print_log_layouts(void) {
    FIELD(runtime_log_entry, size);
    FIELD(runtime_log_entry, home);
    FIELD(runtime_log_entry, names);
    FIELD(runtime_log_entry, portable);
    FIELD(runtime_log_entry, plugin);
    FIELD(runtime_log_entry, config);
    FIELD(runtime_log_entry, start);
    FIELD(runtime_log_entry, time);
    FIELD(runtime_log_entry, level);
    FIELD(runtime_log_entry, message);
    FIELD(runtime_log_entry, no_color);
    SIZE(runtime_log_entry);
    FIELD(runtime_log_answer, size);
    FIELD(runtime_log_answer, console);
    FIELD(runtime_log_answer, storage);
    SIZE(runtime_log_answer);
    FIELD(runtime_log_definition, size);
    FIELD(runtime_log_definition, context);
    FIELD(runtime_log_definition, write);
    FIELD(runtime_log_definition, release_answer);
    FIELD(runtime_log_definition, release_refusal);
    SIZE(runtime_log_definition);
}

static void print_identity_layouts(void) {
    FIELD(runtime_identity_id_argument, size);
    FIELD(runtime_identity_id_argument, id);
    SIZE(runtime_identity_id_argument);
    FIELD(runtime_identity_parts, size);
    FIELD(runtime_identity_parts, resolver);
    FIELD(runtime_identity_parts, host);
    FIELD(runtime_identity_parts, number);
    FIELD(runtime_identity_parts, path);
    FIELD(runtime_identity_parts, storage);
    SIZE(runtime_identity_parts);
    FIELD(runtime_identity_folder_answer, size);
    FIELD(runtime_identity_folder_answer, folder);
    FIELD(runtime_identity_folder_answer, storage);
    SIZE(runtime_identity_folder_answer);
    FIELD(runtime_identity_resolve_argument, size);
    FIELD(runtime_identity_resolve_argument, id);
    FIELD(runtime_identity_resolve_argument, last_known);
    FIELD(runtime_identity_resolve_argument, token);
    FIELD(runtime_identity_resolve_argument, api_base);
    SIZE(runtime_identity_resolve_argument);
    FIELD(runtime_identity_repository, size);
    FIELD(runtime_identity_repository, name);
    FIELD(runtime_identity_repository, full_name);
    FIELD(runtime_identity_repository, default_branch);
    FIELD(runtime_identity_repository, storage);
    SIZE(runtime_identity_repository);
    FIELD(runtime_identity_copy, id);
    FIELD(runtime_identity_copy, read_failed);
    FIELD(runtime_identity_copy, file);
    SIZE(runtime_identity_copy);
    FIELD(runtime_identity_agree_argument, size);
    FIELD(runtime_identity_agree_argument, id);
    FIELD(runtime_identity_agree_argument, copies);
    FIELD(runtime_identity_agree_argument, copy_count);
    SIZE(runtime_identity_agree_argument);
    FIELD(runtime_identity_also_at_argument, size);
    FIELD(runtime_identity_also_at_argument, id);
    FIELD(runtime_identity_also_at_argument, api_base);
    SIZE(runtime_identity_also_at_argument);
    FIELD(runtime_identity_agreement, size);
    FIELD(runtime_identity_agreement, ids);
    FIELD(runtime_identity_agreement, id_count);
    FIELD(runtime_identity_agreement, primary);
    FIELD(runtime_identity_agreement, primary_confirmed);
    FIELD(runtime_identity_agreement, start_file_malformed);
    FIELD(runtime_identity_agreement, start_file_unavailable);
    FIELD(runtime_identity_agreement, storage);
    SIZE(runtime_identity_agreement);
    FIELD(runtime_identity_version_argument, size);
    FIELD(runtime_identity_version_argument, recorded);
    FIELD(runtime_identity_version_argument, primary);
    SIZE(runtime_identity_version_argument);
    FIELD(runtime_identity_version_answer, size);
    FIELD(runtime_identity_version_answer, accepted);
    SIZE(runtime_identity_version_answer);
    FIELD(runtime_identity_definition, size);
    FIELD(runtime_identity_definition, context);
    FIELD(runtime_identity_definition, parse);
    FIELD(runtime_identity_definition, folder);
    FIELD(runtime_identity_definition, resolve);
    FIELD(runtime_identity_definition, agree);
    FIELD(runtime_identity_definition, also_at);
    FIELD(runtime_identity_definition, accepts_version);
    FIELD(runtime_identity_definition, release_parts);
    FIELD(runtime_identity_definition, release_folder_answer);
    FIELD(runtime_identity_definition, release_repository);
    FIELD(runtime_identity_definition, release_agreement);
    FIELD(runtime_identity_definition, release_refusal);
    SIZE(runtime_identity_definition);
}

static void print_fetch_layouts(void) {
    FIELD(runtime_fetch_get_argument, size);
    FIELD(runtime_fetch_get_argument, url);
    FIELD(runtime_fetch_get_argument, destination);
    FIELD(runtime_fetch_get_argument, sha256);
    FIELD(runtime_fetch_get_argument, etag);
    FIELD(runtime_fetch_get_argument, last_modified);
    FIELD(runtime_fetch_get_argument, limited);
    FIELD(runtime_fetch_get_argument, max_bytes);
    FIELD(runtime_fetch_get_argument, timeout_seconds);
    SIZE(runtime_fetch_get_argument);
    FIELD(runtime_fetch_get_answer, size);
    FIELD(runtime_fetch_get_answer, outcome);
    FIELD(runtime_fetch_get_answer, sha256);
    FIELD(runtime_fetch_get_answer, bytes);
    FIELD(runtime_fetch_get_answer, etag);
    FIELD(runtime_fetch_get_answer, last_modified);
    FIELD(runtime_fetch_get_answer, storage);
    SIZE(runtime_fetch_get_answer);
    FIELD(runtime_fetch_definition, size);
    FIELD(runtime_fetch_definition, context);
    FIELD(runtime_fetch_definition, get);
    FIELD(runtime_fetch_definition, release_answer);
    FIELD(runtime_fetch_definition, release_refusal);
    SIZE(runtime_fetch_definition);
}

static void print_detect_layouts(void) {
    FIELD(runtime_detect_on_path_argument, size);
    FIELD(runtime_detect_on_path_argument, binary);
    FIELD(runtime_detect_on_path_argument, environment);
    FIELD(runtime_detect_on_path_argument, environment_count);
    SIZE(runtime_detect_on_path_argument);
    FIELD(runtime_detect_on_path_answer, size);
    FIELD(runtime_detect_on_path_answer, path);
    FIELD(runtime_detect_on_path_answer, storage);
    SIZE(runtime_detect_on_path_answer);
    FIELD(runtime_detect_definition, size);
    FIELD(runtime_detect_definition, context);
    FIELD(runtime_detect_definition, on_path);
    FIELD(runtime_detect_definition, release_answer);
    FIELD(runtime_detect_definition, release_refusal);
    SIZE(runtime_detect_definition);
}

int main(void) {
    print_status_layouts();
    print_value_layouts();
    print_json_kind_layouts();
    print_paths_layouts();
    print_places_layouts();
    print_secrets_layouts();
    print_settings_layouts();
    print_config_layouts();
    print_log_layouts();
    print_identity_layouts();
    print_fetch_layouts();
    print_detect_layouts();
    return 0;
}
