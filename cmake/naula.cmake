# naula lives in the top-level project's siblings/, where its repo file's pin puts it, as the weivel checkouts do.
# Its own tests are not added: it builds them only as the top-level project.

include_guard(GLOBAL)

function(spisor_add_naula)
    if(TARGET naula)
        return()
    endif()
    set(checkout ${CMAKE_SOURCE_DIR}/siblings/naula)
    if(NOT EXISTS ${checkout}/CMakeLists.txt)
        message(FATAL_ERROR "siblings/naula is missing: check out intisy/naula there at the commit repo.json pins, "
            "as CI does")
    endif()
    set(NAULA_TESTS OFF)
    add_subdirectory(${checkout} ${CMAKE_BINARY_DIR}/naula EXCLUDE_FROM_ALL)
    set_target_properties(naula_test_hooks PROPERTIES SPISOR_TEST_ONLY TRUE)
endfunction()
