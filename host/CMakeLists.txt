set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

set(host_sources src/application_choices.c src/catalog_inputs.c src/catalog_writer.c
    src/command_background.c src/command_catalog_build.c src/command_hook.c src/command_install.c src/command_launch.c
    src/command_remove.c src/command_status.c src/command_update.c src/host_context.c src/host_holds.c src/host_options.c
    src/host_output.c src/host_service.c src/host_start.c src/loader_start.c src/own_executable.c src/own_run.c src/terminal_question.c src/windows_command.c)
if(WIN32)
    set(host_platform_sources src/child_windows.c src/host_arguments.c)
else()
    set(host_platform_sources src/child_posix.c)
endif()

add_library(host_objects OBJECT ${host_sources} ${host_platform_sources})
target_include_directories(host_objects PUBLIC src ${PROJECT_SOURCE_DIR}/runtime/src PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(host_objects PUBLIC runtime_objects)
spisor_strict_warnings(host_objects)

set(host_runtime_shared_objects $<TARGET_OBJECTS:runtime_change_reversal> $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_fetch> $<TARGET_OBJECTS:runtime_catalog> $<TARGET_OBJECTS:runtime_catalog_refresh>
    $<TARGET_OBJECTS:runtime_cache> $<TARGET_OBJECTS:runtime_updates> $<TARGET_OBJECTS:runtime_processes>
    $<TARGET_OBJECTS:runtime_hosts>)
set(host_runtime_objects ${host_runtime_shared_objects} $<TARGET_OBJECTS:runtime_path_entry>
    $<TARGET_OBJECTS:runtime_install>)

function(host_executable target output_name)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE runtime_objects ${runtime_executable_libraries})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/host/src ${PROJECT_SOURCE_DIR}/runtime/src
        ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
    spisor_strict_warnings(${target})
endfunction()

host_executable(forebay_host forebay-host src/main.c $<TARGET_OBJECTS:host_objects> ${host_runtime_objects})

if(WIN32)
    set(host_boot_platform_sources src/child_windows.c src/host_arguments.c ${PROJECT_SOURCE_DIR}/runtime/src/command_line_windows.c
        ${PROJECT_SOURCE_DIR}/runtime/src/windows_process.c)
else()
    set(host_boot_platform_sources src/child_posix.c)
endif()
set(host_boot_runtime_sources alive_stamp.c boot.c boot_hint.c boot_resolve.c cache_folders.c cache_marker.c cache_place.c cache_refusal.c
    catalog_answer.c catalog_path.c catalog_text.c directory_tree.c file_stream.c file_times.c process_alive.c host_references.c hosts_lock.c
    json_service.c node_json_file.c platform_key.c run_records.c segment.c service_call.c sha256.c)
list(TRANSFORM host_boot_runtime_sources PREPEND ${PROJECT_SOURCE_DIR}/runtime/src/)

# forebay-boot changes almost never once published, so it carries only what boot.resolve needs.
add_executable(forebay_boot src/boot_main.c src/host_output.c src/host_service.c src/own_executable.c src/windows_command.c
    ${host_boot_platform_sources} ${host_boot_runtime_sources})
target_link_libraries(forebay_boot PRIVATE spisor_library)
target_include_directories(forebay_boot PRIVATE ${PROJECT_SOURCE_DIR}/host/src ${PROJECT_SOURCE_DIR}/runtime/src
    ${PROJECT_SOURCE_DIR}/src)
set_target_properties(forebay_boot PROPERTIES OUTPUT_NAME forebay-boot)
spisor_strict_warnings(forebay_boot)

if(WIN32)
    add_dependencies(forebay_host runtime_launcher)
endif()

set(FOREBAY_HOST_VERSION 0.1.0)

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
    set(host_files $<TARGET_FILE:${executable}> $<TARGET_FILE:forebay_boot>)
    if(WIN32)
        list(APPEND host_files $<TARGET_FILE:runtime_launcher>)
    endif()
    add_custom_target(${target} ALL
        COMMAND stage_bundle host ${directory}/forebay-host ${FOREBAY_HOST_VERSION} ${host_files}
        COMMAND stage_bundle library ${directory}/runtime ${PROJECT_SOURCE_DIR}/runtime/library.json $<TARGET_FILE:runtime>
        VERBATIM)
    add_dependencies(${target} stage_bundle ${executable} forebay_boot runtime)
    if(WIN32)
        add_dependencies(${target} runtime_launcher)
    endif()
endfunction()

stage_bundles(forebay_host_bundles forebay_host $<TARGET_FILE_DIR:forebay_host>/bundles)

add_subdirectory(test)
