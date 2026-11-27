# Prepares typescript-host's inputs and stages it through stage-bundle's language-host mode, which names no language:
# weivel/typescript's program bundled by its own weivel-bundle, koffi's package with this platform's prebuilt alone,
# and host data carrying the in-process entry and the Node pin checked against the checkout's engines.node.
#
# cmake -DSTAGE_BUNDLE=<stage-bundle> -DDIRECTORY=<bundle> -DVERSION=<version> -DCHECKOUT=<weivel/typescript>
#       -DNODE_PIN=<node.json> -DCORE=<weivel_core> -DSTANDARD=<weivel_standard> -DSCRATCH=<inputs folder>
#       -P typescript_host_bundle.cmake
cmake_minimum_required(VERSION 3.19)

foreach(input STAGE_BUNDLE DIRECTORY VERSION CHECKOUT NODE_PIN CORE STANDARD SCRATCH)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "typescript-host: -D${input} is missing")
    endif()
endforeach()

function(read_json path variable)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "typescript-host: ${path} does not exist")
    endif()
    file(READ "${path}" text)
    string(JSON kind ERROR_VARIABLE problem TYPE "${text}")
    if(problem OR NOT kind STREQUAL "OBJECT")
        message(FATAL_ERROR "typescript-host: ${path} is no JSON object")
    endif()
    set(${variable} "${text}" PARENT_SCOPE)
endfunction()

function(json_text json variable)
    string(JSON value ERROR_VARIABLE problem GET "${json}" ${ARGN})
    if(problem)
        set(value "")
    endif()
    set(${variable} "${value}" PARENT_SCOPE)
endfunction()

function(platform_key variable)
    execute_process(COMMAND "${STAGE_BUNDLE}" platform OUTPUT_VARIABLE key RESULT_VARIABLE result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0 OR key STREQUAL "")
        message(FATAL_ERROR "typescript-host: ${STAGE_BUNDLE} platform answered ${result}")
    endif()
    set(${variable} "${key}" PARENT_SCOPE)
endfunction()

set(plain_version "^[0-9]+\\.[0-9]+\\.[0-9]+$")

function(read_minimum variable)
    read_json("${CHECKOUT}/package.json" package)
    json_text("${package}" engine engines node)
    string(REGEX REPLACE "^>=" "" minimum "${engine}")
    if(NOT engine MATCHES "^>=" OR NOT minimum MATCHES "${plain_version}")
        if(engine STREQUAL "")
            set(engine "(none)")
        endif()
        message(FATAL_ERROR "typescript-host: ${CHECKOUT}/package.json names engines.node ${engine}, "
            "not >=<major>.<minor>.<patch>")
    endif()
    set(${variable} "${minimum}" PARENT_SCOPE)
endfunction()

function(check_build builds platform)
    json_text("${builds}" url ${platform} url)
    json_text("${builds}" sha256 ${platform} sha256)
    if(NOT url MATCHES "^https://.*(\\.zip|\\.tar\\.gz)$")
        if(url STREQUAL "")
            set(url "no url")
        endif()
        message(FATAL_ERROR "typescript-host: ${NODE_PIN} pins ${url} for ${platform}, not an https url ending .zip or "
            ".tar.gz")
    endif()
    string(LENGTH "${sha256}" length)
    if(NOT sha256 MATCHES "^[0-9a-f]+$" OR NOT length EQUAL 64)
        if(sha256 STREQUAL "")
            set(sha256 "no sha256")
        endif()
        message(FATAL_ERROR "typescript-host: ${NODE_PIN} pins ${sha256} for ${platform}, not a sha256 of 64 lowercase "
            "hexadecimal digits")
    endif()
endfunction()

# The pin is {pinned, builds}: pinned no older than the checkout's minimum, every build one an install can fetch and
# check, this platform's among them.
function(check_pin pin minimum platform)
    json_text("${pin}" pinned pinned)
    string(JSON builds_kind ERROR_VARIABLE problem TYPE "${pin}" builds)
    if(NOT pinned MATCHES "${plain_version}" OR problem OR NOT builds_kind STREQUAL "OBJECT")
        message(FATAL_ERROR "typescript-host: ${NODE_PIN} is not {\"pinned\": \"<major>.<minor>.<patch>\", "
            "\"builds\": {...}}")
    endif()
    if(pinned VERSION_LESS minimum)
        message(FATAL_ERROR "typescript-host: ${NODE_PIN} pins Node ${pinned}, older than the ${minimum} the program "
            "needs")
    endif()
    json_text("${pin}" builds builds)
    string(JSON count LENGTH "${builds}")
    set(has_platform FALSE)
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON key MEMBER "${builds}" ${index})
            check_build("${builds}" "${key}")
            if(key STREQUAL platform)
                set(has_platform TRUE)
            endif()
        endforeach()
    endif()
    if(NOT has_platform)
        message(FATAL_ERROR "typescript-host: ${NODE_PIN} pins no build for ${platform}, the platform this bundle is "
            "staged for")
    endif()
endfunction()

function(run_bundler)
    find_program(node_program node)
    if(NOT node_program)
        message(FATAL_ERROR "typescript-host: no node on the PATH runs the bundler")
    endif()
    execute_process(COMMAND "${node_program}" "${CHECKOUT}/dist/bundle/bin.js" program "${SCRATCH}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "typescript-host: ${CHECKOUT}/dist/bundle/bin.js program ${SCRATCH} failed: ${result}")
    endif()
endfunction()

# koffi names a prebuilt folder by os_arch, the platform key with an underscore, and its sources and every other
# platform's prebuilt are never loaded, so a bundle built for one platform carries one.
function(copy_koffi platform)
    set(package "${CHECKOUT}/node_modules/koffi")
    set(target "${SCRATCH}/node_modules/koffi")
    string(REPLACE "-" "_" triplet "${platform}")
    foreach(name package.json index.js LICENSE.txt build/koffi/${triplet}/koffi.node)
        if(NOT EXISTS "${package}/${name}")
            message(FATAL_ERROR "typescript-host: ${package}/${name} does not exist")
        endif()
        get_filename_component(folder "${target}/${name}" DIRECTORY)
        file(MAKE_DIRECTORY "${folder}")
        file(COPY "${package}/${name}" DESTINATION "${folder}")
    endforeach()
endfunction()

function(write_host_data path pin minimum)
    json_text("${pin}" pinned pinned)
    json_text("${pin}" builds builds)
    set(node "{}")
    string(JSON node SET "${node}" pinned "\"${pinned}\"")
    string(JSON node SET "${node}" minimum "\"${minimum}\"")
    string(JSON node SET "${node}" builds "${builds}")
    set(data "{}")
    string(JSON data SET "${data}" inProcessEntry "\"weivel-host-in-process.mjs\"")
    string(JSON data SET "${data}" node "${node}")
    file(WRITE "${path}" "${data}\n")
endfunction()

platform_key(platform)
read_minimum(minimum)
read_json("${NODE_PIN}" pin)
check_pin("${pin}" "${minimum}" "${platform}")
file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}")
run_bundler()
copy_koffi("${platform}")
set(host_data "${SCRATCH}.host-data.json")
write_host_data("${host_data}" "${pin}" "${minimum}")
execute_process(COMMAND "${STAGE_BUNDLE}" language-host "${DIRECTORY}" "${VERSION}" typescript javascript
    weivel-host.mjs "${host_data}" "${CORE}" "${STANDARD}" weivel-host-in-process.mjs node_modules/koffi
    WORKING_DIRECTORY "${SCRATCH}" RESULT_VARIABLE staged)
if(NOT staged EQUAL 0)
    message(FATAL_ERROR "typescript-host: stage-bundle language-host answered ${staged}")
endif()
