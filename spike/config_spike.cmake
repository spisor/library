add_executable(config_benchmark ${PROJECT_SOURCE_DIR}/spike/config_benchmark.c $<TARGET_OBJECTS:runtime_secret_store>
    $<TARGET_OBJECTS:runtime_path_entry>)
target_link_libraries(config_benchmark PRIVATE runtime_objects ${runtime_secret_store_libraries}
    ${runtime_path_entry_libraries})
spisor_strict_warnings(config_benchmark)
target_compile_definitions(config_benchmark PRIVATE SPIKE_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME config_definition_spike COMMAND config_benchmark)

# The Test workflow builds Debug and prints only failing tests, so the timing runs in a Release tree of its own here.
if(NOT SPIKE_NESTED_RELEASE)
    add_custom_target(spike_release_benchmark ALL
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR} -DBINARY=${CMAKE_BINARY_DIR}/spike-release
                -DGENERATOR=${CMAKE_GENERATOR} -DTARGET=config_benchmark -P ${PROJECT_SOURCE_DIR}/spike/release_benchmark.cmake
        DEPENDS config_benchmark
        VERBATIM)
endif()
