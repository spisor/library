#include <stdio.h>

#include "runtime/cache.h"
#include "runtime/catalog.h"
#include "runtime/changes.h"
#include "runtime/commands.h"
#include "runtime/config.h"
#include "runtime/detect.h"
#include "runtime/fetch.h"
#include "runtime/identity.h"
#include "runtime/install.h"
#include "runtime/lists.h"
#include "runtime/log.h"
#include "runtime/marketplace.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/registry.h"
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

static void print_registry_layouts(void) {
    FIELD(runtime_registry_provenance, size);
    FIELD(runtime_registry_provenance, method);
    FIELD(runtime_registry_provenance, version);
    FIELD(runtime_registry_provenance, location);
    FIELD(runtime_registry_provenance, os);
    FIELD(runtime_registry_provenance, architecture);
    FIELD(runtime_registry_provenance, installed_at);
    FIELD(runtime_registry_provenance, package);
    FIELD(runtime_registry_provenance, options);
    SIZE(runtime_registry_provenance);
    FIELD(runtime_registry_start, size);
    FIELD(runtime_registry_start, runtime);
    FIELD(runtime_registry_start, entry);
    SIZE(runtime_registry_start);
    FIELD(runtime_registration, size);
    FIELD(runtime_registration, id);
    FIELD(runtime_registration, version);
    FIELD(runtime_registration, home);
    FIELD(runtime_registration, application);
    FIELD(runtime_registration, provenance);
    FIELD(runtime_registration, requires_command);
    FIELD(runtime_registration, start);
    SIZE(runtime_registration);
    FIELD(runtime_registry_register_argument, size);
    FIELD(runtime_registry_register_argument, state);
    FIELD(runtime_registry_register_argument, registration);
    SIZE(runtime_registry_register_argument);
    FIELD(runtime_registry_host_argument, size);
    FIELD(runtime_registry_host_argument, state);
    FIELD(runtime_registry_host_argument, id);
    SIZE(runtime_registry_host_argument);
    FIELD(runtime_registry_list_argument, size);
    FIELD(runtime_registry_list_argument, state);
    SIZE(runtime_registry_list_argument);
    FIELD(runtime_registry_forget_argument, size);
    FIELD(runtime_registry_forget_argument, state);
    FIELD(runtime_registry_forget_argument, cache);
    FIELD(runtime_registry_forget_argument, id);
    SIZE(runtime_registry_forget_argument);
    FIELD(runtime_registry_registration_answer, size);
    FIELD(runtime_registry_registration_answer, registration);
    FIELD(runtime_registry_registration_answer, storage);
    SIZE(runtime_registry_registration_answer);
    FIELD(runtime_registry_host, registration);
    FIELD(runtime_registry_host, orphan);
    SIZE(runtime_registry_host);
    FIELD(runtime_registry_listing, size);
    FIELD(runtime_registry_listing, hosts);
    FIELD(runtime_registry_listing, host_count);
    FIELD(runtime_registry_listing, unreadable);
    FIELD(runtime_registry_listing, unreadable_count);
    FIELD(runtime_registry_listing, storage);
    SIZE(runtime_registry_listing);
    FIELD(runtime_registry_forget_answer, size);
    FIELD(runtime_registry_forget_answer, forgotten);
    SIZE(runtime_registry_forget_answer);
    FIELD(runtime_registry_definition, size);
    FIELD(runtime_registry_definition, context);
    FIELD(runtime_registry_definition, register_host);
    FIELD(runtime_registry_definition, get);
    FIELD(runtime_registry_definition, list);
    FIELD(runtime_registry_definition, forget);
    FIELD(runtime_registry_definition, release_answer);
    FIELD(runtime_registry_definition, release_listing);
    FIELD(runtime_registry_definition, release_refusal);
    SIZE(runtime_registry_definition);
}

static void print_changes_layouts(void) {
    FIELD(runtime_change, size);
    FIELD(runtime_change, kind);
    FIELD(runtime_change, path);
    FIELD(runtime_change, content);
    FIELD(runtime_change, executable);
    FIELD(runtime_change, file);
    FIELD(runtime_change, key);
    FIELD(runtime_change, key_count);
    FIELD(runtime_change, value);
    FIELD(runtime_change, create);
    FIELD(runtime_change, line);
    FIELD(runtime_change, directory);
    FIELD(runtime_change, home);
    FIELD(runtime_change, source);
    FIELD(runtime_change, shell);
    SIZE(runtime_change);
    FIELD(runtime_changes_apply_argument, size);
    FIELD(runtime_changes_apply_argument, state);
    FIELD(runtime_changes_apply_argument, host);
    FIELD(runtime_changes_apply_argument, change);
    SIZE(runtime_changes_apply_argument);
    FIELD(runtime_changes_apply_answer, size);
    FIELD(runtime_changes_apply_answer, recorded);
    FIELD(runtime_changes_apply_answer, sequence);
    SIZE(runtime_changes_apply_answer);
    FIELD(runtime_changes_reverse_argument, size);
    FIELD(runtime_changes_reverse_argument, state);
    FIELD(runtime_changes_reverse_argument, host);
    SIZE(runtime_changes_reverse_argument);
    FIELD(runtime_change_drift, sequence);
    FIELD(runtime_change_drift, kind);
    FIELD(runtime_change_drift, detail);
    SIZE(runtime_change_drift);
    FIELD(runtime_changes_reverse_answer, size);
    FIELD(runtime_changes_reverse_answer, reversed);
    FIELD(runtime_changes_reverse_answer, drifted);
    FIELD(runtime_changes_reverse_answer, drifted_count);
    FIELD(runtime_changes_reverse_answer, storage);
    SIZE(runtime_changes_reverse_answer);
    FIELD(runtime_changes_definition, size);
    FIELD(runtime_changes_definition, context);
    FIELD(runtime_changes_definition, apply);
    FIELD(runtime_changes_definition, reverse);
    FIELD(runtime_changes_definition, release_reverse_answer);
    FIELD(runtime_changes_definition, release_refusal);
    SIZE(runtime_changes_definition);
}

static void print_commands_layouts(void) {
    FIELD(runtime_command_argument, size);
    FIELD(runtime_command_argument, state);
    FIELD(runtime_command_argument, host);
    FIELD(runtime_command_argument, home);
    FIELD(runtime_command_argument, name);
    FIELD(runtime_command_argument, target);
    FIELD(runtime_command_argument, arguments);
    FIELD(runtime_command_argument, argument_count);
    FIELD(runtime_command_argument, environment);
    FIELD(runtime_command_argument, environment_count);
    SIZE(runtime_command_argument);
    FIELD(runtime_commands_rename_argument, size);
    FIELD(runtime_commands_rename_argument, state);
    FIELD(runtime_commands_rename_argument, host);
    FIELD(runtime_commands_rename_argument, from);
    FIELD(runtime_commands_rename_argument, to);
    FIELD(runtime_commands_rename_argument, environment);
    FIELD(runtime_commands_rename_argument, environment_count);
    SIZE(runtime_commands_rename_argument);
    FIELD(runtime_commands_delete_argument, size);
    FIELD(runtime_commands_delete_argument, state);
    FIELD(runtime_commands_delete_argument, host);
    FIELD(runtime_commands_delete_argument, name);
    SIZE(runtime_commands_delete_argument);
    FIELD(runtime_command_answer, size);
    FIELD(runtime_command_answer, name);
    FIELD(runtime_command_answer, storage);
    SIZE(runtime_command_answer);
    FIELD(runtime_commands_definition, size);
    FIELD(runtime_commands_definition, context);
    FIELD(runtime_commands_definition, request);
    FIELD(runtime_commands_definition, rename);
    FIELD(runtime_commands_definition, remove);
    FIELD(runtime_commands_definition, add);
    FIELD(runtime_commands_definition, release_answer);
    FIELD(runtime_commands_definition, release_refusal);
    SIZE(runtime_commands_definition);
}

static void print_lists_layouts(void) {
    FIELD(runtime_lists_plugins_argument, size);
    FIELD(runtime_lists_plugins_argument, installed);
    FIELD(runtime_lists_plugins_argument, detected);
    FIELD(runtime_lists_plugins_argument, available);
    SIZE(runtime_lists_plugins_argument);
    FIELD(runtime_list_detection, item);
    FIELD(runtime_list_detection, installed);
    FIELD(runtime_list_detection, refused);
    SIZE(runtime_list_detection);
    FIELD(runtime_lists_applications_argument, size);
    FIELD(runtime_lists_applications_argument, items);
    FIELD(runtime_lists_applications_argument, state);
    FIELD(runtime_lists_applications_argument, environment);
    FIELD(runtime_lists_applications_argument, environment_count);
    FIELD(runtime_lists_applications_argument, detections);
    FIELD(runtime_lists_applications_argument, detection_count);
    SIZE(runtime_lists_applications_argument);
    FIELD(runtime_list_item, id);
    FIELD(runtime_list_item, state);
    FIELD(runtime_list_item, label);
    FIELD(runtime_list_item, row);
    SIZE(runtime_list_item);
    FIELD(runtime_list, size);
    FIELD(runtime_list, items);
    FIELD(runtime_list, item_count);
    FIELD(runtime_list, storage);
    SIZE(runtime_list);
    FIELD(runtime_lists_definition, size);
    FIELD(runtime_lists_definition, context);
    FIELD(runtime_lists_definition, plugins);
    FIELD(runtime_lists_definition, applications);
    FIELD(runtime_lists_definition, release_list);
    FIELD(runtime_lists_definition, release_refusal);
    SIZE(runtime_lists_definition);
}

static void print_marketplace_layouts(void) {
    FIELD(runtime_marketplace_build_argument, size);
    FIELD(runtime_marketplace_build_argument, list);
    FIELD(runtime_marketplace_build_argument, built);
    FIELD(runtime_marketplace_build_argument, previous);
    FIELD(runtime_marketplace_build_argument, token);
    FIELD(runtime_marketplace_build_argument, token_host);
    FIELD(runtime_marketplace_build_argument, api_base);
    SIZE(runtime_marketplace_build_argument);
    FIELD(runtime_marketplace_skipped_entry, id);
    FIELD(runtime_marketplace_skipped_entry, reason);
    SIZE(runtime_marketplace_skipped_entry);
    FIELD(runtime_marketplace_build_answer, size);
    FIELD(runtime_marketplace_build_answer, copy);
    FIELD(runtime_marketplace_build_answer, skipped);
    FIELD(runtime_marketplace_build_answer, skipped_count);
    FIELD(runtime_marketplace_build_answer, storage);
    SIZE(runtime_marketplace_build_answer);
    FIELD(runtime_marketplace_sources_argument, size);
    FIELD(runtime_marketplace_sources_argument, settings);
    SIZE(runtime_marketplace_sources_argument);
    FIELD(runtime_marketplace_source, id);
    FIELD(runtime_marketplace_source, kind);
    FIELD(runtime_marketplace_source, url);
    FIELD(runtime_marketplace_source, provider);
    SIZE(runtime_marketplace_source);
    FIELD(runtime_marketplace_skipped_source, index);
    FIELD(runtime_marketplace_skipped_source, detail);
    SIZE(runtime_marketplace_skipped_source);
    FIELD(runtime_marketplace_sources_answer, size);
    FIELD(runtime_marketplace_sources_answer, sources);
    FIELD(runtime_marketplace_sources_answer, source_count);
    FIELD(runtime_marketplace_sources_answer, skipped);
    FIELD(runtime_marketplace_sources_answer, skipped_count);
    FIELD(runtime_marketplace_sources_answer, storage);
    SIZE(runtime_marketplace_sources_answer);
    FIELD(runtime_marketplace_read_argument, size);
    FIELD(runtime_marketplace_read_argument, id);
    FIELD(runtime_marketplace_read_argument, url);
    FIELD(runtime_marketplace_read_argument, text);
    FIELD(runtime_marketplace_read_argument, destination);
    SIZE(runtime_marketplace_read_argument);
    FIELD(runtime_marketplace_copy_answer, size);
    FIELD(runtime_marketplace_copy_answer, copy);
    FIELD(runtime_marketplace_copy_answer, storage);
    SIZE(runtime_marketplace_copy_answer);
    FIELD(runtime_marketplace_copy, source);
    FIELD(runtime_marketplace_copy, copy);
    SIZE(runtime_marketplace_copy);
    FIELD(runtime_marketplace_lookup_argument, size);
    FIELD(runtime_marketplace_lookup_argument, copies);
    FIELD(runtime_marketplace_lookup_argument, copy_count);
    FIELD(runtime_marketplace_lookup_argument, name);
    SIZE(runtime_marketplace_lookup_argument);
    FIELD(runtime_marketplace_lookup_answer, size);
    FIELD(runtime_marketplace_lookup_answer, match);
    FIELD(runtime_marketplace_lookup_answer, id);
    FIELD(runtime_marketplace_lookup_answer, marketplace);
    FIELD(runtime_marketplace_lookup_answer, ambiguous);
    FIELD(runtime_marketplace_lookup_answer, ambiguous_count);
    FIELD(runtime_marketplace_lookup_answer, storage);
    SIZE(runtime_marketplace_lookup_answer);
    FIELD(runtime_marketplace_remember_argument, size);
    FIELD(runtime_marketplace_remember_argument, state);
    FIELD(runtime_marketplace_remember_argument, marketplace);
    FIELD(runtime_marketplace_remember_argument, name);
    FIELD(runtime_marketplace_remember_argument, id);
    FIELD(runtime_marketplace_remember_argument, accept);
    FIELD(runtime_marketplace_remember_argument, check_only);
    SIZE(runtime_marketplace_remember_argument);
    FIELD(runtime_marketplace_remember_answer, size);
    FIELD(runtime_marketplace_remember_answer, memory);
    FIELD(runtime_marketplace_remember_answer, was);
    FIELD(runtime_marketplace_remember_answer, now);
    FIELD(runtime_marketplace_remember_answer, storage);
    SIZE(runtime_marketplace_remember_answer);
    FIELD(runtime_marketplace_definition, size);
    FIELD(runtime_marketplace_definition, context);
    FIELD(runtime_marketplace_definition, build);
    FIELD(runtime_marketplace_definition, sources);
    FIELD(runtime_marketplace_definition, read);
    FIELD(runtime_marketplace_definition, lookup);
    FIELD(runtime_marketplace_definition, remember);
    FIELD(runtime_marketplace_definition, release_build_answer);
    FIELD(runtime_marketplace_definition, release_sources_answer);
    FIELD(runtime_marketplace_definition, release_copy_answer);
    FIELD(runtime_marketplace_definition, release_lookup_answer);
    FIELD(runtime_marketplace_definition, release_remember_answer);
    FIELD(runtime_marketplace_definition, release_refusal);
    SIZE(runtime_marketplace_definition);
}

static void print_cache_layouts(void) {
    FIELD(runtime_cache_install_argument, size);
    FIELD(runtime_cache_install_argument, cache);
    FIELD(runtime_cache_install_argument, id);
    FIELD(runtime_cache_install_argument, item);
    FIELD(runtime_cache_install_argument, version);
    FIELD(runtime_cache_install_argument, platform);
    SIZE(runtime_cache_install_argument);
    FIELD(runtime_cache_install_answer, size);
    FIELD(runtime_cache_install_answer, id);
    FIELD(runtime_cache_install_answer, version);
    FIELD(runtime_cache_install_answer, directory);
    FIELD(runtime_cache_install_answer, fetched);
    FIELD(runtime_cache_install_answer, fetched_count);
    FIELD(runtime_cache_install_answer, cached);
    FIELD(runtime_cache_install_answer, cached_count);
    FIELD(runtime_cache_install_answer, storage);
    SIZE(runtime_cache_install_answer);
    FIELD(runtime_cache_place_argument, size);
    FIELD(runtime_cache_place_argument, cache);
    FIELD(runtime_cache_place_argument, platform);
    SIZE(runtime_cache_place_argument);
    FIELD(runtime_cache_versions, id);
    FIELD(runtime_cache_versions, versions);
    FIELD(runtime_cache_versions, version_count);
    SIZE(runtime_cache_versions);
    FIELD(runtime_cache_installed_answer, size);
    FIELD(runtime_cache_installed_answer, bundles);
    FIELD(runtime_cache_installed_answer, bundle_count);
    FIELD(runtime_cache_installed_answer, libraries);
    FIELD(runtime_cache_installed_answer, library_count);
    FIELD(runtime_cache_installed_answer, storage);
    SIZE(runtime_cache_installed_answer);
    FIELD(runtime_cache_check_argument, size);
    FIELD(runtime_cache_check_argument, directory);
    SIZE(runtime_cache_check_argument);
    FIELD(runtime_cache_check_answer, size);
    FIELD(runtime_cache_check_answer, verdict);
    FIELD(runtime_cache_check_answer, detail);
    FIELD(runtime_cache_check_answer, fix);
    FIELD(runtime_cache_check_answer, file);
    FIELD(runtime_cache_check_answer, storage);
    SIZE(runtime_cache_check_answer);
    FIELD(runtime_cache_damage, id);
    FIELD(runtime_cache_damage, version);
    FIELD(runtime_cache_damage, detail);
    FIELD(runtime_cache_damage, fix);
    FIELD(runtime_cache_damage, file);
    SIZE(runtime_cache_damage);
    FIELD(runtime_cache_damaged_answer, size);
    FIELD(runtime_cache_damaged_answer, damaged);
    FIELD(runtime_cache_damaged_answer, damaged_count);
    FIELD(runtime_cache_damaged_answer, storage);
    SIZE(runtime_cache_damaged_answer);
    FIELD(runtime_cache_plugin, id);
    FIELD(runtime_cache_plugin, version);
    SIZE(runtime_cache_plugin);
    FIELD(runtime_cache_locate_argument, size);
    FIELD(runtime_cache_locate_argument, cache);
    FIELD(runtime_cache_locate_argument, plugins);
    FIELD(runtime_cache_locate_argument, plugin_count);
    SIZE(runtime_cache_locate_argument);
    FIELD(runtime_cache_located, id);
    FIELD(runtime_cache_located, version);
    FIELD(runtime_cache_located, directory);
    FIELD(runtime_cache_located, manifest);
    FIELD(runtime_cache_located, entry);
    SIZE(runtime_cache_located);
    FIELD(runtime_cache_locate_answer, size);
    FIELD(runtime_cache_locate_answer, located);
    FIELD(runtime_cache_locate_answer, located_count);
    FIELD(runtime_cache_locate_answer, problems);
    FIELD(runtime_cache_locate_answer, problem_count);
    FIELD(runtime_cache_locate_answer, storage);
    SIZE(runtime_cache_locate_answer);
    FIELD(runtime_cache_platform_answer, size);
    FIELD(runtime_cache_platform_answer, platform);
    FIELD(runtime_cache_platform_answer, storage);
    SIZE(runtime_cache_platform_answer);
    FIELD(runtime_cache_sweep_argument, size);
    FIELD(runtime_cache_sweep_argument, cache);
    FIELD(runtime_cache_sweep_argument, state);
    SIZE(runtime_cache_sweep_argument);
    FIELD(runtime_cache_sweep_answer, size);
    FIELD(runtime_cache_sweep_answer, removed);
    FIELD(runtime_cache_sweep_answer, removed_count);
    FIELD(runtime_cache_sweep_answer, storage);
    SIZE(runtime_cache_sweep_answer);
    FIELD(runtime_cache_deploy_argument, size);
    FIELD(runtime_cache_deploy_argument, directory);
    FIELD(runtime_cache_deploy_argument, plugin_directory);
    FIELD(runtime_cache_deploy_argument, id);
    FIELD(runtime_cache_deploy_argument, name);
    FIELD(runtime_cache_deploy_argument, replaces);
    SIZE(runtime_cache_deploy_argument);
    FIELD(runtime_cache_deploy_answer, size);
    FIELD(runtime_cache_deploy_answer, import_name);
    FIELD(runtime_cache_deploy_answer, storage);
    SIZE(runtime_cache_deploy_answer);
    FIELD(runtime_cache_definition, size);
    FIELD(runtime_cache_definition, context);
    FIELD(runtime_cache_definition, install);
    FIELD(runtime_cache_definition, installed);
    FIELD(runtime_cache_definition, check);
    FIELD(runtime_cache_definition, damaged);
    FIELD(runtime_cache_definition, locate);
    FIELD(runtime_cache_definition, platform);
    FIELD(runtime_cache_definition, sweep);
    FIELD(runtime_cache_definition, deploy);
    FIELD(runtime_cache_definition, release_install_answer);
    FIELD(runtime_cache_definition, release_installed_answer);
    FIELD(runtime_cache_definition, release_check_answer);
    FIELD(runtime_cache_definition, release_damaged_answer);
    FIELD(runtime_cache_definition, release_locate_answer);
    FIELD(runtime_cache_definition, release_platform_answer);
    FIELD(runtime_cache_definition, release_sweep_answer);
    FIELD(runtime_cache_definition, release_deploy_answer);
    FIELD(runtime_cache_definition, release_refusal);
    SIZE(runtime_cache_definition);
}

static void print_catalog_layouts(void) {
    FIELD(runtime_catalog_source, kind);
    FIELD(runtime_catalog_source, location);
    SIZE(runtime_catalog_source);
    FIELD(runtime_catalog_read_argument, size);
    FIELD(runtime_catalog_read_argument, text);
    FIELD(runtime_catalog_read_argument, origin);
    SIZE(runtime_catalog_read_argument);
    FIELD(runtime_catalog_answer, size);
    FIELD(runtime_catalog_answer, catalog);
    FIELD(runtime_catalog_answer, storage);
    SIZE(runtime_catalog_answer);
    FIELD(runtime_catalog_sources_argument, size);
    FIELD(runtime_catalog_sources_argument, settings);
    SIZE(runtime_catalog_sources_argument);
    FIELD(runtime_catalog_skipped, index);
    FIELD(runtime_catalog_skipped, detail);
    SIZE(runtime_catalog_skipped);
    FIELD(runtime_catalog_sources_answer, size);
    FIELD(runtime_catalog_sources_answer, sources);
    FIELD(runtime_catalog_sources_answer, source_count);
    FIELD(runtime_catalog_sources_answer, skipped);
    FIELD(runtime_catalog_sources_answer, skipped_count);
    FIELD(runtime_catalog_sources_answer, storage);
    SIZE(runtime_catalog_sources_answer);
    FIELD(runtime_catalog_reading, source);
    FIELD(runtime_catalog_reading, catalog);
    SIZE(runtime_catalog_reading);
    FIELD(runtime_catalog_merge_argument, size);
    FIELD(runtime_catalog_merge_argument, catalogs);
    FIELD(runtime_catalog_merge_argument, catalog_count);
    SIZE(runtime_catalog_merge_argument);
    FIELD(runtime_catalog_shadowed, id);
    FIELD(runtime_catalog_shadowed, source);
    SIZE(runtime_catalog_shadowed);
    FIELD(runtime_catalog_merge_answer, size);
    FIELD(runtime_catalog_merge_answer, catalog);
    FIELD(runtime_catalog_merge_answer, shadowed);
    FIELD(runtime_catalog_merge_answer, shadowed_count);
    FIELD(runtime_catalog_merge_answer, storage);
    SIZE(runtime_catalog_merge_answer);
    FIELD(runtime_catalog_check_argument, size);
    FIELD(runtime_catalog_check_argument, source);
    FIELD(runtime_catalog_check_argument, cache);
    FIELD(runtime_catalog_check_argument, now);
    FIELD(runtime_catalog_check_argument, interval_hours);
    SIZE(runtime_catalog_check_argument);
    FIELD(runtime_catalog_check_answer, size);
    FIELD(runtime_catalog_check_answer, kind);
    FIELD(runtime_catalog_check_answer, catalog);
    FIELD(runtime_catalog_check_answer, detail);
    FIELD(runtime_catalog_check_answer, storage);
    SIZE(runtime_catalog_check_answer);
    FIELD(runtime_catalog_current_argument, size);
    FIELD(runtime_catalog_current_argument, sources);
    FIELD(runtime_catalog_current_argument, source_count);
    FIELD(runtime_catalog_current_argument, cache);
    SIZE(runtime_catalog_current_argument);
    FIELD(runtime_catalog_enabled, id);
    FIELD(runtime_catalog_enabled, pin);
    SIZE(runtime_catalog_enabled);
    FIELD(runtime_catalog_installed, id);
    FIELD(runtime_catalog_installed, versions);
    FIELD(runtime_catalog_installed, version_count);
    SIZE(runtime_catalog_installed);
    FIELD(runtime_catalog_resolve_argument, size);
    FIELD(runtime_catalog_resolve_argument, catalog);
    FIELD(runtime_catalog_resolve_argument, enabled);
    FIELD(runtime_catalog_resolve_argument, enabled_count);
    FIELD(runtime_catalog_resolve_argument, installed);
    FIELD(runtime_catalog_resolve_argument, installed_count);
    FIELD(runtime_catalog_resolve_argument, kind);
    SIZE(runtime_catalog_resolve_argument);
    FIELD(runtime_catalog_version, id);
    FIELD(runtime_catalog_version, version);
    SIZE(runtime_catalog_version);
    FIELD(runtime_catalog_resolve_answer, size);
    FIELD(runtime_catalog_resolve_answer, run);
    FIELD(runtime_catalog_resolve_answer, run_count);
    FIELD(runtime_catalog_resolve_answer, fetch);
    FIELD(runtime_catalog_resolve_answer, fetch_count);
    FIELD(runtime_catalog_resolve_answer, storage);
    SIZE(runtime_catalog_resolve_answer);
    FIELD(runtime_catalog_definition, size);
    FIELD(runtime_catalog_definition, context);
    FIELD(runtime_catalog_definition, read);
    FIELD(runtime_catalog_definition, sources);
    FIELD(runtime_catalog_definition, merge);
    FIELD(runtime_catalog_definition, check);
    FIELD(runtime_catalog_definition, current);
    FIELD(runtime_catalog_definition, resolve);
    FIELD(runtime_catalog_definition, release_answer);
    FIELD(runtime_catalog_definition, release_sources_answer);
    FIELD(runtime_catalog_definition, release_merge_answer);
    FIELD(runtime_catalog_definition, release_check_answer);
    FIELD(runtime_catalog_definition, release_resolve_answer);
    FIELD(runtime_catalog_definition, release_refusal);
    SIZE(runtime_catalog_definition);
}

static void print_install_layouts(void) {
    FIELD(runtime_install_warnings_argument, size);
    FIELD(runtime_install_warnings_argument, manifest);
    SIZE(runtime_install_warnings_argument);
    FIELD(runtime_install_warning, kind);
    FIELD(runtime_install_warning, subject);
    FIELD(runtime_install_warning, detail);
    SIZE(runtime_install_warning);
    FIELD(runtime_install_warnings_answer, size);
    FIELD(runtime_install_warnings_answer, warnings);
    FIELD(runtime_install_warnings_answer, warning_count);
    FIELD(runtime_install_warnings_answer, storage);
    SIZE(runtime_install_warnings_answer);
    FIELD(runtime_install_packages_argument, size);
    FIELD(runtime_install_packages_argument, portable);
    FIELD(runtime_install_packages_argument, item);
    FIELD(runtime_install_packages_argument, manifest_packages);
    SIZE(runtime_install_packages_argument);
    FIELD(runtime_install_package, method);
    FIELD(runtime_install_package, name);
    FIELD(runtime_install_package, options);
    SIZE(runtime_install_package);
    FIELD(runtime_install_packages_answer, size);
    FIELD(runtime_install_packages_answer, packages);
    FIELD(runtime_install_packages_answer, package_count);
    FIELD(runtime_install_packages_answer, storage);
    SIZE(runtime_install_packages_answer);
    FIELD(runtime_install_plan_argument, size);
    FIELD(runtime_install_plan_argument, portable);
    FIELD(runtime_install_plan_argument, item);
    FIELD(runtime_install_plan_argument, manifest_packages);
    FIELD(runtime_install_plan_argument, methods);
    FIELD(runtime_install_plan_argument, method_count);
    FIELD(runtime_install_plan_argument, chosen);
    FIELD(runtime_install_plan_argument, installed_providers);
    FIELD(runtime_install_plan_argument, installed_provider_count);
    FIELD(runtime_install_plan_argument, catalog);
    SIZE(runtime_install_plan_argument);
    FIELD(runtime_install_step, kind);
    FIELD(runtime_install_step, provider);
    FIELD(runtime_install_step, package);
    SIZE(runtime_install_step);
    FIELD(runtime_install_plan_answer, size);
    FIELD(runtime_install_plan_answer, steps);
    FIELD(runtime_install_plan_answer, step_count);
    FIELD(runtime_install_plan_answer, storage);
    SIZE(runtime_install_plan_answer);
    FIELD(runtime_install_detect_argument, size);
    FIELD(runtime_install_detect_argument, portable);
    FIELD(runtime_install_detect_argument, cache);
    FIELD(runtime_install_detect_argument, item);
    FIELD(runtime_install_detect_argument, manifest_packages);
    SIZE(runtime_install_detect_argument);
    FIELD(runtime_install_detection, item);
    FIELD(runtime_install_detection, package);
    FIELD(runtime_install_detection, installed);
    FIELD(runtime_install_detection, version);
    FIELD(runtime_install_detection, refused);
    SIZE(runtime_install_detection);
    FIELD(runtime_install_detect_answer, size);
    FIELD(runtime_install_detect_answer, detections);
    FIELD(runtime_install_detect_answer, detection_count);
    FIELD(runtime_install_detect_answer, storage);
    SIZE(runtime_install_detect_answer);
    FIELD(runtime_install_definition, size);
    FIELD(runtime_install_definition, context);
    FIELD(runtime_install_definition, warnings);
    FIELD(runtime_install_definition, packages);
    FIELD(runtime_install_definition, plan);
    FIELD(runtime_install_definition, detect);
    FIELD(runtime_install_definition, release_warnings_answer);
    FIELD(runtime_install_definition, release_packages_answer);
    FIELD(runtime_install_definition, release_plan_answer);
    FIELD(runtime_install_definition, release_detect_answer);
    FIELD(runtime_install_definition, release_refusal);
    SIZE(runtime_install_definition);
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
    print_registry_layouts();
    print_changes_layouts();
    print_commands_layouts();
    print_lists_layouts();
    print_marketplace_layouts();
    print_cache_layouts();
    print_catalog_layouts();
    print_install_layouts();
    return 0;
}
