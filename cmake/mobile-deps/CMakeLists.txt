cmake_minimum_required(VERSION 3.18)
project(SunshineClientMobileDependencies NONE)

set(SUNSHINE_MOBILE_TARGET "" CACHE STRING "Android or Apple device or simulator target")
set(_mobile_targets android ios tvos ios-simulator tvos-simulator)
set_property(CACHE SUNSHINE_MOBILE_TARGET PROPERTY STRINGS ${_mobile_targets})
if(NOT SUNSHINE_MOBILE_TARGET IN_LIST _mobile_targets)
    message(FATAL_ERROR "Set SUNSHINE_MOBILE_TARGET to one of: ${_mobile_targets}")
endif()

find_program(BASH_EXECUTABLE bash REQUIRED)

# These dependencies use upstream Configure, configure and Meson cross-file interfaces.
add_custom_target(sunshine_mobile_deps ALL
        COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build-client-deps.sh"
            "${SUNSHINE_MOBILE_TARGET}" "${CMAKE_BINARY_DIR}"
        COMMENT "Build dependencies for ${SUNSHINE_MOBILE_TARGET}"
        USES_TERMINAL
        VERBATIM)
