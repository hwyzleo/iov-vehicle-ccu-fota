# cmake/FotaWarnings.cmake - CCU-FOTA-DSN-CR-008 §12
#
# Per-target warning / sanitizer / coverage application.
#
# Design:
#   * Applied per-target (PRIVATE), never via global CMAKE_CXX_FLAGS mutation.
#   * Compiler-aware so the same tree builds on host clang (macOS dev) and
#     target GCC (aarch64-linux-gnu cross-compile).
#   * Instrumentation (sanitizer/coverage) is dev/CI-only and never exported
#     into install rules or imported targets -> no release ABI leak.
#   * Does NOT set compiler/sysroot/target triple.

include_guard(GLOBAL)

# fota_apply_warnings(<target> [WARNINGS_AS_ERRORS])
#
# Applies a baseline warning set privately to <target>. Optional
# WARNINGS_AS_ERRORS promotes warnings to errors for that target only.
function(fota_apply_warnings target)
    cmake_parse_arguments(ARG "WARNINGS_AS_ERRORS" "" "" ${ARGN})

    set(_warnings -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)

    # Compiler-specific extras. -Wno-unknown-warning-option is clang-only;
    # on GCC it would itself warn, so guard it.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        list(APPEND _warnings -Wno-unknown-warning-option)
    endif()

    target_compile_options(${target} PRIVATE ${_warnings})

    if(FOTA_WARNINGS_AS_ERRORS OR ARG_WARNINGS_AS_ERRORS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    # Dev/CI instrumentation. Only meaningful for non-release; applied
    # PRIVATE so it never enters the installed/exported interface.
    if(FOTA_ENABLE_SANITIZER)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()

    if(FOTA_ENABLE_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage)
        target_link_options(${target} PRIVATE --coverage)
    endif()
endfunction()
