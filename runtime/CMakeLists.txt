set(runtime_objects_sources src/segment.c src/paths.c src/merge.c src/config.c src/log.c src/places.c src/user_home.c
    src/secret_file.c src/secrets.c src/directory_tree.c src/registry.c src/change_kinds.c src/path_comparison.c
    src/changes.c
    src/hosts_lock.c src/install.c src/json_span.c src/json_style.c src/json_edit.c src/json_key_change.c
    src/sha256.c src/file_stream.c src/identity_parse.c src/install_warnings.c src/import_name.c src/json_service.c src/catalog_answer.c src/marketplace_check.c
    src/definition_support.c src/answer_storage.c src/paths_definition.c src/places_definition.c
    src/secrets_definition.c src/config_conversion.c src/config_definition.c src/settings_definition.c
    src/log_definition.c src/registry_definition.c src/catalog_time.c src/file_times.c src/thread.c src/atomic.c
    src/process_environment.c)
add_library(runtime_objects OBJECT ${runtime_objects_sources})
target_include_directories(runtime_objects PUBLIC src)
target_include_directories(runtime_objects PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_objects PUBLIC spisor_library)
set_target_properties(runtime_objects PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_objects)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(runtime_objects PUBLIC Threads::Threads)
endif()

add_library(runtime_objects_config_test_hooks OBJECT ${runtime_objects_sources})
target_include_directories(runtime_objects_config_test_hooks PUBLIC src)
target_include_directories(runtime_objects_config_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_objects_config_test_hooks PUBLIC spisor_library)
target_compile_definitions(runtime_objects_config_test_hooks PRIVATE SPISOR_CONFIG_TEST_HOOKS)
spisor_strict_warnings(runtime_objects_config_test_hooks)
set_target_properties(runtime_objects_config_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

set(runtime_change_reversal_sources src/change_reversal.c src/changes_definition.c)
add_library(runtime_change_reversal OBJECT ${runtime_change_reversal_sources})
target_link_libraries(runtime_change_reversal PUBLIC runtime_objects)
target_include_directories(runtime_change_reversal PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_change_reversal PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_change_reversal)

add_library(runtime_change_reversal_test_hooks OBJECT ${runtime_change_reversal_sources})
target_link_libraries(runtime_change_reversal_test_hooks PUBLIC runtime_objects)
target_include_directories(runtime_change_reversal_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(runtime_change_reversal_test_hooks PRIVATE SPISOR_CHANGES_TEST_HOOKS)
spisor_strict_warnings(runtime_change_reversal_test_hooks)
set_target_properties(runtime_change_reversal_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

add_library(runtime_commands OBJECT src/commands.c src/commands_definition.c)
target_link_libraries(runtime_commands PUBLIC runtime_change_reversal)
target_include_directories(runtime_commands PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_commands PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_commands)

if(WIN32)
    set(runtime_path_entry_source src/path_entry_windows.c)
    set(runtime_path_entry_test_source src/path_entry_windows.c src/path_entry_test_value_windows.c)
    set(runtime_path_entry_libraries Advapi32 User32)
else()
    set(runtime_path_entry_source src/path_entry_posix.c)
    set(runtime_path_entry_test_source src/path_entry_posix.c)
    set(runtime_path_entry_libraries "")
endif()
add_library(runtime_path_entry OBJECT ${runtime_path_entry_source})
target_include_directories(runtime_path_entry PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_path_entry PUBLIC runtime_objects)
set_target_properties(runtime_path_entry PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_path_entry)

add_library(runtime_path_entry_test_hooks OBJECT ${runtime_path_entry_test_source})
target_include_directories(runtime_path_entry_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_path_entry_test_hooks PUBLIC runtime_objects)
target_compile_definitions(runtime_path_entry_test_hooks PRIVATE SPISOR_PATH_ENTRY_TEST_HOOKS)
spisor_strict_warnings(runtime_path_entry_test_hooks)
set_target_properties(runtime_path_entry_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

if(WIN32)
    set(runtime_secret_store_source src/secret_windows.c)
    set(runtime_secret_store_libraries Advapi32)
elseif(APPLE)
    set(runtime_secret_store_source src/secret_macos.c)
    set(runtime_secret_store_libraries "-framework Security" "-framework CoreFoundation")
else()
    set(runtime_secret_store_source src/secret_linux.c)
    find_package(Threads REQUIRED)
    set(runtime_secret_store_libraries ${CMAKE_DL_LIBS} Threads::Threads)
endif()
add_library(runtime_secret_store OBJECT ${runtime_secret_store_source})
target_include_directories(runtime_secret_store PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_include_directories(runtime_secret_store PUBLIC src)
set_target_properties(runtime_secret_store PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_secret_store)

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(runtime_secret_store_test_hooks OBJECT ${runtime_secret_store_source})
    target_include_directories(runtime_secret_store_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include)
    target_include_directories(runtime_secret_store_test_hooks PUBLIC src)
    target_compile_definitions(runtime_secret_store_test_hooks PRIVATE SPISOR_SECRET_TEST_HOOKS)
    spisor_strict_warnings(runtime_secret_store_test_hooks)
    set_target_properties(runtime_secret_store_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)
endif()

if(WIN32)
    set(runtime_fetch_transport_source src/fetch_windows.c)
    set(runtime_fetch_libraries Winhttp Shlwapi)
else()
    set(runtime_fetch_transport_source src/fetch_curl.c)
    find_package(Threads REQUIRED)
    set(runtime_fetch_libraries ${CMAKE_DL_LIBS} Threads::Threads)
endif()
set(runtime_fetch_sources src/fetch.c src/fetch_sink.c src/fetch_file.c ${runtime_fetch_transport_source})
add_library(runtime_fetch OBJECT ${runtime_fetch_sources})
target_include_directories(runtime_fetch PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_fetch PUBLIC runtime_objects)
set_target_properties(runtime_fetch PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_fetch)

add_library(runtime_fetch_test_hooks OBJECT ${runtime_fetch_sources})
target_include_directories(runtime_fetch_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_fetch_test_hooks PUBLIC runtime_objects)
target_compile_definitions(runtime_fetch_test_hooks PRIVATE SPISOR_FETCH_TEST_HOOKS)
spisor_strict_warnings(runtime_fetch_test_hooks)
set_target_properties(runtime_fetch_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

add_library(runtime_identity_resolve OBJECT src/identity_resolve.c)
target_link_libraries(runtime_identity_resolve PUBLIC runtime_objects)
target_include_directories(runtime_identity_resolve PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_identity_resolve PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_identity_resolve)

add_library(runtime_identity_also_at OBJECT src/identity_also_at.c src/identity_definition.c)
target_link_libraries(runtime_identity_also_at PUBLIC runtime_objects)
target_include_directories(runtime_identity_also_at PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_identity_also_at PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_identity_also_at)

set(runtime_catalog_sources src/catalog.c src/catalog_check.c src/catalog_definition.c src/catalog_patterns.c src/catalog_schema.c src/catalog_fold.c
    src/catalog_fold_table.c src/catalog_sources.c src/catalog_path.c src/catalog_read.c src/catalog_listed_source.c
    src/catalog_resolve.c src/catalog_semver.c src/catalog_source.c src/catalog_state.c src/catalog_text.c
    src/catalog_url.c src/catalog_url_host.c)
add_library(runtime_install_by_id OBJECT src/install_by_id.c)
target_link_libraries(runtime_install_by_id PUBLIC runtime_objects)
target_include_directories(runtime_install_by_id PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_install_by_id PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_install_by_id)

add_library(runtime_marketplace_build OBJECT src/marketplace_build.c)
target_include_directories(runtime_marketplace_build PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_marketplace_build PUBLIC runtime_objects)
set_target_properties(runtime_marketplace_build PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_marketplace_build)

add_library(runtime_marketplace_reading OBJECT src/marketplace_sources.c src/marketplace_read.c src/marketplace_name_lookup.c src/marketplace_memory.c
    src/marketplace_definition.c)
target_link_libraries(runtime_marketplace_reading PUBLIC runtime_objects)
target_include_directories(runtime_marketplace_reading PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_marketplace_reading PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_marketplace_reading)

add_library(runtime_marketplace_reading_test_hooks OBJECT src/marketplace_sources.c src/marketplace_read.c src/marketplace_name_lookup.c src/marketplace_memory.c
    src/marketplace_definition.c)
target_link_libraries(runtime_marketplace_reading_test_hooks PUBLIC runtime_objects)
target_include_directories(runtime_marketplace_reading_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(runtime_marketplace_reading_test_hooks PRIVATE SPISOR_MARKETPLACE_TEST_HOOKS)
spisor_strict_warnings(runtime_marketplace_reading_test_hooks)
set_target_properties(runtime_marketplace_reading_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

add_library(runtime_catalog OBJECT ${runtime_catalog_sources})
target_link_libraries(runtime_catalog PUBLIC runtime_objects)
target_include_directories(runtime_catalog PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_catalog PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_catalog)

add_library(runtime_catalog_refresh OBJECT src/catalog_refresh.c)
target_link_libraries(runtime_catalog_refresh PUBLIC runtime_objects)
target_include_directories(runtime_catalog_refresh PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_catalog_refresh PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_catalog_refresh)

add_library(runtime_catalog_refresh_test_hooks OBJECT src/catalog_refresh.c)
target_link_libraries(runtime_catalog_refresh_test_hooks PUBLIC runtime_objects)
target_include_directories(runtime_catalog_refresh_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(runtime_catalog_refresh_test_hooks PRIVATE SPISOR_CATALOG_TEST_HOOKS)
spisor_strict_warnings(runtime_catalog_refresh_test_hooks)
set_target_properties(runtime_catalog_refresh_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

set(runtime_cache_sources src/boot_hint.c src/cache.c src/cache_damaged.c src/cache_folders.c src/cache_install.c src/cache_installed.c src/cache_locate.c src/cache_marker.c
    src/cache_deploy.c src/cache_definition.c src/cache_place.c src/cache_plan.c src/cache_references.c src/cache_refusal.c src/cache_store.c src/cache_sweep.c
    src/host_references.c src/node_json_file.c src/platform_key.c src/process_alive.c src/run_records.c
    src/alive_stamp.c src/definition_host_target.c)
add_library(runtime_cache OBJECT ${runtime_cache_sources})
target_link_libraries(runtime_cache PUBLIC runtime_objects)
target_include_directories(runtime_cache PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_cache PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_cache)

add_library(runtime_cache_test_hooks OBJECT ${runtime_cache_sources})
target_link_libraries(runtime_cache_test_hooks PUBLIC runtime_objects)
target_include_directories(runtime_cache_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(runtime_cache_test_hooks PRIVATE SPISOR_CACHE_TEST_HOOKS)
spisor_strict_warnings(runtime_cache_test_hooks)
set_target_properties(runtime_cache_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

set(runtime_updates_sources src/boot.c src/boot_resolve.c src/hosts_start.c src/plugin_choices.c src/updates.c
    src/updates_apply.c src/updates_catalog.c src/updates_check.c src/updates_recorded.c src/updates_rollback.c src/version_map.c)
add_library(runtime_updates OBJECT ${runtime_updates_sources})
target_link_libraries(runtime_updates PUBLIC runtime_objects)
target_include_directories(runtime_updates PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_updates PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_updates)

set(runtime_processes_sources src/holders.c src/holders_lock.c src/process_start.c src/processes.c
    src/processes_definition.c src/processes_hold.c
    src/processes_ready.c src/processes_runs.c)
if(WIN32)
    list(APPEND runtime_processes_sources src/command_line_windows.c src/windows_process.c)
endif()
add_library(runtime_processes OBJECT ${runtime_processes_sources})
target_link_libraries(runtime_processes PUBLIC runtime_objects)
target_include_directories(runtime_processes PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_processes PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_processes)

set(runtime_hosts_sources src/application_home.c src/application_provenance.c src/followed_entry.c
    src/hosts_register.c src/hosts_remove.c src/lists.c src/loader_installed.c
    src/loader_definition.c src/loader_standard.c src/loader_application_facts.c src/entry_template.c src/on_path.c
    src/lists_definition.c)
add_library(runtime_hosts OBJECT ${runtime_hosts_sources})
target_link_libraries(runtime_hosts PUBLIC runtime_objects)
target_include_directories(runtime_hosts PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_hosts PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_hosts)

add_library(runtime_hosts_definition OBJECT src/hosts_definition.c)
target_link_libraries(runtime_hosts_definition PUBLIC runtime_objects)
target_include_directories(runtime_hosts_definition PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_hosts_definition PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_hosts_definition)

set(runtime_ui_sources src/ui_action.c src/ui_binding.c src/ui_definition.c src/ui_items.c src/ui_navigation.c
    src/ui_refusal.c src/ui_render.c src/ui_screen.c src/ui_screen_definition.c src/ui_settings_view.c)
add_library(runtime_ui OBJECT ${runtime_ui_sources})
target_link_libraries(runtime_ui PUBLIC runtime_objects)
target_include_directories(runtime_ui PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_ui PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_ui)

set(runtime_install_sources src/boot_place.c src/boot_stub.c src/install_application.c src/install_applications.c
    src/install_definition.c src/install_ecosystem.c src/install_entries.c src/install_hosts.c src/install_loaders.c src/install_methods.c src/install_references.c
    src/install_register_loader.c src/install_report.c src/install_runtime.c src/install_settings.c src/install_support.c
    src/process_run.c src/boot_definition.c src/updates_definition.c src/updates_run.c)
add_library(runtime_install OBJECT ${runtime_install_sources})
target_link_libraries(runtime_install PUBLIC runtime_objects)
target_include_directories(runtime_install PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_install PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_install)

add_library(runtime_install_test_hooks OBJECT ${runtime_install_sources} src/install_method_fake.c)
target_link_libraries(runtime_install_test_hooks PUBLIC runtime_objects)
target_include_directories(runtime_install_test_hooks PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(runtime_install_test_hooks PRIVATE SPISOR_INSTALL_TEST_HOOKS)
spisor_strict_warnings(runtime_install_test_hooks)
set_target_properties(runtime_install_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)

set(runtime_standard_sources src/standard_refusal.c src/scope_host.c src/scope_manifest.c src/plugin_scope.c
    src/scope_config.c src/scope_secrets.c src/scope_paths.c src/scope_log.c src/log_queue.c src/log_writer.c
    src/standard_scope.c)
add_library(runtime_standard OBJECT ${runtime_standard_sources})
target_link_libraries(runtime_standard PUBLIC runtime_objects weivel_standard)
target_include_directories(runtime_standard PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_standard PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_standard)

add_library(runtime_standard_host OBJECT src/host_run.c src/host_run_progress.c src/standard_host.c)
target_link_libraries(runtime_standard_host PUBLIC runtime_objects weivel_standard)
target_include_directories(runtime_standard_host PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_standard_host PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_standard_host)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Whether a definition is exported follows membership here, never which object library holds its source.
set(runtime_shipped_object_libraries runtime_objects runtime_change_reversal runtime_commands runtime_path_entry runtime_secret_store
    runtime_fetch runtime_identity_resolve runtime_identity_also_at runtime_install_by_id runtime_marketplace_build
    runtime_marketplace_reading runtime_catalog runtime_catalog_refresh runtime_cache runtime_updates runtime_processes
    runtime_hosts runtime_hosts_definition runtime_ui runtime_install runtime_standard runtime_standard_host)
set(runtime_shipped_objects "")
foreach(library IN LISTS runtime_shipped_object_libraries)
    target_compile_definitions(${library} PRIVATE RUNTIME_BUILDING)
    list(APPEND runtime_shipped_objects $<TARGET_OBJECTS:${library}>)
endforeach()

# The headers name every symbol runtime exports, a definition's getter and its description, for the tests that check
# the exports and for the macOS linker, which exports only these.
file(GLOB runtime_public_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/runtime/*.h)
set(runtime_definition_symbols "")
set(runtime_definition_header_count 0)
set(runtime_definition_getter_header_count 0)
set(runtime_definition_bodies "")
set(runtime_definition_getters "")
foreach(header IN LISTS runtime_public_headers)
    file(READ ${header} header_text)
    get_filename_component(header_name ${header} NAME)
    string(REGEX MATCHALL "typedef[ \t\r\n]+struct[ \t\r\n]+runtime_[a-z_]+_definition[ \t\r\n]*{" bodies
        "${header_text}")
    foreach(body IN LISTS bodies)
        string(REGEX MATCH "runtime_[a-z_]+_definition" struct "${body}")
        list(APPEND runtime_definition_bodies "${header_name} ${struct}")
    endforeach()
    if(header_text MATCHES "#include \"runtime/definition.h\"")
        math(EXPR runtime_definition_header_count "${runtime_definition_header_count} + 1")
    endif()
    string(REGEX MATCHALL "runtime_[a-z_]+_definition_of[ \t\r\n]*[(][ \t\r\n]*void[ \t\r\n]*[)]" declarations
        "${header_text}")
    foreach(declaration IN LISTS declarations)
        string(REGEX MATCH "runtime_[a-z_]+_definition_of" symbol "${declaration}")
        list(APPEND runtime_definition_symbols ${symbol})
        list(APPEND runtime_definition_getters "${header_name} ${symbol}")
    endforeach()
    if(declarations)
        math(EXPR runtime_definition_getter_header_count "${runtime_definition_getter_header_count} + 1")
    endif()
endforeach()
list(REMOVE_DUPLICATES runtime_definition_symbols)
set(runtime_exported_symbols ${runtime_definition_symbols})
foreach(symbol IN LISTS runtime_definition_symbols)
    string(REGEX REPLACE "_of$" "_description" description_symbol "${symbol}")
    list(APPEND runtime_exported_symbols ${description_symbol})
endforeach()

add_library(runtime SHARED src/library_load.c ${runtime_shipped_objects})
target_compile_definitions(runtime PRIVATE RUNTIME_BUILDING)
target_include_directories(runtime PRIVATE src ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime PRIVATE spisor_library spisor_allocator ${runtime_secret_store_libraries} ${runtime_path_entry_libraries}
    ${runtime_fetch_libraries})
set_target_properties(runtime PROPERTIES C_VISIBILITY_PRESET hidden)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(runtime PRIVATE -Wl,--exclude-libs,ALL)
elseif(APPLE)
    # ld64 has no --exclude-libs, and weivel/standard's archive gives its API default visibility, so only a list keeps
    # what runtime links statically out of its exports.
    list(TRANSFORM runtime_exported_symbols PREPEND "_" OUTPUT_VARIABLE runtime_macho_exports)
    string(JOIN "\n" runtime_macho_export_lines ${runtime_macho_exports})
    set(runtime_export_list ${CMAKE_CURRENT_BINARY_DIR}/runtime_exports.txt)
    file(CONFIGURE OUTPUT ${runtime_export_list} CONTENT "${runtime_macho_export_lines}\n")
    target_link_options(runtime PRIVATE "LINKER:-exported_symbols_list,${runtime_export_list}")
    set_property(TARGET runtime APPEND PROPERTY LINK_DEPENDS ${runtime_export_list})
endif()
spisor_strict_warnings(runtime)

# Each symbol is its definition's name with `_description`, the name weivel_describe's DESCRIBED_ELSEWHERE expects, so
# a library nesting one of runtime's definitions finds its description without being told the symbol.
file(GLOB runtime_described_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/runtime/*.h)
list(SORT runtime_described_headers)
weivel_describe(runtime
    HEADERS ${PROJECT_SOURCE_DIR}/include/spisor/status.h ${runtime_described_headers}
    PREFIX runtime_described_
    EXPORT RUNTIME_DEFINITION_API
    STATUS spisor_status
    REFUSAL spisor_refusal
    VALUE spisor_value
    TREE spisor_value_tree
    SERVICE runtime_paths_definition SYMBOL runtime_paths_definition_description
    SERVICE runtime_identity_definition SYMBOL runtime_identity_definition_description
    SERVICE runtime_marketplace_definition SYMBOL runtime_marketplace_definition_description
    SERVICE runtime_places_definition SYMBOL runtime_places_definition_description
    SERVICE runtime_config_definition SYMBOL runtime_config_definition_description
    SERVICE runtime_settings_definition SYMBOL runtime_settings_definition_description
    SERVICE runtime_log_definition SYMBOL runtime_log_definition_description
    SERVICE runtime_secrets_definition SYMBOL runtime_secrets_definition_description
    SERVICE runtime_registry_definition SYMBOL runtime_registry_definition_description
    SERVICE runtime_changes_definition SYMBOL runtime_changes_definition_description
    SERVICE runtime_commands_definition SYMBOL runtime_commands_definition_description
    SERVICE runtime_install_definition SYMBOL runtime_install_definition_description
    SERVICE runtime_fetch_definition SYMBOL runtime_fetch_definition_description
    SERVICE runtime_catalog_definition SYMBOL runtime_catalog_definition_description
    SERVICE runtime_cache_definition SYMBOL runtime_cache_definition_description
    SERVICE runtime_updates_definition SYMBOL runtime_updates_definition_description
    SERVICE runtime_boot_definition SYMBOL runtime_boot_definition_description
    SERVICE runtime_processes_definition SYMBOL runtime_processes_definition_description
    SERVICE runtime_hosts_definition SYMBOL runtime_hosts_definition_description
    SERVICE runtime_loader_definition SYMBOL runtime_loader_definition_description
    SERVICE runtime_detect_definition SYMBOL runtime_detect_definition_description
    SERVICE runtime_lists_definition SYMBOL runtime_lists_definition_description
    SERVICE runtime_ui_definition SYMBOL runtime_ui_definition_description)
target_link_libraries(runtime PRIVATE weivel_standard)

if(WIN32)
    add_executable(runtime_launcher src/launcher_windows.c src/command_line_windows.c)
    set_target_properties(runtime_launcher PROPERTIES OUTPUT_NAME runtime-launcher)
    spisor_strict_warnings(runtime_launcher)
    add_dependencies(runtime runtime_launcher)
endif()

add_subdirectory(test)

set(runtime_executable_libraries ${runtime_secret_store_libraries} ${runtime_path_entry_libraries} ${runtime_fetch_libraries}
    PARENT_SCOPE)
