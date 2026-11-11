set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

set(host_sources src/application_choices.c src/catalog_inputs.c src/catalog_writer.c
    src/command_action.c src/command_background.c src/command_catalog_build.c src/command_hook.c src/command_install.c src/command_launch.c src/command_marketplace.c
    src/command_remove.c src/command_status.c src/command_update.c src/host_context.c src/host_holds.c src/host_options.c
    src/host_output.c src/host_service.c src/host_start.c src/install_by_name.c src/install_warning_report.c src/marketplace_copies.c src/loader_start.c src/own_executable.c src/own_run.c src/required_plugins.c src/session_ready.c src/terminal_question.c src/windows_command.c)
if(WIN32)
    set(host_platform_sources src/child_windows.c src/host_arguments.c)
else()
    set(host_platform_sources src/child_posix.c)
endif()

add_library(host_objects OBJECT ${host_sources} ${host_platform_sources})
target_include_directories(host_objects PUBLIC src ${PROJECT_SOURCE_DIR}/runtime/src PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(host_objects PUBLIC runtime_objects)
spisor_strict_warnings(host_objects)

add_library(host_marketplace_lookup OBJECT src/marketplace_lookup.c)
target_include_directories(host_marketplace_lookup PRIVATE src)
spisor_strict_warnings(host_marketplace_lookup)

add_library(host_marketplace_lookup_test_hook OBJECT src/marketplace_lookup.c)
target_include_directories(host_marketplace_lookup_test_hook PRIVATE src)
target_compile_definitions(host_marketplace_lookup_test_hook PRIVATE HOST_MARKETPLACE_TEST_HOOK)
spisor_strict_warnings(host_marketplace_lookup_test_hook)

add_library(host_ready_timeout OBJECT src/ready_timeout.c)
target_include_directories(host_ready_timeout PRIVATE src)
spisor_strict_warnings(host_ready_timeout)

add_library(host_ready_timeout_test_hook OBJECT src/ready_timeout.c)
target_include_directories(host_ready_timeout_test_hook PRIVATE src)
target_compile_definitions(host_ready_timeout_test_hook PRIVATE HOST_READY_TIMEOUT_TEST_HOOK)
spisor_strict_warnings(host_ready_timeout_test_hook)

set(host_runtime_shared_objects $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_identity_resolve> $<TARGET_OBJECTS:runtime_identity_also_at>
    $<TARGET_OBJECTS:runtime_install_by_id> $<TARGET_OBJECTS:runtime_marketplace_build> $<TARGET_OBJECTS:runtime_marketplace_reading>
    $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
    $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_updates> $<TARGET_OBJECTS:runtime_processes>
    $<TARGET_OBJECTS:runtime_hosts>)
set(host_runtime_objects ${host_runtime_shared_objects} $<TARGET_OBJECTS:runtime_path_entry>
    $<TARGET_OBJECTS:runtime_install> $<TARGET_OBJECTS:host_ready_timeout> $<TARGET_OBJECTS:host_marketplace_lookup>)

function(host_executable target output_name)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE runtime_objects ${runtime_executable_libraries})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/host/src ${PROJECT_SOURCE_DIR}/runtime/src
        ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
    spisor_strict_warnings(${target})
endfunction()

host_executable(spisor_host spisor-host src/main.c $<TARGET_OBJECTS:host_objects> ${host_runtime_objects})

if(WIN32)
    set(host_boot_platform_sources src/child_windows.c src/host_arguments.c ${PROJECT_SOURCE_DIR}/runtime/src/command_line_windows.c
        ${PROJECT_SOURCE_DIR}/runtime/src/windows_process.c)
else()
    set(host_boot_platform_sources src/child_posix.c)
endif()
set(host_boot_runtime_sources alive_stamp.c boot.c boot_hint.c boot_resolve.c cache_folders.c cache_marker.c cache_place.c cache_refusal.c
    catalog_answer.c catalog_path.c catalog_text.c directory_tree.c file_stream.c file_times.c process_alive.c host_references.c hosts_lock.c
    identity_parse.c json_service.c node_json_file.c platform_key.c run_records.c segment.c service_call.c sha256.c)
list(TRANSFORM host_boot_runtime_sources PREPEND ${PROJECT_SOURCE_DIR}/runtime/src/)

# spisor-boot changes almost never once published, so it carries only what boot.resolve needs.
add_executable(spisor_boot src/boot_main.c src/host_output.c src/host_service.c src/own_executable.c src/windows_command.c
    ${host_boot_platform_sources} ${host_boot_runtime_sources})
target_link_libraries(spisor_boot PRIVATE spisor_library)
target_include_directories(spisor_boot PRIVATE ${PROJECT_SOURCE_DIR}/host/src ${PROJECT_SOURCE_DIR}/runtime/src
    ${PROJECT_SOURCE_DIR}/src)
set_target_properties(spisor_boot PROPERTIES OUTPUT_NAME spisor-boot)
spisor_strict_warnings(spisor_boot)

if(WIN32)
    add_dependencies(spisor_host runtime_launcher)
endif()

set(SPISOR_HOST_VERSION 0.1.0)

if(WIN32)
    set(stage_bundle_platform_sources src/host_arguments.c)
else()
    set(stage_bundle_platform_sources "")
endif()
add_executable(stage_bundle bundle/stage_bundle.c ${stage_bundle_platform_sources}
    ${PROJECT_SOURCE_DIR}/runtime/src/directory_tree.c ${PROJECT_SOURCE_DIR}/runtime/src/platform_key.c)
target_link_libraries(stage_bundle PRIVATE spisor_library)
target_include_directories(stage_bundle PRIVATE ${PROJECT_SOURCE_DIR}/host/src ${PROJECT_SOURCE_DIR}/runtime/src
    ${PROJECT_SOURCE_DIR}/src)
set_target_properties(stage_bundle PROPERTIES OUTPUT_NAME stage-bundle)
spisor_strict_warnings(stage_bundle)

function(stage_bundles target executable directory)
    set(host_files $<TARGET_FILE:${executable}> $<TARGET_FILE:spisor_boot>)
    if(WIN32)
        list(APPEND host_files $<TARGET_FILE:runtime_launcher>)
    endif()
    add_custom_target(${target} ALL
        COMMAND stage_bundle host ${directory}/spisor-host ${SPISOR_HOST_VERSION} ${host_files}
        COMMAND stage_bundle library ${directory}/runtime ${PROJECT_SOURCE_DIR}/runtime/library.json $<TARGET_FILE:runtime>
        VERBATIM)
    add_dependencies(${target} stage_bundle ${executable} spisor_boot runtime)
    if(WIN32)
        add_dependencies(${target} runtime_launcher)
    endif()
endfunction()

stage_bundles(spisor_host_bundles spisor_host $<TARGET_FILE_DIR:spisor_host>/bundles)

add_subdirectory(test)
