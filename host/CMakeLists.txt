set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

set(host_sources src/application_choices.c src/boot_placement.c src/catalog_inputs.c src/catalog_writer.c
    src/command_background.c src/command_catalog_build.c src/command_hook.c src/command_install.c src/command_launch.c
    src/command_remove.c src/command_status.c src/command_update.c src/host_context.c src/host_holds.c src/host_options.c
    src/host_output.c src/host_start.c src/loader_start.c src/terminal_question.c src/windows_command.c)
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

host_executable(forebay_boot forebay-boot src/boot_main.c src/host_output.c src/host_context.c src/windows_command.c
    ${host_platform_sources} ${host_runtime_objects})

if(WIN32)
    add_dependencies(forebay_host runtime_launcher)
endif()

add_subdirectory(test)
