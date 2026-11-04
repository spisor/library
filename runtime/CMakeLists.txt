add_library(runtime_objects OBJECT src/segment.c src/paths.c src/merge.c src/config.c src/log.c src/places.c
    src/secret_file.c src/secrets.c src/directory_tree.c src/registry.c src/change_kinds.c src/changes.c
    src/hosts_lock.c src/commands.c src/install.c src/json_span.c src/json_style.c src/json_edit.c src/json_key_change.c
    src/sha256.c src/file_stream.c src/service_call.c)
target_include_directories(runtime_objects PUBLIC src)
target_include_directories(runtime_objects PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_objects PUBLIC spisor_library)
set_target_properties(runtime_objects PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_objects)

add_library(runtime_change_reversal OBJECT src/change_reversal.c)
target_link_libraries(runtime_change_reversal PUBLIC runtime_objects)
set_target_properties(runtime_change_reversal PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_change_reversal)

add_library(runtime_change_reversal_test_hooks OBJECT src/change_reversal.c)
target_link_libraries(runtime_change_reversal_test_hooks PUBLIC runtime_objects)
target_compile_definitions(runtime_change_reversal_test_hooks PRIVATE SPISOR_CHANGES_TEST_HOOKS)
spisor_strict_warnings(runtime_change_reversal_test_hooks)

if(WIN32)
    set(runtime_path_entry_source src/path_entry_windows.c)
    set(runtime_path_entry_libraries Advapi32 User32)
else()
    set(runtime_path_entry_source src/path_entry_posix.c)
    set(runtime_path_entry_libraries "")
endif()
add_library(runtime_path_entry OBJECT ${runtime_path_entry_source})
target_link_libraries(runtime_path_entry PUBLIC runtime_objects)
set_target_properties(runtime_path_entry PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_path_entry)

add_library(runtime_path_entry_test_hooks OBJECT ${runtime_path_entry_source})
target_link_libraries(runtime_path_entry_test_hooks PUBLIC runtime_objects)
target_compile_definitions(runtime_path_entry_test_hooks PRIVATE SPISOR_PATH_ENTRY_TEST_HOOKS)
spisor_strict_warnings(runtime_path_entry_test_hooks)

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
target_include_directories(runtime_secret_store PUBLIC src)
set_target_properties(runtime_secret_store PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_secret_store)

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(runtime_secret_store_test_hooks OBJECT ${runtime_secret_store_source})
    target_include_directories(runtime_secret_store_test_hooks PUBLIC src)
    target_compile_definitions(runtime_secret_store_test_hooks PRIVATE SPISOR_SECRET_TEST_HOOKS)
    spisor_strict_warnings(runtime_secret_store_test_hooks)
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
target_link_libraries(runtime_fetch PUBLIC runtime_objects)
set_target_properties(runtime_fetch PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_fetch)

add_library(runtime_fetch_test_hooks OBJECT ${runtime_fetch_sources})
target_link_libraries(runtime_fetch_test_hooks PUBLIC runtime_objects)
target_compile_definitions(runtime_fetch_test_hooks PRIVATE SPISOR_FETCH_TEST_HOOKS)
spisor_strict_warnings(runtime_fetch_test_hooks)

set(runtime_catalog_sources src/catalog.c src/catalog_answer.c src/catalog_check.c src/catalog_patterns.c src/catalog_schema.c src/catalog_fold.c
    src/catalog_fold_table.c src/catalog_sources.c src/catalog_path.c src/catalog_read.c
    src/catalog_resolve.c src/catalog_semver.c src/catalog_source.c src/catalog_state.c src/catalog_text.c
    src/catalog_time.c src/catalog_url.c src/catalog_url_host.c)
add_library(runtime_catalog OBJECT ${runtime_catalog_sources})
target_link_libraries(runtime_catalog PUBLIC runtime_objects)
set_target_properties(runtime_catalog PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_catalog)

add_library(runtime_catalog_refresh OBJECT src/catalog_refresh.c)
target_link_libraries(runtime_catalog_refresh PUBLIC runtime_objects)
set_target_properties(runtime_catalog_refresh PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_catalog_refresh)

add_library(runtime_catalog_refresh_test_hooks OBJECT src/catalog_refresh.c)
target_link_libraries(runtime_catalog_refresh_test_hooks PUBLIC runtime_objects)
target_compile_definitions(runtime_catalog_refresh_test_hooks PRIVATE SPISOR_CATALOG_TEST_HOOKS)
spisor_strict_warnings(runtime_catalog_refresh_test_hooks)

set(runtime_cache_sources src/cache.c src/cache_folders.c src/cache_install.c src/cache_installed.c src/cache_marker.c
    src/cache_place.c src/cache_plan.c src/cache_references.c src/cache_refusal.c src/cache_store.c src/cache_sweep.c
    src/file_times.c src/host_references.c src/node_json_file.c src/platform_key.c src/process_alive.c src/run_records.c)
add_library(runtime_cache OBJECT ${runtime_cache_sources})
target_link_libraries(runtime_cache PUBLIC runtime_objects)
target_include_directories(runtime_cache PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_cache PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_cache)

set(runtime_updates_sources src/boot.c src/boot_hint.c src/boot_resolve.c src/plugin_choices.c src/updates.c
    src/updates_apply.c src/updates_catalog.c src/updates_check.c src/updates_rollback.c src/version_map.c)
add_library(runtime_updates OBJECT ${runtime_updates_sources})
target_link_libraries(runtime_updates PUBLIC runtime_objects)
target_include_directories(runtime_updates PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(runtime_updates PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_updates)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(runtime SHARED src/release.c $<TARGET_OBJECTS:runtime_objects> $<TARGET_OBJECTS:runtime_change_reversal>
    $<TARGET_OBJECTS:runtime_path_entry> $<TARGET_OBJECTS:runtime_secret_store> $<TARGET_OBJECTS:runtime_fetch>
    $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
    $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_updates>)
target_include_directories(runtime PRIVATE src)
target_link_libraries(runtime PRIVATE spisor_library ${runtime_secret_store_libraries} ${runtime_path_entry_libraries}
    ${runtime_fetch_libraries})
set_target_properties(runtime PROPERTIES C_VISIBILITY_PRESET hidden)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(runtime PRIVATE -Wl,--exclude-libs,ALL)
endif()
spisor_strict_warnings(runtime)

if(WIN32)
    add_executable(runtime_launcher src/launcher_windows.c)
    set_target_properties(runtime_launcher PROPERTIES OUTPUT_NAME runtime-launcher)
    spisor_strict_warnings(runtime_launcher)
    add_dependencies(runtime runtime_launcher)
endif()

add_subdirectory(test)
