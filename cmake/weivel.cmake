# spisor_add_weivel() adds the weivel core, standard and C language layer from siblings/, where the repo file's pins
# put them, so runtime's tests read its declaration with the standard and load it through the C host on that core.
# Their own tests are not added: each builds them only as the top-level project.

include_guard(GLOBAL)

function(spisor_add_weivel)
    set(siblings ${PROJECT_SOURCE_DIR}/siblings)
    foreach(sibling IN ITEMS core standard c)
        if(NOT EXISTS ${siblings}/${sibling}/CMakeLists.txt)
            message(FATAL_ERROR "siblings/${sibling} is missing: check out weivel/${sibling} there at the commit "
                ".weivel/repo.json pins, as CI does")
        endif()
    endforeach()
    add_subdirectory(${siblings}/core ${PROJECT_BINARY_DIR}/weivel/core EXCLUDE_FROM_ALL)
    add_subdirectory(${siblings}/standard ${PROJECT_BINARY_DIR}/weivel/standard EXCLUDE_FROM_ALL)
    set(WEIVEL_C_CORE_INCLUDE_DIRECTORY ${siblings}/core/include CACHE PATH "The core's include directory")
    set(WEIVEL_C_STANDARD_INCLUDE_DIRECTORY ${siblings}/standard/include CACHE PATH "The standard's include directory")
    set(WEIVEL_C_STANDARD_SOURCE_DIRECTORY ${siblings}/standard CACHE PATH "The standard checkout")
    set(WEIVEL_C_CORE_SOURCE_DIRECTORY ${siblings}/core CACHE PATH "The core checkout")
    add_subdirectory(${siblings}/c ${PROJECT_BINARY_DIR}/weivel/c EXCLUDE_FROM_ALL)
endfunction()
