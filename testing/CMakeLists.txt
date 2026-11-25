add_library(spisor_vector_reader STATIC EXCLUDE_FROM_ALL vector_reader.c value_equal.c)
target_include_directories(spisor_vector_reader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(spisor_vector_reader PUBLIC spisor_library)
set_target_properties(spisor_vector_reader PROPERTIES SPISOR_TEST_ONLY TRUE)
spisor_strict_warnings(spisor_vector_reader)

add_library(spisor_described_checks STATIC EXCLUDE_FROM_ALL described_checks.c described_library.c)
target_include_directories(spisor_described_checks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spisor_described_checks PUBLIC weivel_standard ${CMAKE_DL_LIBS})
set_target_properties(spisor_described_checks PROPERTIES SPISOR_TEST_ONLY TRUE)
spisor_strict_warnings(spisor_described_checks)

# Linked only where weivel/c is added, as every project testing a library on its host does.
add_library(spisor_another_definition_check STATIC EXCLUDE_FROM_ALL another_definition.c)
target_include_directories(spisor_another_definition_check PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spisor_another_definition_check PUBLIC weivel_c_standard)
set_target_properties(spisor_another_definition_check PROPERTIES SPISOR_TEST_ONLY TRUE)
spisor_strict_warnings(spisor_another_definition_check)

# spisor_add_described_checks(LIBRARY <shared library target> DECLARATION <library.json>) adds
# <LIBRARY>_short_shape_test and <LIBRARY>_empty_answer_test, which load LIBRARY as a consumer would and check every
# definition DECLARATION gives a description.
function(spisor_add_described_checks)
    cmake_parse_arguments(PARSE_ARGV 0 DESCRIBED "" "LIBRARY;DECLARATION" "")
    foreach(required IN ITEMS LIBRARY DECLARATION)
        if(NOT DESCRIBED_${required})
            message(FATAL_ERROR "spisor_add_described_checks needs ${required}")
        endif()
    endforeach()
    foreach(check IN ITEMS short_shape_test empty_answer_test)
        set(name ${DESCRIBED_LIBRARY}_${check})
        add_executable(${name} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${check}.c)
        target_compile_definitions(${name} PRIVATE SPISOR_DESCRIBED_LIBRARY_PATH="$<TARGET_FILE:${DESCRIBED_LIBRARY}>"
            SPISOR_DESCRIBED_DECLARATION_PATH="${DESCRIBED_DECLARATION}")
        target_link_libraries(${name} PRIVATE spisor_described_checks)
        add_dependencies(${name} ${DESCRIBED_LIBRARY})
        spisor_strict_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endfunction()
