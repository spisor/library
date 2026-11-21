# The weivel checkouts live in the top-level project's siblings/, where its repo file's pins put them: a library built
# beneath another project, as forebay-library builds this one, finds the pins of that project rather than its own.
# Their own tests are not added: each builds them only as the top-level project.

include_guard(GLOBAL)

function(spisor_weivel_sibling sibling)
    set(checkout ${CMAKE_SOURCE_DIR}/siblings/${sibling})
    if(NOT EXISTS ${checkout}/CMakeLists.txt)
        message(FATAL_ERROR "siblings/${sibling} is missing: check out weivel/${sibling} there at the commit "
            "repo.json pins, as CI does")
    endif()
    set(spisor_weivel_checkout ${checkout} PARENT_SCOPE)
endfunction()

# The core and the standard, which every build needs: runtime links the standard statically.
function(spisor_add_weivel_standard)
    if(TARGET weivel_standard)
        return()
    endif()
    spisor_weivel_sibling(core)
    add_subdirectory(${spisor_weivel_checkout} ${CMAKE_BINARY_DIR}/weivel/core EXCLUDE_FROM_ALL)
    spisor_weivel_sibling(standard)
    add_subdirectory(${spisor_weivel_checkout} ${CMAKE_BINARY_DIR}/weivel/standard EXCLUDE_FROM_ALL)
endfunction()

# The C language layer, which only the tests load runtime's declaration through.
function(spisor_add_weivel)
    spisor_add_weivel_standard()
    if(TARGET weivel_c)
        return()
    endif()
    set(siblings ${CMAKE_SOURCE_DIR}/siblings)
    spisor_weivel_sibling(c)
    set(WEIVEL_C_CORE_INCLUDE_DIRECTORY ${siblings}/core/include CACHE PATH "The core's include directory")
    set(WEIVEL_C_STANDARD_INCLUDE_DIRECTORY ${siblings}/standard/include CACHE PATH "The standard's include directory")
    set(WEIVEL_C_STANDARD_SOURCE_DIRECTORY ${siblings}/standard CACHE PATH "The standard checkout")
    set(WEIVEL_C_CORE_SOURCE_DIRECTORY ${siblings}/core CACHE PATH "The core checkout")
    add_subdirectory(${spisor_weivel_checkout} ${CMAKE_BINARY_DIR}/weivel/c EXCLUDE_FROM_ALL)
endfunction()
