cmake_minimum_required(VERSION 3.19)
project(spisor_library C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# An unchanged source must build unchanged bytes: a shim is rewritten whenever runtime-launcher.exe's bytes differ.
if(MSVC)
    add_compile_options(/Brepro)
    add_link_options(/Brepro)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/sanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/weivel.cmake)
spisor_add_weivel_standard()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/naula.cmake)
spisor_add_naula()

set(spisor_library_sources src/javascript.c src/path.c src/file.c src/wide_path.c
    src/windows_file.c src/prepared_undo.c src/lock.c src/json_write.c src/json_file.c src/refusal.c
    src/value.c src/value_block.c src/json_value.c src/entry_order.c src/json_allocation.c src/definition.c)
add_library(spisor_library STATIC ${spisor_library_sources})
target_include_directories(spisor_library PUBLIC include $<TARGET_PROPERTY:naula,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(spisor_library PUBLIC naula_cjson)
set_target_properties(spisor_library PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(UNIX)
    target_link_libraries(spisor_library PUBLIC m)
endif()

function(spisor_strict_warnings target)
    if(MSVC)
        # An option, not a definition: the link graph reads every definition a test-only target carries as test-only.
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

spisor_strict_warnings(spisor_library)

# spisor_library carries no allocator: every binary links exactly one of these two, so a test can count and fail
# allocations without a second definition of the allocator beside it. Each also answers naula's allocator, so naula's
# blocks, cJSON's included, are counted with the library's. spisor_library reads JSON through naula, so a binary
# linking it also links one of naula and naula_test_hooks.
add_library(spisor_allocator OBJECT src/allocation.c src/naula_allocator.c)
target_include_directories(spisor_allocator PRIVATE include $<TARGET_PROPERTY:naula,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(spisor_allocator PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden SPISOR_ALLOCATOR TRUE)
spisor_strict_warnings(spisor_allocator)

add_library(spisor_allocator_counting OBJECT EXCLUDE_FROM_ALL src/allocation.c src/naula_allocator.c)
target_include_directories(spisor_allocator_counting PUBLIC src include
    $<TARGET_PROPERTY:naula,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(spisor_allocator_counting PUBLIC SPISOR_TESTING)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(spisor_allocator_counting PUBLIC Threads::Threads)
endif()
set_target_properties(spisor_allocator_counting PROPERTIES SPISOR_TEST_ONLY TRUE SPISOR_ALLOCATOR TRUE)
spisor_strict_warnings(spisor_allocator_counting)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/link_graph.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/allocation_check.cmake)
spisor_refuse_plain_allocation(${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src
    ${CMAKE_CURRENT_SOURCE_DIR}/host/src ${CMAKE_CURRENT_SOURCE_DIR}/host/bundle)
add_subdirectory(testing)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_timeouts.cmake)
    spisor_refuse_unbounded_tests("${CMAKE_CURRENT_SOURCE_DIR}")
    spisor_add_weivel()
    add_subdirectory(test)
    add_subdirectory(runtime)
    add_subdirectory(host)
endif()

spisor_refuse_test_only_links("${CMAKE_CURRENT_SOURCE_DIR}")
