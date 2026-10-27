cmake_minimum_required(VERSION 3.15)
project(spisor_library C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(spisor_cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(spisor_cjson PUBLIC vendor/cjson)
set_target_properties(spisor_cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(spisor_library STATIC src/json_kind.c src/json_grammar.c src/javascript.c src/path.c src/file.c src/json_write.c)
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

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)
endif()
