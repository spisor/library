# -DWORK=<dir> -DALLOCATION_CHECK=<allocation_check.cmake>

cmake_minimum_required(VERSION 3.19)

file(REMOVE_RECURSE "${WORK}")

function(expect_check contents must_contain)
    file(WRITE "${WORK}/src/probe.c" "${contents}\n")
    execute_process(COMMAND ${CMAKE_COMMAND} "-DSPISOR_ALLOCATION_DIRECTORIES=${WORK}/src" -P "${ALLOCATION_CHECK}"
                    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " output "${out}${err}")
    string(FIND "${output}" "${must_contain}" position)
    if(must_contain STREQUAL "")
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "refused '${contents}': ${output}")
        endif()
    elseif(code EQUAL 0 OR position EQUAL -1)
        message(FATAL_ERROR "'${contents}' answered ${code} without '${must_contain}': ${output}")
    endif()
endfunction()

expect_check("void f(void *block) { spisor_memory_release(block); LocalFree(block); cJSON_free(block); }" "")
expect_check("void f(const char *text) { char *copy = spisor_memory_duplicate_text(text); }" "")
foreach(call IN ITEMS "malloc(8)" "calloc(1, 8)" "realloc(block, 8)" "free(block)" "free (block)" "strdup(text)"
                      "_strdup(text)" "(malloc)(8)" "( free ) (block)" "(_strdup)(text)")
    expect_check("void f(void) { ${call}; }" "probe.c: void f(void) { ${call}; }")
endforeach()
file(WRITE "${WORK}/src/allocation.c" "void *spisor_memory_allocate(unsigned bytes) { return malloc(bytes); }\n")
expect_check("int g;" "")
