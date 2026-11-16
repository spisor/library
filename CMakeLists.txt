cmake_minimum_required(VERSION 3.15)
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

add_library(spisor_cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(spisor_cjson PUBLIC vendor/cjson)
set_target_properties(spisor_cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(spisor_library_sources src/json_kind.c src/json_grammar.c src/javascript.c src/path.c src/file.c src/wide_path.c
    src/windows_file.c src/lock.c src/json_write.c src/json_file.c src/utf8.c src/refusal.c src/value.c
    src/value_block.c src/json_value.c src/entry_order.c)
add_library(spisor_library STATIC ${spisor_library_sources})
target_include_directories(spisor_library PUBLIC include)
target_link_libraries(spisor_library PUBLIC spisor_cjson)
set_target_properties(spisor_library PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
    target_link_libraries(spisor_library PUBLIC m)
endif()

function(spisor_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

spisor_strict_warnings(spisor_library)

# spisor_library carries no allocator: every binary links exactly one of these two, so a test can count and fail
# allocations without a second definition of the allocator beside it.
add_library(spisor_allocator OBJECT src/allocation.c)
set_target_properties(spisor_allocator PROPERTIES POSITION_INDEPENDENT_CODE ON SPISOR_ALLOCATOR TRUE)
spisor_strict_warnings(spisor_allocator)

add_library(spisor_allocator_counting OBJECT EXCLUDE_FROM_ALL src/allocation.c)
target_include_directories(spisor_allocator_counting PUBLIC src)
target_compile_definitions(spisor_allocator_counting PUBLIC SPISOR_TESTING)
set_target_properties(spisor_allocator_counting PROPERTIES SPISOR_TEST_ONLY TRUE SPISOR_ALLOCATOR TRUE)
spisor_strict_warnings(spisor_allocator_counting)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/link_graph.cmake)
add_subdirectory(testing)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)
    add_subdirectory(runtime)
    add_subdirectory(host)
endif()

spisor_refuse_test_only_links("${CMAKE_CURRENT_SOURCE_DIR}")
