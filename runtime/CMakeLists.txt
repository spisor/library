add_library(runtime_objects OBJECT src/segment.c src/paths.c src/merge.c src/config.c)
target_include_directories(runtime_objects PUBLIC src)
target_link_libraries(runtime_objects PUBLIC spisor_library)
set_target_properties(runtime_objects PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
spisor_strict_warnings(runtime_objects)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_library(runtime SHARED src/release.c $<TARGET_OBJECTS:runtime_objects>)
target_include_directories(runtime PRIVATE src)
target_link_libraries(runtime PRIVATE spisor_library)
set_target_properties(runtime PROPERTIES C_VISIBILITY_PRESET hidden)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(runtime PRIVATE -Wl,--exclude-libs,ALL)
endif()
spisor_strict_warnings(runtime)

add_subdirectory(test)
