function(runtime_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_link_libraries(${name} PRIVATE runtime_objects)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/test)
    spisor_strict_warnings(${name})
    target_compile_definitions(${name} PRIVATE RUNTIME_SCRATCH_PATH="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

foreach(name paths_test config_test log_test places_test)
    runtime_test(${name} $<TARGET_OBJECTS:runtime_secret_store>)
endforeach()

runtime_test(secrets_test fake_secret_backend.c)
