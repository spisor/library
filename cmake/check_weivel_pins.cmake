# -DREPO=<repository root>
#
# Fails naming each of weivel's `core` and `standard` whose pinned commit in the repository's repo.json differs from
# the one siblings/typescript/repo.json or siblings/rust/repo.json pins, so every language host loads one weivel. It
# reads JSON with string(JSON), so no language but CMake runs it.

cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED REPO)
    message(FATAL_ERROR "REPO is required: the repository root")
endif()

function(pinned_commit file import_name result)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} is missing: check out each weivel language at siblings/<language> at the "
            "commit repo.json pins")
    endif()
    file(READ "${file}" text)
    string(JSON count LENGTH "${text}" dependencies)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON name GET "${text}" dependencies ${index} importName)
        if(name STREQUAL import_name)
            string(JSON commit GET "${text}" dependencies ${index} commit)
            set(${result} "${commit}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "${file} pins no dependency named ${import_name}")
endfunction()

set(differences "")
foreach(language typescript rust)
    foreach(import_name core standard)
        pinned_commit("${REPO}/repo.json" ${import_name} own)
        pinned_commit("${REPO}/siblings/${language}/repo.json" ${import_name} theirs)
        if(NOT own STREQUAL theirs)
            string(APPEND differences "\n  ${import_name}: ${own} here, ${theirs} in siblings/${language}")
        endif()
    endforeach()
endforeach()

if(differences)
    message(FATAL_ERROR "the library and a weivel language it pins pin different weivel commits:${differences}")
endif()
