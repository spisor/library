spisor_bound_tests()

function(runtime_test_with_allocator name allocator)
    add_executable(${name} ${name}.c ${ARGN} $<TARGET_OBJECTS:runtime_path_entry_test_hooks>)
    target_link_libraries(${name} PRIVATE runtime_objects ${allocator} ${runtime_secret_store_libraries}
        ${runtime_path_entry_libraries})
    target_link_libraries(${name} PRIVATE spisor_vector_reader)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/src)
    spisor_strict_warnings(${name})
    target_compile_definitions(${name} PRIVATE RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

function(runtime_test name)
    runtime_test_with_allocator(${name} spisor_allocator ${ARGN})
endfunction()

runtime_test_with_allocator(reader_with_counting_allocator_test spisor_allocator_counting
    $<TARGET_OBJECTS:runtime_secret_store>)
target_compile_definitions(reader_with_counting_allocator_test PRIVATE
    SPISOR_VECTOR_READER_VECTOR_PATH="${PROJECT_SOURCE_DIR}/test/vectors/vector-reader.json")

foreach(name paths_test settings_test log_test places_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_secret_store>)
endforeach()

runtime_test(secrets_test fake_secret_backend.c)
runtime_test(config_test fake_secret_backend.c)

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

runtime_test(changes_test $<TARGET_OBJECTS:runtime_change_reversal_test_hooks> $<TARGET_OBJECTS:runtime_commands> $<TARGET_OBJECTS:runtime_secret_store>)
if(NOT WIN32)
    target_link_libraries(changes_test PRIVATE Threads::Threads)
endif()

runtime_test(path_entry_test $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_commands> $<TARGET_OBJECTS:runtime_secret_store>)

add_executable(command_probe command_probe.c)
spisor_strict_warnings(command_probe)

runtime_test(commands_test $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_commands> $<TARGET_OBJECTS:runtime_secret_store>)
target_compile_definitions(commands_test PRIVATE RUNTIME_PROBE_PATH="$<TARGET_FILE:command_probe>")
add_dependencies(commands_test command_probe)
if(WIN32)
    add_dependencies(commands_test runtime_launcher)
    # It holds and moves away the runtime-launcher.exe beside it, so it gets a copy no other test copies shims from.
    set(commands_test_directory ${CMAKE_BINARY_DIR}/bin/commands_test/$<CONFIG>)
    set_target_properties(commands_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${commands_test_directory})
    add_custom_command(OUTPUT ${commands_test_directory}/runtime-launcher.exe DEPENDS runtime_launcher
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:runtime_launcher>
        ${commands_test_directory}/runtime-launcher.exe)
    add_custom_target(commands_test_launcher DEPENDS ${commands_test_directory}/runtime-launcher.exe)
    add_dependencies(commands_test commands_test_launcher)
endif()
runtime_test(lifecycle_test $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_commands> $<TARGET_OBJECTS:runtime_secret_store>)
if(WIN32)
    add_dependencies(lifecycle_test runtime_launcher)
endif()

runtime_test(sha256_test $<TARGET_OBJECTS:runtime_secret_store>)
runtime_test(identity_test $<TARGET_OBJECTS:runtime_identity_resolve> $<TARGET_OBJECTS:runtime_identity_also_at>
    $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(identity_test PRIVATE ${runtime_fetch_libraries})

runtime_test(fetch_test http_fixture.c $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(fetch_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(fetch_test PRIVATE Ws2_32)
endif()

runtime_test(identity_resolve_test http_fixture.c $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_identity_also_at> $<TARGET_OBJECTS:runtime_fetch_test_hooks>
    $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(identity_resolve_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(identity_resolve_test PRIVATE Ws2_32)
endif()

runtime_test(identity_also_at_test http_fixture.c $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_identity_also_at> $<TARGET_OBJECTS:runtime_fetch_test_hooks>
    $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(identity_also_at_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(identity_also_at_test PRIVATE Ws2_32)
endif()

runtime_test(marketplace_build_test http_fixture.c $<TARGET_OBJECTS:runtime_marketplace_build>
    $<TARGET_OBJECTS:runtime_marketplace_reading_test_hooks> $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(marketplace_build_test PRIVATE ${runtime_fetch_libraries})
target_compile_definitions(marketplace_build_test PRIVATE
    RUNTIME_MARKETPLACE_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/runtime/schema/marketplace.schema.json"
    RUNTIME_MARKETPLACE_COPY_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/runtime/schema/marketplace-copy.schema.json")
if(WIN32)
    target_link_libraries(marketplace_build_test PRIVATE Ws2_32)
endif()

runtime_test(marketplace_sources_test http_fixture.c $<TARGET_OBJECTS:runtime_marketplace_reading_test_hooks>
    $<TARGET_OBJECTS:runtime_marketplace_build> $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(marketplace_sources_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(marketplace_sources_test PRIVATE Ws2_32)
endif()

runtime_test(marketplace_memory_test $<TARGET_OBJECTS:runtime_marketplace_reading_test_hooks>
    $<TARGET_OBJECTS:runtime_marketplace_build> $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(marketplace_memory_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(marketplace_memory_test PRIVATE Ws2_32)
endif()

set(SPISOR_MARKETPLACE_TEMPLATES_PATH "" CACHE PATH "Folder holding the six marketplace-template-<host> checkouts; the test skips when empty")
runtime_test(marketplace_templates_test $<TARGET_OBJECTS:runtime_secret_store>)
target_compile_definitions(marketplace_templates_test PRIVATE SPISOR_MARKETPLACE_TEMPLATES_PATH="${SPISOR_MARKETPLACE_TEMPLATES_PATH}")

runtime_test(identity_resolve_shipped_test $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_identity_also_at> $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(identity_resolve_shipped_test PRIVATE ${runtime_fetch_libraries})

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

runtime_test(cache_deploy_test $<TARGET_OBJECTS:runtime_cache_test_hooks> $<TARGET_OBJECTS:runtime_catalog>
    $<TARGET_OBJECTS:runtime_catalog_refresh> $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(cache_deploy_test PRIVATE ${runtime_fetch_libraries})

foreach(name cache_locate_test cache_id_key_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_updates> $<TARGET_OBJECTS:runtime_processes>
        $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh> $<TARGET_OBJECTS:runtime_fetch_test_hooks>
        $<TARGET_OBJECTS:runtime_secret_store>)
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
endforeach()

set(runtime_processes_test_objects $<TARGET_OBJECTS:runtime_processes> $<TARGET_OBJECTS:runtime_cache>
    $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh> $<TARGET_OBJECTS:runtime_fetch>
    $<TARGET_OBJECTS:runtime_secret_store>)
add_executable(background_fixture background_fixture.c ${runtime_processes_test_objects}
    $<TARGET_OBJECTS:runtime_path_entry>)
target_link_libraries(background_fixture PRIVATE runtime_objects spisor_allocator ${runtime_secret_store_libraries}
    ${runtime_path_entry_libraries} ${runtime_fetch_libraries})
target_include_directories(background_fixture PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
spisor_strict_warnings(background_fixture)

set(runtime_hosts_test_objects $<TARGET_OBJECTS:runtime_hosts> ${runtime_processes_test_objects}
    $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_commands>)
foreach(name loader_test entry_template_test detect_test lists_test)
    runtime_test(${name} ${runtime_hosts_test_objects})
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
endforeach()

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

set(runtime_install_by_id_objects $<TARGET_OBJECTS:runtime_install_by_id> $<TARGET_OBJECTS:runtime_identity_resolve>
    $<TARGET_OBJECTS:runtime_identity_also_at>)
set(runtime_install_test_objects $<TARGET_OBJECTS:runtime_install_test_hooks> $<TARGET_OBJECTS:runtime_updates>
    $<TARGET_OBJECTS:runtime_hosts_definition> ${runtime_install_by_id_objects} ${runtime_hosts_test_objects})
# A test serving byId over the loopback fake needs plain http, which only the fetch test hooks allow.
set(runtime_install_plain_http_test_objects ${runtime_install_test_objects})
list(TRANSFORM runtime_install_plain_http_test_objects REPLACE "runtime_fetch>$" "runtime_fetch_test_hooks>")
foreach(name updates_test boot_hint_test boot_unlistable_test hosts_start_test)
    runtime_test(${name} ${runtime_install_plain_http_test_objects})
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
endforeach()
foreach(name install_test install_warnings_test install_methods_test install_entries_test install_runtime_test
        install_ecosystem_test install_applications_test hosts_test)
    runtime_test(${name} ${runtime_install_test_objects})
    target_link_libraries(${name} PRIVATE ${runtime_fetch_libraries})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    if(WIN32)
        add_dependencies(${name} runtime_launcher)
    endif()
endforeach()
if(WIN32)
    add_dependencies(hosts_test runtime_launcher)
endif()
runtime_test(install_by_id_test http_fixture.c ${runtime_install_plain_http_test_objects})
target_link_libraries(install_by_id_test PRIVATE ${runtime_fetch_libraries})
target_include_directories(install_by_id_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(install_by_id_test PRIVATE Ws2_32)
    add_dependencies(install_by_id_test runtime_launcher)
endif()
target_compile_definitions(install_runtime_test PRIVATE RUNTIME_NODE_PROBE_PATH="$<TARGET_FILE:node_probe>")
add_dependencies(install_runtime_test node_probe)

runtime_test(host_removal_test ${runtime_install_test_objects})
target_link_libraries(host_removal_test PRIVATE ${runtime_fetch_libraries})

add_executable(settings_lock_test settings_lock_test.c $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_path_entry_test_hooks>)
target_link_libraries(settings_lock_test PRIVATE runtime_objects_config_test_hooks spisor_allocator ${runtime_secret_store_libraries}
    ${runtime_path_entry_libraries})
if(NOT WIN32)
    target_link_libraries(settings_lock_test PRIVATE Threads::Threads)
endif()
target_link_libraries(settings_lock_test PRIVATE spisor_vector_reader)
target_include_directories(settings_lock_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/src)
spisor_strict_warnings(settings_lock_test)
target_compile_definitions(settings_lock_test PRIVATE RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME settings_lock_test COMMAND settings_lock_test)

set(runtime_marketplace_test_objects $<TARGET_OBJECTS:runtime_marketplace_build> $<TARGET_OBJECTS:runtime_marketplace_reading>)
set(runtime_definition_test_objects ${runtime_marketplace_test_objects} ${runtime_install_test_objects})
# Its catalog check of a remote index reaches a local file through the catalog's request redirect, a test hook.
set(runtime_out_of_memory_test_objects ${runtime_install_plain_http_test_objects})
list(TRANSFORM runtime_out_of_memory_test_objects REPLACE "runtime_catalog_refresh>$"
    "runtime_catalog_refresh_test_hooks>")
runtime_test_with_allocator(out_of_memory_test spisor_allocator_counting http_fixture.c
    ${runtime_marketplace_test_objects} ${runtime_out_of_memory_test_objects})
target_link_libraries(out_of_memory_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(out_of_memory_test PRIVATE Ws2_32)
endif()
target_include_directories(out_of_memory_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_tests_properties(out_of_memory_test PROPERTIES TIMEOUT 600)
add_test(NAME out_of_memory_commands_test COMMAND out_of_memory_test commands)
set_tests_properties(out_of_memory_commands_test PROPERTIES TIMEOUT 1800)
add_test(NAME out_of_memory_cache_test COMMAND out_of_memory_test cache)
add_test(NAME out_of_memory_install_test COMMAND out_of_memory_test install)
set_tests_properties(out_of_memory_install_test PROPERTIES TIMEOUT 1800)
set_tests_properties(out_of_memory_cache_test PROPERTIES TIMEOUT 1800)
if(WIN32)
    add_dependencies(out_of_memory_test runtime_launcher)
endif()

runtime_test_with_allocator(out_of_memory_ecosystem_test spisor_allocator_counting ${runtime_install_test_objects})
target_link_libraries(out_of_memory_ecosystem_test PRIVATE ${runtime_fetch_libraries})
target_include_directories(out_of_memory_ecosystem_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_tests_properties(out_of_memory_ecosystem_test PROPERTIES TIMEOUT 1800)
if(WIN32)
    add_dependencies(out_of_memory_ecosystem_test runtime_launcher)
endif()

runtime_test_with_allocator(out_of_memory_install_operations_test spisor_allocator_counting
    ${runtime_install_test_objects})
target_link_libraries(out_of_memory_install_operations_test PRIVATE ${runtime_fetch_libraries})
target_include_directories(out_of_memory_install_operations_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(out_of_memory_install_operations_test PRIVATE
    RUNTIME_NODE_PROBE_PATH="$<TARGET_FILE:node_probe>"
    RUNTIME_BACKGROUND_FIXTURE_PATH="$<TARGET_FILE:background_fixture>")
add_dependencies(out_of_memory_install_operations_test node_probe background_fixture)
set_tests_properties(out_of_memory_install_operations_test PROPERTIES TIMEOUT 1800)
if(WIN32)
    add_dependencies(out_of_memory_install_operations_test runtime_launcher)
endif()

runtime_test_with_allocator(out_of_memory_ui_test spisor_allocator_counting $<TARGET_OBJECTS:runtime_ui>
    ${runtime_hosts_test_objects})
target_link_libraries(out_of_memory_ui_test PRIVATE ${runtime_fetch_libraries})

runtime_test_with_allocator(json_hooks_race_test spisor_allocator_counting ${runtime_definition_test_objects})
target_link_libraries(json_hooks_race_test PRIVATE ${runtime_fetch_libraries})
target_include_directories(json_hooks_race_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(json_hooks_race_test PRIVATE Threads::Threads)
endif()

file(GLOB runtime_public_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/runtime/*.h)
set(runtime_definition_symbols "")
set(runtime_definition_header_count 0)
foreach(header IN LISTS runtime_public_headers)
    file(READ ${header} header_text)
    if(header_text MATCHES "#include \"runtime/definition.h\"")
        math(EXPR runtime_definition_header_count "${runtime_definition_header_count} + 1")
    endif()
    string(REGEX MATCHALL "runtime_[a-z_]+_definition_of[ \t\r\n]*[(][ \t\r\n]*void[ \t\r\n]*[)]" declarations
        "${header_text}")
    foreach(declaration IN LISTS declarations)
        string(REGEX MATCH "runtime_[a-z_]+_definition_of" symbol "${declaration}")
        list(APPEND runtime_definition_symbols ${symbol})
    endforeach()
endforeach()
list(REMOVE_DUPLICATES runtime_definition_symbols)
string(JOIN [[", "]] runtime_definition_symbol_list ${runtime_definition_symbols})
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/definition_symbols.h
    CONTENT [[#define RUNTIME_DEFINITION_SYMBOLS "@runtime_definition_symbol_list@"
#define RUNTIME_DEFINITION_HEADER_COUNT @runtime_definition_header_count@
]])
add_executable(definition_exports_test definition_exports_test.c)
target_include_directories(definition_exports_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(definition_exports_test PRIVATE RUNTIME_LIBRARY_PATH="$<TARGET_FILE:runtime>")
target_link_libraries(definition_exports_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(definition_exports_test runtime)
spisor_strict_warnings(definition_exports_test)
add_test(NAME definition_exports_test COMMAND definition_exports_test)

add_executable(declaration_test declaration_test.c)
target_include_directories(declaration_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(declaration_test PRIVATE
    RUNTIME_DECLARATION_PATH="${PROJECT_SOURCE_DIR}/runtime/library.json")
target_link_libraries(declaration_test PRIVATE weivel_standard)
spisor_strict_warnings(declaration_test)
add_test(NAME declaration_test COMMAND declaration_test)

add_executable(weivel_host_load_test weivel_host_load_test.c)
target_include_directories(weivel_host_load_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(weivel_host_load_test PRIVATE
    RUNTIME_DECLARATION_PATH="${PROJECT_SOURCE_DIR}/runtime/library.json"
    RUNTIME_LIBRARY_PATH="$<TARGET_FILE:runtime>" WEIVEL_CORE_PATH="$<TARGET_FILE:weivel_core_shared>")
target_link_libraries(weivel_host_load_test PRIVATE weivel_c weivel_standard)
add_dependencies(weivel_host_load_test runtime weivel_core_shared)
spisor_strict_warnings(weivel_host_load_test)
add_test(NAME weivel_host_load_test COMMAND weivel_host_load_test)

include(${PROJECT_SOURCE_DIR}/cmake/definition_calls.cmake)
set(runtime_definition_operations ${CMAKE_CURRENT_BINARY_DIR}/generated/definition_operations.c)
spisor_definition_operations(${runtime_definition_operations} DEFINITIONS ${PROJECT_SOURCE_DIR}/include/runtime
    INCLUDE_ROOT ${PROJECT_SOURCE_DIR}/include PREFIX runtime_ LAYOUTS layouts SHAPES ${PROJECT_SOURCE_DIR}/include)
foreach(name IN ITEMS short_shape_test empty_answer_test)
    spisor_add_definition_call_test(${name} SOURCE ${name}.c LIBRARY runtime OPERATIONS ${runtime_definition_operations})
endforeach()

add_executable(parity_test parity_test.c parity_files.c parity_json.c)
target_include_directories(parity_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(parity_test PRIVATE RUNTIME_LIBRARY_PATH="$<TARGET_FILE:runtime>"
    PARITY_PATH="${PROJECT_SOURCE_DIR}/parity" PARITY_WORK_PATH="${CMAKE_CURRENT_BINARY_DIR}/parity-work")
target_link_libraries(parity_test PRIVATE spisor_vector_reader spisor_library spisor_allocator ${CMAKE_DL_LIBS})
add_dependencies(parity_test runtime)
spisor_strict_warnings(parity_test)
add_test(NAME parity_test COMMAND parity_test)

file(READ ${PROJECT_SOURCE_DIR}/runtime/library.json runtime_declaration)
if(MSVC)
    set(runtime_export_lister ${CMAKE_LINKER})
    set(runtime_export_format pe)
elseif(APPLE)
    set(runtime_export_lister ${CMAKE_NM})
    set(runtime_export_format macho)
else()
    set(runtime_export_lister ${CMAKE_NM})
    set(runtime_export_format elf)
endif()
string(JOIN "," runtime_expected_exports ${runtime_definition_symbols})
add_test(NAME runtime_exports_only_definitions_test
    COMMAND ${CMAKE_COMMAND} -DLIBRARY=$<TARGET_FILE:runtime> -DLISTER=${runtime_export_lister}
        -DFORMAT=${runtime_export_format} -DEXPECTED=${runtime_expected_exports}
        -P ${PROJECT_SOURCE_DIR}/cmake/exports_check.cmake)

runtime_test_with_allocator(environment_check_test spisor_allocator_counting ${runtime_install_test_objects})
target_link_libraries(environment_check_test PRIVATE ${runtime_fetch_libraries})
target_include_directories(environment_check_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/runtime/src)
