set(host_test_runtime_objects ${host_runtime_shared_objects} $<TARGET_OBJECTS:runtime_path_entry_test_hooks>
    $<TARGET_OBJECTS:runtime_install_test_hooks>)

host_executable(forebay_host_test forebay-host-test ${PROJECT_SOURCE_DIR}/host/src/main.c $<TARGET_OBJECTS:host_objects>
    ${host_test_runtime_objects})
if(WIN32)
    add_dependencies(forebay_host_test runtime_launcher)
endif()

add_executable(fake_application fake_application.c)
target_link_libraries(fake_application PRIVATE spisor_cjson)
target_include_directories(fake_application PRIVATE ${PROJECT_SOURCE_DIR}/host/src)
if(WIN32)
    target_sources(fake_application PRIVATE ${PROJECT_SOURCE_DIR}/host/src/host_arguments.c)
endif()
spisor_strict_warnings(fake_application)

host_executable(layer_fixture layer-fixture layer_fixture.c $<TARGET_OBJECTS:host_objects> ${host_test_runtime_objects})

set(host_test_fixture_definitions RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}"
    FOREBAY_HOST_PATH="$<TARGET_FILE:forebay_host_test>" FOREBAY_BOOT_PATH="$<TARGET_FILE:forebay_boot>"
    FAKE_APPLICATION_PATH="$<TARGET_FILE:fake_application>" LAYER_FIXTURE_PATH="$<TARGET_FILE:layer_fixture>")
if(WIN32)
    list(APPEND host_test_fixture_definitions RUNTIME_LAUNCHER_PATH="$<TARGET_FILE:runtime_launcher>")
endif()

function(host_test name)
    host_executable(${name} ${name} ${name}.c ${ARGN} $<TARGET_OBJECTS:host_objects> ${host_test_runtime_objects})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test ${PROJECT_SOURCE_DIR}/runtime/test)
    target_compile_definitions(${name} PRIVATE ${host_test_fixture_definitions})
    add_dependencies(${name} forebay_host_test forebay_boot fake_application layer_fixture)
    add_test(NAME host_${name} COMMAND ${name})
    set_tests_properties(host_${name} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK user_path_value)
endfunction()

host_test(launch_test)
host_test(hook_test)
host_test(install_command_test)
host_test(boot_stub_test)
host_test(lifecycle_command_test)
host_test(profile_test)
