foreach(name json_kind_test path_test file_test javascript_test)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE spisor_library)
    spisor_strict_warnings(${name})
    target_compile_definitions(${name} PRIVATE
        SPISOR_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/path-join.json"
        SPISOR_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endforeach()

add_test(NAME public_headers_are_ascii
    COMMAND ${CMAKE_COMMAND} -DDIRECTORY=${PROJECT_SOURCE_DIR}/include -P ${CMAKE_CURRENT_SOURCE_DIR}/ascii_check.cmake)
