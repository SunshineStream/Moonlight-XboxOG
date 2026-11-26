cmake_minimum_required(VERSION 3.18)
project(MoonlightMobileDependencies NONE)

set(MOONLIGHT_MOBILE_TARGET "" CACHE STRING "Android or Apple device or simulator target")
set(_mobile_targets android ios tvos ios-simulator tvos-simulator)
set_property(CACHE MOONLIGHT_MOBILE_TARGET PROPERTY STRINGS ${_mobile_targets})
if(NOT MOONLIGHT_MOBILE_TARGET IN_LIST _mobile_targets)
    message(FATAL_ERROR "Set MOONLIGHT_MOBILE_TARGET to one of: ${_mobile_targets}")
endif()

find_program(BASH_EXECUTABLE bash REQUIRED)

# These dependencies use upstream Configure, configure and Meson cross-file interfaces.
add_custom_target(moonlight_mobile_deps ALL
        COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build-client-deps.sh"
            "${MOONLIGHT_MOBILE_TARGET}" "${CMAKE_BINARY_DIR}"
        COMMENT "Build dependencies for ${MOONLIGHT_MOBILE_TARGET}"
        USES_TERMINAL
        VERBATIM)
