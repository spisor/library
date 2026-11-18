set(SPISOR_PLAIN_ALLOCATION "(^|[^A-Za-z0-9_.>])(malloc|calloc|realloc|free|strdup|_strdup)[ \t]*\\(")

function(spisor_plain_allocations_in directory out_variable)
    file(GLOB_RECURSE sources "${directory}/*.c" "${directory}/*.h")
    set(found "")
    foreach(source IN LISTS sources)
        if(source MATCHES "/src/allocation\\.c$")
            continue()
        endif()
        file(STRINGS "${source}" calls REGEX "${SPISOR_PLAIN_ALLOCATION}")
        foreach(call IN LISTS calls)
            string(STRIP "${call}" call)
            string(APPEND found "\n  ${source}: ${call}")
        endforeach()
    endforeach()
    set(${out_variable} "${found}" PARENT_SCOPE)
endfunction()

#[[
Refuses a shipped source that allocates or releases past the binary's one allocator, since a test's counting
allocator cannot fail a plain malloc. Vendored code is not named, and src/allocation.c is the allocator itself.
]]
function(spisor_refuse_plain_allocation)
    set(found "")
    foreach(directory IN LISTS ARGN)
        spisor_plain_allocations_in("${directory}" in_directory)
        string(APPEND found "${in_directory}")
    endforeach()
    if(NOT found STREQUAL "")
        message(FATAL_ERROR
            "allocate through spisor_memory_allocate and release through spisor_memory_release:${found}")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    spisor_refuse_plain_allocation(${SPISOR_ALLOCATION_DIRECTORIES})
endif()
