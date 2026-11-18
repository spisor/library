# Requires the exports of LIBRARY, as LISTER reads them in FORMAT (pe, elf or macho), to be exactly EXPECTED, a
# comma-separated list of names.

string(REPLACE "," ";" EXPECTED "${EXPECTED}")

if(FORMAT STREQUAL "pe")
    execute_process(COMMAND "${LISTER}" /dump /exports "${LIBRARY}" OUTPUT_VARIABLE listing RESULT_VARIABLE result)
    string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ +[0-9A-F]+ +[A-Za-z_][A-Za-z0-9_]*" rows "${listing}")
elseif(FORMAT STREQUAL "elf")
    execute_process(COMMAND "${LISTER}" -D --defined-only "${LIBRARY}" OUTPUT_VARIABLE listing RESULT_VARIABLE result)
    string(REGEX MATCHALL "\n[0-9a-f]+ [TDRBVW] [A-Za-z_][A-Za-z0-9_@.]*" rows "\n${listing}")
elseif(FORMAT STREQUAL "macho")
    execute_process(COMMAND "${LISTER}" -gU "${LIBRARY}" OUTPUT_VARIABLE listing RESULT_VARIABLE result)
    string(REGEX MATCHALL "\n[0-9a-f]+ [TDS] _[A-Za-z_][A-Za-z0-9_]*" rows "\n${listing}")
else()
    message(FATAL_ERROR "FORMAT must be pe, elf or macho, not '${FORMAT}'")
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${LISTER} could not list the exports of ${LIBRARY}: ${result}")
endif()

set(exported "")
foreach(row IN LISTS rows)
    string(REGEX MATCH "[A-Za-z_][A-Za-z0-9_@.]*$" name "${row}")
    if(FORMAT STREQUAL "macho")
        string(SUBSTRING "${name}" 1 -1 name)
    endif()
    # A name the C standard reserves, such as an ELF loader's _init, is the toolchain's, never the library's.
    if(NOT name MATCHES "^_")
        list(APPEND exported "${name}")
    endif()
endforeach()
list(REMOVE_DUPLICATES exported)

set(unexpected ${exported})
list(REMOVE_ITEM unexpected ${EXPECTED})
set(missing ${EXPECTED})
list(REMOVE_ITEM missing ${exported})
if(unexpected OR missing)
    message(FATAL_ERROR "${LIBRARY} exports what it must not or lacks a definition:\n"
            "  exported but not expected: ${unexpected}\n  expected but not exported: ${missing}")
endif()
list(LENGTH exported count)
message(STATUS "${LIBRARY} exports exactly its ${count} expected symbols")
