set(host_test_runtime_objects ${host_runtime_shared_objects} $<TARGET_OBJECTS:runtime_path_entry_test_hooks>
    $<TARGET_OBJECTS:runtime_install_test_hooks>)

host_executable(forebay_host_test forebay-host-test ${PROJECT_SOURCE_DIR}/host/src/main.c $<TARGET_OBJECTS:host_objects>
    ${host_test_runtime_objects})
if(WIN32)
    add_dependencies(forebay_host_test runtime_launcher)
endif()

set(host_test_bundles $<TARGET_FILE_DIR:forebay_host_test>/test-bundles)
stage_bundles(forebay_host_test_bundles forebay_host_test ${host_test_bundles})

add_executable(fake_application fake_application.c)
target_link_libraries(fake_application PRIVATE spisor_cjson)
target_include_directories(fake_application PRIVATE ${PROJECT_SOURCE_DIR}/host/src)
if(WIN32)
    target_sources(fake_application PRIVATE ${PROJECT_SOURCE_DIR}/host/src/host_arguments.c)
endif()
spisor_strict_warnings(fake_application)

set(c_host_runtime_objects $<TARGET_OBJECTS:runtime_change_reversal_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
    $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_updates> $<TARGET_OBJECTS:runtime_processes>
    $<TARGET_OBJECTS:runtime_hosts> $<TARGET_OBJECTS:runtime_path_entry_test_hooks> $<TARGET_OBJECTS:runtime_install_test_hooks>)
host_executable(c_host c-host c_host.c ${c_host_runtime_objects})
if(WIN32)
    add_dependencies(c_host runtime_launcher)
endif()

add_executable(javascript_trap javascript_trap.c)
spisor_strict_warnings(javascript_trap)

host_executable(layer_fixture layer-fixture layer_fixture.c $<TARGET_OBJECTS:host_objects> ${host_test_runtime_objects})

set(host_test_fixture_definitions RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}"
    FOREBAY_HOST_PATH="$<TARGET_FILE:forebay_host_test>" FOREBAY_BOOT_PATH="$<TARGET_FILE:forebay_boot>"
    FAKE_APPLICATION_PATH="$<TARGET_FILE:fake_application>" LAYER_FIXTURE_PATH="$<TARGET_FILE:layer_fixture>"
    C_HOST_PATH="$<TARGET_FILE:c_host>" JAVASCRIPT_TRAP_PATH="$<TARGET_FILE:javascript_trap>"
    STAGED_BUNDLES_PATH="${host_test_bundles}" FOREBAY_HOST_VERSION="${FOREBAY_HOST_VERSION}")
if(WIN32)
    list(APPEND host_test_fixture_definitions RUNTIME_LAUNCHER_PATH="$<TARGET_FILE:runtime_launcher>")
endif()

function(host_test name)
    host_executable(${name} ${name} ${name}.c ${ARGN} $<TARGET_OBJECTS:host_objects> ${host_test_runtime_objects})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/runtime/test)
    target_compile_definitions(${name} PRIVATE ${host_test_fixture_definitions})
    add_dependencies(${name} forebay_host_test forebay_boot fake_application layer_fixture forebay_host_test_bundles)
    add_test(NAME host_${name} COMMAND ${name})
    set_tests_properties(host_${name} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK user_path_value)
endfunction()

host_test(launch_test)
host_test(hook_test)
host_test(install_command_test)
host_test(boot_stub_test)
host_test(lifecycle_command_test)
host_test(profile_test)
host_test(command_line_test)
host_test(child_test)
host_test(catalog_build_test)
host_test(install_script_test)
host_test(c_host_proof)
add_dependencies(c_host_proof c_host)
host_test(hook_without_javascript)
add_dependencies(hook_without_javascript javascript_trap)
target_compile_definitions(install_script_test PRIVATE INSTALL_SCRIPTS_PATH="${PROJECT_SOURCE_DIR}/install"
    CATALOG_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/runtime/schema/catalog.schema.json")
if(WIN32)
    find_program(SPISOR_PWSH pwsh)
    if(SPISOR_PWSH)
        add_test(NAME host_install_script_test_pwsh COMMAND install_script_test)
        set_tests_properties(host_install_script_test_pwsh PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK user_path_value
            ENVIRONMENT "INSTALL_SCRIPT_POWERSHELL=${SPISOR_PWSH}")
    endif()
endif()
