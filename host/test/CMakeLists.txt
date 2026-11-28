spisor_bound_tests()

set(host_test_runtime_objects ${host_runtime_shared_objects} $<TARGET_OBJECTS:runtime_path_entry_test_hooks>
    $<TARGET_OBJECTS:runtime_install_test_hooks> $<TARGET_OBJECTS:host_ready_timeout_test_hook> $<TARGET_OBJECTS:host_marketplace_lookup_test_hook>
    $<TARGET_OBJECTS:host_terminal_attached_test_hook>)

host_executable(spisor_host_test spisor-host-test ${PROJECT_SOURCE_DIR}/host/src/main.c $<TARGET_OBJECTS:host_objects>
    ${host_test_runtime_objects})
if(WIN32)
    add_dependencies(spisor_host_test runtime_launcher)
endif()

set(host_test_bundles $<TARGET_FILE_DIR:spisor_host_test>/test-bundles)
stage_bundles(spisor_host_test_bundles spisor_host_test ${host_test_bundles})

add_executable(fake_application fake_application.c)
target_link_libraries(fake_application PRIVATE naula_cjson spisor_allocator)
target_include_directories(fake_application PRIVATE ${PROJECT_SOURCE_DIR}/host/src ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include)
if(WIN32)
    target_sources(fake_application PRIVATE ${PROJECT_SOURCE_DIR}/host/src/host_arguments.c)
endif()
spisor_strict_warnings(fake_application)

set(c_host_runtime_objects $<TARGET_OBJECTS:runtime_change_reversal_test_hooks> $<TARGET_OBJECTS:runtime_commands> $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_identity_resolve> $<TARGET_OBJECTS:runtime_identity_also_at>
    $<TARGET_OBJECTS:runtime_install_by_id> $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
    $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_updates> $<TARGET_OBJECTS:runtime_processes>
    $<TARGET_OBJECTS:runtime_hosts> $<TARGET_OBJECTS:runtime_hosts_definition>
    $<TARGET_OBJECTS:runtime_processes_definition>
    $<TARGET_OBJECTS:runtime_path_entry_test_hooks> $<TARGET_OBJECTS:runtime_install_test_hooks>)
host_executable(c_host c-host c_host.c ${PROJECT_SOURCE_DIR}/host/src/host_definition.c ${c_host_runtime_objects})
if(WIN32)
    add_dependencies(c_host runtime_launcher)
endif()

add_executable(javascript_trap javascript_trap.c)
spisor_strict_warnings(javascript_trap)

host_executable(layer_fixture layer-fixture layer_fixture.c $<TARGET_OBJECTS:host_objects> ${host_test_runtime_objects})

# The C fixture plugin and the two fixture languages' host programs share one folder with weivel/c's shared layer, so
# stage-bundle stages each program beside the layer the plugin links too.
set(language_host_inputs ${CMAKE_CURRENT_BINARY_DIR}/language-host-inputs)
add_library(c_fixture_plugin SHARED c_fixture_plugin.c)
target_link_libraries(c_fixture_plugin PRIVATE weivel_c_shared)
set_target_properties(c_fixture_plugin PROPERTIES C_VISIBILITY_PRESET hidden RUNTIME_OUTPUT_DIRECTORY ${language_host_inputs}
    LIBRARY_OUTPUT_DIRECTORY ${language_host_inputs} ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/language-host-archives)
spisor_strict_warnings(c_fixture_plugin)
foreach(language one two)
    add_executable(fixture_${language}_host language_host_program.c)
    target_compile_definitions(fixture_${language}_host PRIVATE HOST_LANGUAGE="fixture-${language}")
    target_link_libraries(fixture_${language}_host PRIVATE weivel_c_shared weivel_standard_shared)
    set_target_properties(fixture_${language}_host PROPERTIES OUTPUT_NAME fixture-${language}-host
        RUNTIME_OUTPUT_DIRECTORY ${language_host_inputs})
    spisor_strict_warnings(fixture_${language}_host)
endforeach()
add_custom_target(language_host_weivel_c
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:weivel_c_shared> $<TARGET_FILE_DIR:fixture_one_host>
    VERBATIM)
add_dependencies(language_host_weivel_c weivel_c_shared fixture_one_host fixture_two_host)

set(language_host_bundles $<TARGET_FILE_DIR:spisor_host_test>/language-hosts)
set(fixture_host_data ${CMAKE_CURRENT_BINARY_DIR}/fixture-host-data.json)
file(WRITE ${fixture_host_data} "{}\n")
set(typescript_host_inputs ${CMAKE_CURRENT_BINARY_DIR}/typescript-host-inputs)
file(WRITE ${typescript_host_inputs}/host.js "export {};\n")
set(typescript_host_data ${CMAKE_CURRENT_BINARY_DIR}/typescript-host-data.json)
file(WRITE ${typescript_host_data} "{\"node\": {\"pinned\": \"26.13.1\", \"minimum\": \"22.18.0\", \"builds\": {}}}\n")
set(staged_weivel $<TARGET_FILE:weivel_core_shared> $<TARGET_FILE:weivel_standard_shared>)
add_custom_target(language_host_test_bundles ALL
    COMMAND ${CMAKE_COMMAND} -E chdir $<TARGET_FILE_DIR:fixture_one_host> $<TARGET_FILE:stage_bundle> language-host
        ${language_host_bundles}/fixture-one-host 1.0.0 fixture-one native $<TARGET_FILE_NAME:fixture_one_host>
        ${fixture_host_data} ${staged_weivel} $<TARGET_FILE_NAME:weivel_c_shared>
    COMMAND ${CMAKE_COMMAND} -E chdir $<TARGET_FILE_DIR:fixture_two_host> $<TARGET_FILE:stage_bundle> language-host
        ${language_host_bundles}/fixture-two-host 1.0.0 fixture-two native $<TARGET_FILE_NAME:fixture_two_host>
        ${fixture_host_data} ${staged_weivel} $<TARGET_FILE_NAME:weivel_c_shared>
    COMMAND ${CMAKE_COMMAND} -E chdir ${typescript_host_inputs} $<TARGET_FILE:stage_bundle> language-host
        ${language_host_bundles}/typescript-host 1.0.0 typescript javascript host.js ${typescript_host_data} ${staged_weivel}
    VERBATIM)
add_dependencies(language_host_test_bundles stage_bundle language_host_weivel_c weivel_core_shared
    weivel_standard_shared weivel_c_shared spisor_host_test)

set(host_test_fixture_definitions RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}"
    SPISOR_HOST_PATH="$<TARGET_FILE:spisor_host_test>" SPISOR_BOOT_PATH="$<TARGET_FILE:spisor_boot>"
    FAKE_APPLICATION_PATH="$<TARGET_FILE:fake_application>" LAYER_FIXTURE_PATH="$<TARGET_FILE:layer_fixture>"
    C_HOST_PATH="$<TARGET_FILE:c_host>" JAVASCRIPT_TRAP_PATH="$<TARGET_FILE:javascript_trap>"
    STAGED_BUNDLES_PATH="${host_test_bundles}" SPISOR_HOST_VERSION="${SPISOR_HOST_VERSION}" BUILD_TYPE="$<CONFIG>"
    STAGED_LANGUAGE_HOSTS_PATH="${language_host_bundles}" C_FIXTURE_PLUGIN_PATH="$<TARGET_FILE:c_fixture_plugin>"
    RUNTIME_BINARY_NAME="$<TARGET_FILE_NAME:runtime>")
if(WIN32)
    list(APPEND host_test_fixture_definitions RUNTIME_LAUNCHER_PATH="$<TARGET_FILE:runtime_launcher>")
endif()

function(host_test name)
    host_executable(${name} ${name} ${name}.c ${ARGN} $<TARGET_OBJECTS:host_objects> ${host_test_runtime_objects})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/runtime/test)
    target_link_libraries(${name} PRIVATE spisor_vector_reader)
    target_compile_definitions(${name} PRIVATE ${host_test_fixture_definitions})
    add_dependencies(${name} spisor_host_test spisor_boot fake_application layer_fixture spisor_host_test_bundles)
    add_test(NAME host_${name} COMMAND ${name})
    set_tests_properties(host_${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

host_test(launch_test)
# A sanitized macOS runner runs past the 300 s SPISOR_TEST_TIMEOUT_SECONDS gives every other test.
set_tests_properties(host_launch_test PROPERTIES TIMEOUT 900)
host_test(action_test)
host_test(language_layers_test)
add_dependencies(language_layers_test language_host_test_bundles c_fixture_plugin)
host_test(ui_command_test)
host_test(hook_test)
host_test(install_command_test)
host_test(boot_stub_test)
host_test(lifecycle_command_test)
host_test(profile_test)
host_test(command_line_test)
host_test(child_test)
host_test(catalog_build_test)
if(WIN32)
    set(failing_stage_bundle_sources ${PROJECT_SOURCE_DIR}/host/src/host_arguments.c)
else()
    set(failing_stage_bundle_sources "")
endif()
host_executable(stage_bundle_failing stage-bundle-failing ${PROJECT_SOURCE_DIR}/host/bundle/stage_bundle.c
    ${failing_stage_bundle_sources} ${host_runtime_objects})
target_compile_definitions(stage_bundle_failing PRIVATE STAGE_BUNDLE_FAILING_RENAME_INTO_PLACE)
set_target_properties(stage_bundle_failing PROPERTIES SPISOR_TEST_ONLY TRUE)

host_test(stage_bundle_test)
add_dependencies(stage_bundle_test stage_bundle stage_bundle_failing)
target_compile_definitions(stage_bundle_test PRIVATE STAGE_BUNDLE_PATH="$<TARGET_FILE:stage_bundle>"
    FAILING_STAGE_BUNDLE_PATH="$<TARGET_FILE:stage_bundle_failing>"
    CMAKE_PATH="${CMAKE_COMMAND}" TYPESCRIPT_HOST_SCRIPT="${SPISOR_TYPESCRIPT_HOST_SCRIPT}")
add_test(NAME host_stage_bundle_node_test COMMAND stage_bundle_test node)
set_tests_properties(host_stage_bundle_node_test PROPERTIES SKIP_RETURN_CODE 77)
host_test(install_script_test)
host_test(c_host_proof)
add_dependencies(c_host_proof c_host)
host_test(hook_without_javascript)
add_dependencies(hook_without_javascript javascript_trap)
# Its 200 ms latency budget is measured, so it runs with no other test beside it.
set_tests_properties(host_hook_without_javascript PROPERTIES RUN_SERIAL TRUE)
target_compile_definitions(install_script_test PRIVATE INSTALL_SCRIPTS_PATH="${PROJECT_SOURCE_DIR}/install"
    CATALOG_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/runtime/schema/catalog.schema.json")
# Windows PowerShell runs take up to 631 s on a windows-latest runner, against 28 s on a workstation.
set(install_script_timeout_seconds 1200)
set_tests_properties(host_install_script_test PROPERTIES TIMEOUT ${install_script_timeout_seconds})
if(WIN32)
    find_program(SPISOR_PWSH pwsh)
    if(SPISOR_PWSH)
        add_test(NAME host_install_script_test_pwsh COMMAND install_script_test)
        set_tests_properties(host_install_script_test_pwsh PROPERTIES SKIP_RETURN_CODE 77
            ENVIRONMENT "INSTALL_SCRIPT_POWERSHELL=${SPISOR_PWSH}" TIMEOUT ${install_script_timeout_seconds})
    endif()
endif()

set(host_marketplace_test_objects ${host_test_runtime_objects})
list(REMOVE_ITEM host_marketplace_test_objects $<TARGET_OBJECTS:runtime_fetch>)
list(APPEND host_marketplace_test_objects $<TARGET_OBJECTS:runtime_fetch_test_hooks>)
host_executable(marketplace_command_test marketplace_command_test TEST_HOOKS marketplace_command_test.c
    ${PROJECT_SOURCE_DIR}/runtime/test/http_fixture.c $<TARGET_OBJECTS:host_objects> ${host_marketplace_test_objects})
if(WIN32)
    target_link_libraries(marketplace_command_test PRIVATE Ws2_32)
endif()
target_include_directories(marketplace_command_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/runtime/test)
target_link_libraries(marketplace_command_test PRIVATE spisor_vector_reader)
target_compile_definitions(marketplace_command_test PRIVATE ${host_test_fixture_definitions} HOST_MARKETPLACE_TEST_HOOK)
add_dependencies(marketplace_command_test spisor_host_test spisor_boot fake_application layer_fixture spisor_host_test_bundles)
add_test(NAME host_marketplace_command_test COMMAND marketplace_command_test)
set_tests_properties(host_marketplace_command_test PROPERTIES SKIP_RETURN_CODE 77)

host_executable(install_by_name_test install_by_name_test TEST_HOOKS install_by_name_test.c
    ${PROJECT_SOURCE_DIR}/runtime/test/http_fixture.c $<TARGET_OBJECTS:host_objects> ${host_marketplace_test_objects})
if(WIN32)
    target_link_libraries(install_by_name_test PRIVATE Ws2_32)
endif()
target_include_directories(install_by_name_test PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/runtime/test)
target_link_libraries(install_by_name_test PRIVATE spisor_vector_reader)
target_compile_definitions(install_by_name_test PRIVATE ${host_test_fixture_definitions} HOST_MARKETPLACE_TEST_HOOK)
add_dependencies(install_by_name_test spisor_host_test spisor_boot fake_application layer_fixture spisor_host_test_bundles)
add_test(NAME host_install_by_name_test COMMAND install_by_name_test)
set_tests_properties(host_install_by_name_test PROPERTIES SKIP_RETURN_CODE 77)
