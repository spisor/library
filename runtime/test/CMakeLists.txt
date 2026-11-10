function(runtime_test name)
    add_executable(${name} ${name}.c ${ARGN} $<TARGET_OBJECTS:runtime_path_entry_test_hooks>)
    target_link_libraries(${name} PRIVATE runtime_objects ${runtime_secret_store_libraries}
        ${runtime_path_entry_libraries})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test)
    spisor_strict_warnings(${name})
    target_compile_definitions(${name} PRIVATE RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

foreach(name paths_test config_test settings_test log_test places_test install_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_secret_store>)
endforeach()

runtime_test(secrets_test fake_secret_backend.c)

runtime_test(json_edit_test $<TARGET_OBJECTS:runtime_secret_store>)
runtime_test(json_key_change_test $<TARGET_OBJECTS:runtime_secret_store>)

runtime_test(registry_test $<TARGET_OBJECTS:runtime_secret_store>)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(registry_test PRIVATE Threads::Threads)
endif()

if(WIN32 OR APPLE)
    if(WIN32)
        runtime_test(secret_store_test $<TARGET_OBJECTS:runtime_secret_store_test_hooks>)
    else()
        runtime_test(secret_store_test $<TARGET_OBJECTS:runtime_secret_store>)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    runtime_test(secret_linux_test $<TARGET_OBJECTS:runtime_secret_store_test_hooks>)
endif()

runtime_test(changes_test $<TARGET_OBJECTS:runtime_change_reversal_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
if(NOT WIN32)
    target_link_libraries(changes_test PRIVATE Threads::Threads)
endif()

runtime_test(path_entry_test $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_secret_store>)

add_executable(command_probe command_probe.c)
spisor_strict_warnings(command_probe)

runtime_test(commands_test $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_secret_store>)
target_compile_definitions(commands_test PRIVATE RUNTIME_PROBE_PATH="$<TARGET_FILE:command_probe>")
add_dependencies(commands_test command_probe)
if(WIN32)
    add_dependencies(commands_test runtime_launcher)
endif()
runtime_test(lifecycle_test $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_secret_store>)
if(WIN32)
    add_dependencies(lifecycle_test runtime_launcher)
endif()
set_tests_properties(path_entry_test commands_test lifecycle_test PROPERTIES RESOURCE_LOCK user_path_value)

runtime_test(sha256_test $<TARGET_OBJECTS:runtime_secret_store>)
runtime_test(identity_test $<TARGET_OBJECTS:runtime_secret_store>)

runtime_test(fetch_test http_fixture.c $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(fetch_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(fetch_test PRIVATE Ws2_32)
endif()

runtime_test(identity_resolve_test http_fixture.c $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(identity_resolve_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(identity_resolve_test PRIVATE Ws2_32)
endif()

runtime_test(fetch_network_test $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(fetch_network_test PRIVATE ${runtime_fetch_libraries})
set_tests_properties(fetch_network_test PROPERTIES LABELS network)

set(runtime_catalog_fixture_definitions RUNTIME_CATALOG_FIXTURES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/catalog"
    RUNTIME_CATALOG_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/runtime/schema/catalog.schema.json")
foreach(name catalog_test catalog_sources_test catalog_resolve_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
        $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
    target_compile_definitions(${name} PRIVATE ${runtime_catalog_fixture_definitions})
endforeach()

runtime_test(catalog_check_test http_fixture.c $<TARGET_OBJECTS:runtime_catalog>
    $<TARGET_OBJECTS:runtime_catalog_refresh_test_hooks> $<TARGET_OBJECTS:runtime_fetch_test_hooks>
    $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(catalog_check_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(catalog_check_test PRIVATE Ws2_32)
endif()

runtime_test(cache_test $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
    $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(cache_test PRIVATE ${runtime_fetch_libraries})

foreach(name updates_test boot_hint_test cache_locate_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_updates> $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_catalog>
        $<TARGET_OBJECTS:runtime_catalog_refresh> $<TARGET_OBJECTS:runtime_fetch_test_hooks>
        $<TARGET_OBJECTS:runtime_secret_store>)
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
endforeach()

set(runtime_processes_test_objects $<TARGET_OBJECTS:runtime_processes> $<TARGET_OBJECTS:runtime_cache>
    $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh> $<TARGET_OBJECTS:runtime_fetch>
    $<TARGET_OBJECTS:runtime_secret_store>)
add_executable(background_fixture background_fixture.c ${runtime_processes_test_objects}
    $<TARGET_OBJECTS:runtime_path_entry>)
target_link_libraries(background_fixture PRIVATE runtime_objects ${runtime_secret_store_libraries}
    ${runtime_path_entry_libraries} ${runtime_fetch_libraries})
target_include_directories(background_fixture PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
spisor_strict_warnings(background_fixture)

set(runtime_hosts_test_objects $<TARGET_OBJECTS:runtime_hosts> ${runtime_processes_test_objects}
    $<TARGET_OBJECTS:runtime_change_reversal>)
foreach(name hosts_test loader_test entry_template_test detect_test lists_test)
    runtime_test(${name} ${runtime_hosts_test_objects})
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
endforeach()
if(WIN32)
    add_dependencies(hosts_test runtime_launcher)
endif()
set_tests_properties(hosts_test PROPERTIES RESOURCE_LOCK user_path_value)

foreach(name ui_navigation_test ui_settings_view_test ui_render_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_ui> ${runtime_hosts_test_objects})
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE RUNTIME_UI_FIXTURES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ui")
endforeach()

runtime_test(processes_test ${runtime_processes_test_objects})
target_link_libraries(processes_test PRIVATE ${runtime_fetch_libraries})
target_compile_definitions(processes_test PRIVATE RUNTIME_BACKGROUND_FIXTURE_PATH="$<TARGET_FILE:background_fixture>")
add_dependencies(processes_test background_fixture)
if(NOT WIN32)
    target_link_libraries(processes_test PRIVATE Threads::Threads)
endif()

runtime_test(processes_ready_test ${runtime_processes_test_objects})
target_link_libraries(processes_ready_test PRIVATE ${runtime_fetch_libraries})

add_executable(node_probe node_probe.c)
spisor_strict_warnings(node_probe)

set(runtime_install_test_objects $<TARGET_OBJECTS:runtime_install_test_hooks> $<TARGET_OBJECTS:runtime_updates>
    ${runtime_hosts_test_objects})
foreach(name install_methods_test install_entries_test install_runtime_test install_ecosystem_test
        install_applications_test)
    runtime_test(${name} ${runtime_install_test_objects})
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    if(WIN32)
        add_dependencies(${name} runtime_launcher)
    endif()
endforeach()
target_compile_definitions(install_runtime_test PRIVATE RUNTIME_NODE_PROBE_PATH="$<TARGET_FILE:node_probe>")
add_dependencies(install_runtime_test node_probe)
set_tests_properties(install_entries_test install_ecosystem_test install_applications_test PROPERTIES
    RESOURCE_LOCK user_path_value)

runtime_test(host_removal_test $<TARGET_OBJECTS:runtime_updates> ${runtime_hosts_test_objects})
target_link_libraries(host_removal_test PRIVATE ${runtime_fetch_libraries})

add_executable(settings_lock_test settings_lock_test.c $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_path_entry_test_hooks>)
target_link_libraries(settings_lock_test PRIVATE runtime_objects_config_test_hooks ${runtime_secret_store_libraries}
    ${runtime_path_entry_libraries})
if(NOT WIN32)
    target_link_libraries(settings_lock_test PRIVATE Threads::Threads)
endif()
target_include_directories(settings_lock_test PRIVATE ${PROJECT_SOURCE_DIR}/test)
spisor_strict_warnings(settings_lock_test)
target_compile_definitions(settings_lock_test PRIVATE RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME settings_lock_test COMMAND settings_lock_test)
