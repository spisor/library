foreach(name json_kind_test path_test file_test javascript_test json_write_test json_file_test lock_test)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE spisor_library)
    spisor_strict_warnings(${name})
    target_compile_definitions(${name} PRIVATE
        SPISOR_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/path-join.json"
        SPISOR_JSON_WRITE_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/json-write.json"
        SPISOR_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

add_test(NAME public_headers_are_ascii
    COMMAND ${CMAKE_COMMAND} -DDIRECTORY=${PROJECT_SOURCE_DIR}/include -P ${CMAKE_CURRENT_SOURCE_DIR}/ascii_check.cmake)

foreach(name status_test value_test json_value_test)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE spisor_library_testing)
    spisor_strict_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

add_executable(vector_reader_test vector_reader_test.c)
target_link_libraries(vector_reader_test PRIVATE spisor_vector_reader)
spisor_strict_warnings(vector_reader_test)
target_compile_definitions(vector_reader_test PRIVATE
    SPISOR_VECTOR_READER_VECTOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/vector-reader.json")
add_test(NAME vector_reader_test COMMAND vector_reader_test)

foreach(scenario
        link_graph_lets_a_test_link_the_reader
        link_graph_refuses_a_direct_link
        link_graph_refuses_a_link_through_another_library
        link_graph_refuses_objects_taken_from_the_reader
        link_graph_refuses_a_link_under_a_condition
        link_graph_refuses_a_link_through_an_alias
        link_graph_refuses_a_shipped_target_in_a_directory_only_named_like_a_test
        link_graph_refuses_a_source_taken_from_a_test_only_directory
        link_graph_refuses_a_raw_path_to_the_readers_file
        link_graph_refuses_a_link_option_naming_the_readers_file
        link_graph_checks_from_the_root_of_a_project_that_adds_this_one)
    add_test(NAME ${scenario}
        COMMAND ${CMAKE_COMMAND} -DSCENARIO=${scenario} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/link-graph-work
            -DLINK_GRAPH=${PROJECT_SOURCE_DIR}/cmake/link_graph.cmake "-DGENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/link_graph/link_graph_test.cmake)
endforeach()

set(spisor_layouts_prefix "^(spisor|runtime)_")
add_executable(layouts layouts/layouts.c)
target_link_libraries(layouts PRIVATE spisor_library)
spisor_strict_warnings(layouts)
add_test(NAME layouts_list_every_public_struct_and_field
    COMMAND ${CMAKE_COMMAND} -DLAYOUTS=$<TARGET_FILE:layouts> -DHEADERS=${PROJECT_SOURCE_DIR}/include
        "-DPREFIX=${spisor_layouts_prefix}" -P ${PROJECT_SOURCE_DIR}/cmake/layouts_check.cmake)
foreach(scenario
        layouts_check_reads_every_shape_of_field
        layouts_check_refuses_a_struct_without_a_layout
        layouts_check_refuses_a_layout_no_header_declares
        layouts_check_refuses_a_struct_defined_after_its_forward_typedef
        layouts_check_refuses_an_anonymous_typedef_struct
        layouts_check_refuses_a_struct_with_a_comment_before_its_brace
        layouts_check_refuses_a_forgotten_field
        layouts_check_refuses_fields_out_of_order
        layouts_check_refuses_fields_without_a_sizeof_line
        layouts_check_refuses_a_malformed_line)
    add_test(NAME ${scenario}
        COMMAND ${CMAKE_COMMAND} -DSCENARIO=${scenario} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/layouts-work
            -DLAYOUTS=$<TARGET_FILE:layouts> -DHEADERS=${PROJECT_SOURCE_DIR}/include "-DPREFIX=${spisor_layouts_prefix}"
            -DCHECK_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/layouts_check.cmake
            -P ${CMAKE_CURRENT_SOURCE_DIR}/layouts/layouts_check_test.cmake)
endforeach()
