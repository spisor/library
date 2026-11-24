# Test-only, since it forges registrations, hints and install markers; runtime_testing carries runtime's objects from
# the same commit, so its writers and the readers a test loads beside it are one source.
add_library(runtime_testing_objects OBJECT src/home_layout.c src/home_prepare.c src/home_write.c src/home.c
    src/real_places.c src/definition.c)
target_include_directories(runtime_testing_objects PUBLIC include PRIVATE src ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_testing_objects PUBLIC runtime_objects)
target_compile_definitions(runtime_testing_objects PRIVATE RUNTIME_TESTING_BUILDING)
set_target_properties(runtime_testing_objects PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
    SPISOR_TEST_ONLY TRUE)
spisor_strict_warnings(runtime_testing_objects)

add_library(runtime_testing SHARED ${PROJECT_SOURCE_DIR}/runtime/src/library_load.c ${runtime_shipped_objects})
target_compile_definitions(runtime_testing PRIVATE RUNTIME_TESTING_BUILDING)
target_include_directories(runtime_testing PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(runtime_testing PRIVATE runtime_testing_objects spisor_library spisor_allocator weivel_standard
    ${runtime_secret_store_libraries} ${runtime_path_entry_libraries} ${runtime_fetch_libraries})
set_target_properties(runtime_testing PROPERTIES C_VISIBILITY_PRESET hidden SPISOR_TEST_ONLY TRUE)
set(runtime_testing_exports runtime_testing_definition_of runtime_testing_definition_description)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(JOIN "; " runtime_testing_export_names ${runtime_testing_exports})
    set(runtime_testing_version_script ${CMAKE_CURRENT_BINARY_DIR}/runtime_testing.map)
    file(CONFIGURE OUTPUT ${runtime_testing_version_script}
        CONTENT "{\n  global: ${runtime_testing_export_names};\n  local: *;\n};\n")
    target_link_options(runtime_testing PRIVATE "LINKER:--version-script=${runtime_testing_version_script}")
    set_property(TARGET runtime_testing APPEND PROPERTY LINK_DEPENDS ${runtime_testing_version_script})
elseif(APPLE)
    list(TRANSFORM runtime_testing_exports PREPEND "_" OUTPUT_VARIABLE runtime_testing_macho_exports)
    string(JOIN "\n" runtime_testing_macho_lines ${runtime_testing_macho_exports})
    set(runtime_testing_export_list ${CMAKE_CURRENT_BINARY_DIR}/runtime_testing_exports.txt)
    file(CONFIGURE OUTPUT ${runtime_testing_export_list} CONTENT "${runtime_testing_macho_lines}\n")
    target_link_options(runtime_testing PRIVATE "LINKER:-exported_symbols_list,${runtime_testing_export_list}")
    set_property(TARGET runtime_testing APPEND PROPERTY LINK_DEPENDS ${runtime_testing_export_list})
endif()
spisor_strict_warnings(runtime_testing)

weivel_describe(runtime_testing
    HEADERS ${PROJECT_SOURCE_DIR}/include/spisor/status.h ${PROJECT_SOURCE_DIR}/include/runtime/definition.h
        ${PROJECT_SOURCE_DIR}/include/runtime/environment.h ${PROJECT_SOURCE_DIR}/include/runtime/item_version.h
        ${PROJECT_SOURCE_DIR}/include/runtime/boot.h ${CMAKE_CURRENT_SOURCE_DIR}/include/runtime_testing/home.h
    PREFIX runtime_testing_described_
    EXPORT RUNTIME_TESTING_API
    STATUS spisor_status
    REFUSAL spisor_refusal
    VALUE spisor_value
    TREE spisor_value_tree
    SERVICE runtime_testing_definition SYMBOL runtime_testing_definition_description
        ANSWERED_BY runtime_testing_definition_of)
