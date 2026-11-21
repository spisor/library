spisor_bound_tests()

foreach(name json_kind_test path_test file_test javascript_test json_write_test json_file_test lock_test
    memory_error_test temporary_name_test)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE spisor_library spisor_allocator)
    spisor_strict_warnings(${name})
    target_compile_definitions(${name} PRIVATE
        SPISOR_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/path-join.json"
        SPISOR_JSON_WRITE_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/json-write.json"
        SPISOR_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

target_include_directories(memory_error_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(temporary_name_test PRIVATE Threads::Threads)
endif()

add_test(NAME public_headers_are_ascii
    COMMAND ${CMAKE_COMMAND} -DDIRECTORY=${PROJECT_SOURCE_DIR}/include -P ${CMAKE_CURRENT_SOURCE_DIR}/ascii_check.cmake)

include(${PROJECT_SOURCE_DIR}/cmake/header_isolation.cmake)
file(GLOB spisor_runtime_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/runtime/*.h)
spisor_refuse_name_in_headers(public_headers_without_cjson NAME cJSON INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/include
    HEADERS ${spisor_runtime_headers} ${PROJECT_SOURCE_DIR}/include/spisor/status.h
    ${PROJECT_SOURCE_DIR}/include/spisor/value.h ${PROJECT_SOURCE_DIR}/include/spisor/definition.h)

foreach(name status_test value_test json_value_test file_memory_test)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE spisor_library spisor_allocator_counting spisor_vector_reader)
    spisor_strict_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endforeach()
target_compile_definitions(file_memory_test PRIVATE SPISOR_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")

add_executable(restatement_test restatement_test.c)
target_include_directories(restatement_test PRIVATE ${PROJECT_SOURCE_DIR}/include
    $<TARGET_PROPERTY:weivel_standard,INTERFACE_INCLUDE_DIRECTORIES>)
spisor_strict_warnings(restatement_test)
add_test(NAME restatement_test COMMAND restatement_test)

add_executable(described_checks_test described_checks_test.c)
target_link_libraries(described_checks_test PRIVATE spisor_described_checks)
spisor_strict_warnings(described_checks_test)
add_test(NAME described_checks_test COMMAND described_checks_test)

add_executable(vector_reader_test vector_reader_test.c)
target_link_libraries(vector_reader_test PRIVATE spisor_vector_reader spisor_allocator_counting)
spisor_strict_warnings(vector_reader_test)
target_compile_definitions(vector_reader_test PRIVATE
    SPISOR_VECTOR_READER_VECTOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/vector-reader.json")
add_test(NAME vector_reader_test COMMAND vector_reader_test)

add_executable(vector_reader_shipped_allocator_test vector_reader_shipped_allocator_test.c)
target_link_libraries(vector_reader_shipped_allocator_test PRIVATE spisor_vector_reader spisor_library spisor_allocator)
spisor_strict_warnings(vector_reader_shipped_allocator_test)
target_compile_definitions(vector_reader_shipped_allocator_test PRIVATE
    SPISOR_VECTOR_READER_VECTOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/vectors/vector-reader.json")
add_test(NAME vector_reader_shipped_allocator_test COMMAND vector_reader_shipped_allocator_test)

foreach(scenario
        link_graph_lets_a_test_link_the_reader
        link_graph_refuses_a_direct_link
        link_graph_refuses_a_link_through_another_library
        link_graph_refuses_objects_taken_from_the_reader
        link_graph_refuses_a_link_under_a_condition
        link_graph_refuses_a_link_through_an_alias
        link_graph_refuses_a_shipped_target_in_a_directory_only_named_like_a_test
        link_graph_refuses_a_source_only_a_test_only_target_lists
        link_graph_refuses_a_raw_path_to_the_readers_file
        link_graph_refuses_a_link_option_naming_the_readers_file
        link_graph_checks_from_the_root_of_a_project_that_adds_this_one
        link_graph_lets_a_test_hook_twin_share_its_sources
        link_graph_refuses_a_link_flag_naming_the_readers_file
        link_graph_prints_each_offence_once
        link_graph_lets_a_binary_choose_one_allocator
        link_graph_refuses_a_library_carrying_an_allocator
        link_graph_refuses_a_binary_with_two_allocators
        link_graph_lets_a_test_build_compile_sources_from_elsewhere
        link_graph_refuses_a_test_source_behind_a_condition
        link_graph_refuses_a_source_from_a_test_directory
        link_graph_refuses_a_test_hook_definition_on_a_shipped_target
        link_graph_refuses_a_test_definition_on_a_shipped_allocator
        link_graph_sees_what_is_set_after_its_call)
    add_test(NAME ${scenario}
        COMMAND ${CMAKE_COMMAND} -DSCENARIO=${scenario} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/link-graph-work
            -DLINK_GRAPH=${PROJECT_SOURCE_DIR}/cmake/link_graph.cmake "-DGENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/link_graph/link_graph_test.cmake)
endforeach()

add_test(NAME allocation_check_refuses_a_plain_allocation
    COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/allocation-check-work
        -DALLOCATION_CHECK=${PROJECT_SOURCE_DIR}/cmake/allocation_check.cmake
        -P ${CMAKE_CURRENT_SOURCE_DIR}/allocation_check/allocation_check_test.cmake)

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
