# spisor_bound_tests() gives every test of the calling directory that sets no TIMEOUT of its own a TIMEOUT of
# SPISOR_TEST_TIMEOUT_SECONDS, at the end of that directory, so a test registered after the call is bound too. Without
# the CTest module ctest bounds nothing, so a test that waits on a process runs until the CI job is cancelled, and the
# cancellation discards the output ctest holds for it.
#
# spisor_refuse_unbounded_tests(<root directory>) fails the configure when a directory below root registers a test
# without calling spisor_bound_tests(). It defers the check to the end of the top-level directory, so it sees every
# directory the configure adds.

include_guard(GLOBAL)

set(SPISOR_TEST_TIMEOUT_SECONDS 300)

function(spisor_apply_test_timeouts seconds)
    get_property(tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS tests)
        get_test_property(${test} TIMEOUT timeout)
        if(NOT timeout)
            set_tests_properties(${test} PROPERTIES TIMEOUT ${seconds})
        endif()
    endforeach()
endfunction()

macro(spisor_bound_tests)
    set_property(DIRECTORY PROPERTY SPISOR_TESTS_BOUNDED TRUE)
    cmake_language(EVAL CODE "cmake_language(DEFER CALL spisor_apply_test_timeouts ${SPISOR_TEST_TIMEOUT_SECONDS})")
endmacro()

function(spisor_unbounded_test_directories directory out)
    get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
    get_property(bounded DIRECTORY "${directory}" PROPERTY SPISOR_TESTS_BOUNDED)
    set(unbounded "")
    if(tests AND NOT bounded)
        list(APPEND unbounded "${directory}")
    endif()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        spisor_unbounded_test_directories("${subdirectory}" nested)
        list(APPEND unbounded ${nested})
    endforeach()
    set(${out} "${unbounded}" PARENT_SCOPE)
endfunction()

function(spisor_check_bounded_tests root)
    spisor_unbounded_test_directories("${root}" unbounded)
    foreach(directory IN LISTS unbounded)
        message(SEND_ERROR "${directory} registers tests without calling spisor_bound_tests(), so ctest would bound none of them")
    endforeach()
endfunction()

function(spisor_refuse_unbounded_tests root)
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL spisor_check_bounded_tests [[${root}]])")
endfunction()
