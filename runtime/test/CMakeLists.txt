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

runtime_test(fetch_test http_fixture.c $<TARGET_OBJECTS:runtime_fetch_test_hooks> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(fetch_test PRIVATE ${runtime_fetch_libraries})
if(WIN32)
    target_link_libraries(fetch_test PRIVATE Ws2_32)
endif()

runtime_test(fetch_network_test $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_secret_store>)
target_link_libraries(fetch_network_test PRIVATE ${runtime_fetch_libraries})
set_tests_properties(fetch_network_test PROPERTIES LABELS network)
