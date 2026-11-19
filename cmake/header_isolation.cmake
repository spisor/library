# spisor_refuse_name_in_headers(<target> NAME <identifier> HEADERS <header>... INCLUDE_DIRECTORIES <directory>...)
# builds, as part of every build, one source that poisons NAME and then includes every HEADER with only the
# INCLUDE_DIRECTORIES on its include path. The build fails when a header, or any header it includes, names NAME in its
# code or includes a header the include path cannot reach, so a public header that leaks a dependency such as cJSON to
# its consumers is refused even while the library itself links that dependency publicly.

include_guard(GLOBAL)

function(spisor_refuse_name_in_headers target)
    cmake_parse_arguments(PARSE_ARGV 1 REFUSE "" "NAME" "HEADERS;INCLUDE_DIRECTORIES")
    if(NOT REFUSE_NAME OR NOT REFUSE_HEADERS)
        message(FATAL_ERROR "spisor_refuse_name_in_headers needs a NAME and at least one header")
    endif()
    string(CONCAT content "#if defined(_MSC_VER)\n#pragma deprecated(${REFUSE_NAME})\n#else\n"
        "#pragma GCC poison ${REFUSE_NAME}\n#endif\n\n")
    foreach(header IN LISTS REFUSE_HEADERS)
        string(APPEND content "#include \"${header}\"\n")
    endforeach()
    set(source ${CMAKE_CURRENT_BINARY_DIR}/${target}.c)
    file(CONFIGURE OUTPUT ${source} CONTENT "${content}")
    add_library(${target} OBJECT ${source})
    target_include_directories(${target} PRIVATE ${REFUSE_INCLUDE_DIRECTORIES})
    set_target_properties(${target} PROPERTIES SPISOR_TEST_ONLY TRUE)
    spisor_strict_warnings(${target})
endfunction()
