# SPISOR_SANITIZE builds every target of the tree under AddressSanitizer and UndefinedBehaviorSanitizer, halting at
# the first report. GCC and Clang only: under MSVC's /fsanitize=address every binary needs the sanitizer's runtime DLL,
# and the host tests start children with an environment of their own and stage executables away from the build tree,
# so those children fail with STATUS_DLL_NOT_FOUND (0xC0000135) before any of their code runs.

include_guard(GLOBAL)

option(SPISOR_SANITIZE "Build under AddressSanitizer and UndefinedBehaviorSanitizer (GCC and Clang)" OFF)

if(SPISOR_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "SPISOR_SANITIZE needs GCC or Clang: see cmake/sanitizers.cmake for why MSVC is not used")
    endif()
    set(spisor_sanitizer_flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_compile_options(${spisor_sanitizer_flags})
    add_link_options(${spisor_sanitizer_flags})
endif()
