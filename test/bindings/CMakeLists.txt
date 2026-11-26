# runtime bound from its descriptions by weivel's TypeScript and Java bindings, and weivel/typescript's bundled host
# program run over it. Test-only: nothing here is linked into, copied beside or installed with a shipped artifact, and
# the weivel checkouts are read, never written: each is built into this directory. A test whose toolchain or checkout
# is missing is registered as skipped, saying what is missing, never as a pass.

spisor_bound_tests()

set(SPISOR_WEIVEL_JAVA_DIR "${CMAKE_SOURCE_DIR}/siblings/java" CACHE PATH "weivel/java at the commit repo.json pins")
set(SPISOR_BINDINGS_JAVA_HOME "$ENV{JAVA_HOME}" CACHE PATH "A JDK of release 22 or later, for the Java bindings test")

set(bindings_plugins ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
set(bindings_work ${CMAKE_CURRENT_BINARY_DIR}/work)

# Registers `name` as skipped for `reason`, which ctest prints.
function(spisor_skipped_binding name reason)
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -E echo "SKIPPED: ${reason}")
    set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
endfunction()

# Registers a fixture that lays out a fresh home for `name` holding `plugins`, and sets `out` to its directory.
function(spisor_binding_home name plugins out)
    set(base ${bindings_work}/${name})
    add_test(NAME ${name}_home COMMAND ${CMAKE_COMMAND} -DBASE=${base} -DPLUGINS=${plugins}
        -DPLUGIN_SOURCES=${bindings_plugins} -DRUNTIME=$<TARGET_FILE:runtime>
        -DDECLARATION=${PROJECT_SOURCE_DIR}/runtime/library.json -P ${CMAKE_CURRENT_SOURCE_DIR}/home.cmake)
    set_tests_properties(${name}_home PROPERTIES FIXTURES_SETUP ${name}_home)
    set(${out} ${base} PARENT_SCOPE)
endfunction()

# The places a host run reads for the home `base`, as a loader's launch sets them.
function(spisor_binding_places base out)
    set(${out} SPISOR_HOME=${base}/portable SPISOR_STATE=${base}/state SPISOR_CACHE=${base}/cache
        SPISOR_SECRETS=${base}/secrets HOME=${base}/user USERPROFILE=${base}/user PARENT_SCOPE)
endfunction()

# Registers the test `name` running COMMAND over a home of its own holding plugin `a`, with ENVIRONMENT beside the
# home's places and the path of the runtime the home holds.
function(spisor_binding_test name)
    cmake_parse_arguments(binding "" "" "COMMAND;ENVIRONMENT" ${ARGN})
    spisor_binding_home(${name} a base)
    spisor_binding_places(${base} places)
    add_test(NAME ${name} COMMAND ${binding_COMMAND})
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED ${name}_home
        ENVIRONMENT "${places};${binding_ENVIRONMENT};SPISOR_BINDINGS_RUNTIME=${base}/rt/$<TARGET_FILE_NAME:runtime>")
endfunction()

set(bindings_unavailable "")
if(SPISOR_SANITIZE)
    set(bindings_unavailable "a sanitized runtime cannot be loaded into an unsanitized node or java")
elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(bindings_unavailable "the tests lay weivel/description.h out for a 64-bit platform only")
endif()

set(typescript_unavailable "${bindings_unavailable}")
find_program(SPISOR_BINDINGS_NODE node)
find_program(SPISOR_BINDINGS_NPM NAMES npm.cmd npm)
if(typescript_unavailable STREQUAL "" AND NOT EXISTS ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/package.json)
    string(CONCAT typescript_unavailable "no weivel/typescript checkout at ${SPISOR_WEIVEL_TYPESCRIPT_DIR}: "
        "check it out at the commit repo.json pins, or set SPISOR_WEIVEL_TYPESCRIPT_DIR")
elseif(typescript_unavailable STREQUAL "" AND (NOT SPISOR_BINDINGS_NODE OR NOT SPISOR_BINDINGS_NPM))
    set(typescript_unavailable "node and npm were not found on the PATH")
endif()
if(typescript_unavailable STREQUAL "")
    file(READ ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/package.json typescript_package)
    string(JSON node_engine GET "${typescript_package}" engines node)
    string(REGEX REPLACE "^>=" "" node_needed "${node_engine}")
    execute_process(COMMAND ${SPISOR_BINDINGS_NODE} --version OUTPUT_VARIABLE node_version
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "^v" "" node_version "${node_version}")
    if(node_version VERSION_LESS node_needed)
        set(typescript_unavailable "node ${node_version} is older than the ${node_needed} weivel/typescript needs")
    endif()
endif()

if(NOT typescript_unavailable STREQUAL "")
    foreach(name bindings_typescript bindings_typescript_cost bindings_typescript_program)
        spisor_skipped_binding(${name} "${typescript_unavailable}")
    endforeach()
endif()

set(java_unavailable "${bindings_unavailable}")
set(java_executable "${SPISOR_BINDINGS_JAVA_HOME}/bin/java${CMAKE_EXECUTABLE_SUFFIX}")
if(WIN32)
    set(java_gradle ${SPISOR_WEIVEL_JAVA_DIR}/gradlew.bat)
else()
    set(java_gradle ${SPISOR_WEIVEL_JAVA_DIR}/gradlew)
endif()
if(java_unavailable STREQUAL "" AND NOT EXISTS ${java_gradle})
    string(CONCAT java_unavailable "no weivel/java checkout at ${SPISOR_WEIVEL_JAVA_DIR}: "
        "check it out at the commit repo.json pins, or set SPISOR_WEIVEL_JAVA_DIR")
elseif(java_unavailable STREQUAL "" AND NOT EXISTS ${java_executable})
    string(CONCAT java_unavailable "no JDK at SPISOR_BINDINGS_JAVA_HOME (\"${SPISOR_BINDINGS_JAVA_HOME}\"), "
        "which defaults to JAVA_HOME")
endif()
if(java_unavailable STREQUAL "")
    execute_process(COMMAND ${java_executable} -version ERROR_VARIABLE java_version OUTPUT_QUIET)
    string(REGEX MATCH "version \"([0-9]+)" java_version "${java_version}")
    if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 LESS 22)
        string(CONCAT java_unavailable "the JDK at ${SPISOR_BINDINGS_JAVA_HOME} is older than 22, "
            "the first with the foreign function API weivel/java binds a description through")
    endif()
endif()
if(NOT java_unavailable STREQUAL "")
    foreach(name bindings_java bindings_java_cost)
        spisor_skipped_binding(${name} "${java_unavailable}")
    endforeach()
endif()

if(NOT typescript_unavailable STREQUAL "" AND NOT java_unavailable STREQUAL "")
    return()
endif()

add_library(spisor_bindings_echo SHARED ${CMAKE_SOURCE_DIR}/siblings/core/test/echo/echo.c)
target_compile_definitions(spisor_bindings_echo PRIVATE ECHO_BUILDING)
set_target_properties(spisor_bindings_echo PROPERTIES OUTPUT_NAME echo C_VISIBILITY_PRESET hidden
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/echo-archive)

add_custom_target(spisor_bindings_natives ALL)
add_dependencies(spisor_bindings_natives runtime weivel_core_shared weivel_standard_shared spisor_bindings_echo)

set(bindings_paths SPISOR_BINDINGS_CORE=$<TARGET_FILE:weivel_core_shared>
    SPISOR_BINDINGS_STANDARD=$<TARGET_FILE:weivel_standard_shared>
    SPISOR_BINDINGS_ECHO=$<TARGET_FILE:spisor_bindings_echo>)

if(typescript_unavailable STREQUAL "")
    set(package ${CMAKE_CURRENT_BINARY_DIR}/weivel-typescript)
    file(MAKE_DIRECTORY ${package})
    file(GLOB_RECURSE typescript_sources CONFIGURE_DEPENDS ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/src/*.ts)
    add_custom_command(OUTPUT ${package}/installed.stamp
        COMMAND ${CMAKE_COMMAND} -E copy ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/package.json
            ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/package-lock.json ${package}
        COMMAND ${SPISOR_BINDINGS_NPM} ci --no-audit --no-fund
        COMMAND ${CMAKE_COMMAND} -E touch ${package}/installed.stamp
        DEPENDS ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/package.json ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/package-lock.json
        WORKING_DIRECTORY ${package}
        COMMENT "Installing weivel/typescript's dependencies into the build tree"
        VERBATIM)
    add_custom_command(OUTPUT ${package}/compiled.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${package}/src ${package}/dist
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/src ${package}/src
        COMMAND ${CMAKE_COMMAND} -E copy ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/tsconfig.json ${package}
        COMMAND ${SPISOR_BINDINGS_NODE} ${package}/node_modules/typescript/bin/tsc -p ${package}/tsconfig.json
        COMMAND ${CMAKE_COMMAND} -E touch ${package}/compiled.stamp
        DEPENDS ${package}/installed.stamp ${SPISOR_WEIVEL_TYPESCRIPT_DIR}/tsconfig.json ${typescript_sources}
        WORKING_DIRECTORY ${package}
        COMMENT "Compiling weivel/typescript into the build tree"
        VERBATIM)
    add_custom_target(spisor_bindings_typescript ALL DEPENDS ${package}/compiled.stamp)

    set(typescript_paths SPISOR_BINDINGS_WEIVEL_TYPESCRIPT=${package} ${bindings_paths})

    set(typescript_tests ${CMAKE_CURRENT_SOURCE_DIR}/typescript)
    spisor_binding_test(bindings_typescript COMMAND ${SPISOR_BINDINGS_NODE} ${typescript_tests}/binding.test.mts
        ENVIRONMENT ${typescript_paths})
    spisor_binding_test(bindings_typescript_cost COMMAND ${SPISOR_BINDINGS_NODE} ${typescript_tests}/cost.test.mts
        ENVIRONMENT ${typescript_paths})

    spisor_binding_home(bindings_program_one a program_one)
    spisor_binding_home(bindings_program_quarantine a,b program_quarantine)
    add_test(NAME bindings_typescript_program COMMAND ${SPISOR_BINDINGS_NODE} ${typescript_tests}/program.test.mts)
    set(program_environment ${typescript_paths} SPISOR_BINDINGS_PLUGINS=${bindings_plugins}
        SPISOR_BINDINGS_RUNTIME_NAME=$<TARGET_FILE_NAME:runtime> SPISOR_BINDINGS_HOME_ONE=${program_one}
        SPISOR_BINDINGS_HOME_QUARANTINE=${program_quarantine})
    set_tests_properties(bindings_typescript_program PROPERTIES
        FIXTURES_REQUIRED "bindings_program_one_home;bindings_program_quarantine_home"
        ENVIRONMENT "${program_environment}")
endif()

if(java_unavailable STREQUAL "")
    set(java_build ${CMAKE_CURRENT_BINARY_DIR}/weivel-java)
    set(java_jar ${java_build}/libs/weivel-java.jar)
    file(GLOB_RECURSE java_sources CONFIGURE_DEPENDS ${SPISOR_WEIVEL_JAVA_DIR}/src/main/*.java)
    add_custom_command(OUTPUT ${java_jar}
        COMMAND ${CMAKE_COMMAND} -E env JAVA_HOME=${SPISOR_BINDINGS_JAVA_HOME}
            ${java_gradle} --no-daemon --quiet -p ${SPISOR_WEIVEL_JAVA_DIR}
            --project-cache-dir ${java_build}/gradle-cache
            --init-script ${CMAKE_CURRENT_SOURCE_DIR}/java/build-into.gradle -PspisorBindingsBuild=${java_build}
            jar
        DEPENDS ${java_sources} ${SPISOR_WEIVEL_JAVA_DIR}/build.gradle
            ${CMAKE_CURRENT_SOURCE_DIR}/java/build-into.gradle
        COMMENT "Building weivel/java's jar into the build tree"
        VERBATIM)
    add_custom_target(spisor_bindings_java ALL DEPENDS ${java_jar})

    set(java_run ${java_executable} --enable-native-access=ALL-UNNAMED -cp ${java_jar}
        ${CMAKE_CURRENT_SOURCE_DIR}/java/RuntimeBinding.java)
    spisor_binding_test(bindings_java COMMAND ${java_run} ENVIRONMENT ${bindings_paths})
    spisor_binding_test(bindings_java_cost COMMAND ${java_run} cost ENVIRONMENT ${bindings_paths})
endif()
