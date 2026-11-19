# Build the Xbox child project with the stock nxdk toolchain, then package the
# resulting executable into the XBE and ISO artifacts consumed by xemu and CI.

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sources.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nxdk.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/moonlight-dependencies.cmake")

#
# metadata
#
set(XBE_TITLE ${CMAKE_PROJECT_NAME})
set(XBOX_XBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/xbe")
set(XBOX_ISO_NAME "${CMAKE_PROJECT_NAME}.iso")
set(XBOX_ISO "${CMAKE_CURRENT_BINARY_DIR}/${XBOX_ISO_NAME}")

if(NOT EXISTS "${NXDK_DIR}/.moonlight-nxdk-copy")
    message(FATAL_ERROR "Use cmake/nxdk-toolchain.cmake to build with an isolated nxdk copy")
endif()
set(ENV{NXDK_DIR} "${NXDK_DIR}")
moonlight_prepare_nxdk("${NXDK_DIR}" "${NXDK_DIR}/.moonlight-bootstrap")

find_package(NXDK REQUIRED)
include("${CMAKE_SOURCE_DIR}/cmake/nxdk-compat.cmake")
moonlight_prepare_nxdk_compat()

find_package(NXDK_SDL2 REQUIRED)
find_package(NXDK_SDL2_Image REQUIRED)
find_package(NXDK_SDL2_TTF REQUIRED)

# add the automount_d_drive symbol to the linker flags, this is automatic with nxdk when using the Makefile option
# if this is not used, we must add some code to the main function to automount the D drive
# e.g. https://github.com/abaire/nxdk_pgraph_tests/blob/4b7934e6d612a6d17f9ec229a2d72601a5caefc4/src/main.cpp#L118-L122
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -include:_automount_d_drive")

file(MAKE_DIRECTORY "${XBOX_XBE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/lucide.cmake")

# ensure assets are copied to the build directory for packaging into ISO
add_custom_target(sync_xbe_assets ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/xbe"
            "${XBOX_XBE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${NXDK_DIR}/samples/sdl_ttf/vegur-regular.ttf"
            "${XBOX_XBE_DIR}/assets/fonts/vegur-regular.ttf"
        COMMAND "${CMAKE_COMMAND}"
            "-DLUCIDE_SOURCE_DIR=${MOONLIGHT_LUCIDE_SOURCE_DIR}"
            "-DLUCIDE_ASSET_DIR=${XBOX_XBE_DIR}/assets"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage-lucide.cmake"
        COMMENT "Sync XBE assets"
        VERBATIM
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")

MOONLIGHT_PREPARE_COMMON_DEPENDENCIES()

# Use nanors' scalar backend on the Xbox CPU, which lacks SSSE3 and newer SIMD
# instructions and cannot link the compiler runtime helpers used for x86 dispatch.
set_property(SOURCE
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/nanors/deps/obl/oblas_lite.c"
        DIRECTORY "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c"
        APPEND PROPERTY COMPILE_OPTIONS
        -include "${CMAKE_SOURCE_DIR}/src/_nxdk_compat/nanors_compat.h"
)

add_executable(${CMAKE_PROJECT_NAME}
        ${MOONLIGHT_SOURCES}
)
target_sources(${CMAKE_PROJECT_NAME}
        PRIVATE
        "${CMAKE_SOURCE_DIR}/src/_nxdk_compat/fenv_compat.cpp"
        $<TARGET_OBJECTS:moonlight_nxdk_net_init>
)
target_include_directories(${CMAKE_PROJECT_NAME}
        SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${MOONLIGHT_FFMPEG_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third-party/tomlplusplus/include"
        "${MOONLIGHT_NXDK_NET_INCLUDE_DIR}"
        "${MOONLIGHT_NXDK_LIBC_EXTENSIONS_DIR}"
        "${MOONLIGHT_NXDK_LWIP_POSIX_COMPAT_DIR}"
        "${NXDK_DIR}/lib/usb/libusbohci/inc"
        "${NXDK_DIR}/lib/usb/libusbohci_xbox"
)
target_link_libraries(${CMAKE_PROJECT_NAME}
        PUBLIC
        moonlight-common-c
        ${MOONLIGHT_FFMPEG_LIBRARIES}
        NXDK::NXDK
        NXDK::NXDK_CXX
        NXDK::Net
        moonlight_nxdk_compat
        NXDK::SDL2
        NXDK::SDL2_Image
        NXDK::SDL2_TTF
        OpenSSL::Crypto
        OpenSSL::SSL
)
target_compile_options(${CMAKE_PROJECT_NAME}
        PRIVATE
        ${MOONLIGHT_COMPILE_OPTIONS}
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
)
# NASM's freestanding i386 objects do not carry Windows SafeSEH metadata; the
# final Xbox image is packaged as XBE and does not use the Windows PE loader.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE /safeseh:no)
target_compile_definitions(${CMAKE_PROJECT_NAME}
        PRIVATE
        XBOX
        NXDK
        USBH_USE_EXTERNAL_CONFIG="usbh_config_xbox.h"
        TOML_EXCEPTIONS=0
        TOML_ENABLE_WINDOWS_COMPAT=0)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        MOONLIGHT_INPUT_DIAGNOSTICS=$<BOOL:${MOONLIGHT_INPUT_DIAGNOSTICS}>)
add_dependencies(${CMAKE_PROJECT_NAME} moonlight-common-c)

if(BUILD_DOCS)
    include(third-party/dockle/cmake/Dockle.cmake)
    dockle_add_docs(docs TARGETS api)
endif()

# convert the built EXE into the XBE format
add_custom_target(cxbe_convert ALL
        COMMAND "${NXDK_DIR}/tools/cxbe/cxbe"
            -OUT:${XBOX_XBE_DIR}/default.xbe
            -TITLE:${XBE_TITLE}
            "${CMAKE_CURRENT_BINARY_DIR}/${XBE_TITLE}.exe"
        COMMENT "CXBE conversion: EXE -> XBE"
        VERBATIM
)
add_dependencies(cxbe_convert ${CMAKE_PROJECT_NAME})
add_dependencies(cxbe_convert sync_xbe_assets)

# convert the XBE into an ISO
add_custom_target(xbe_iso ALL
        COMMAND "${NXDK_DIR}/tools/extract-xiso/build/extract-xiso"
            -c "${XBOX_XBE_DIR}" "${XBOX_ISO_NAME}"
        COMMENT "CXBE conversion: XBE -> XISO"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        VERBATIM
)
add_dependencies(xbe_iso cxbe_convert)

if(MOONLIGHT_BUILD_AUDIO_EXPERIMENTS)
    add_executable(audio_playback_probe
            "${CMAKE_SOURCE_DIR}/experiments/audio_playback_probe.cpp"
            "${CMAKE_SOURCE_DIR}/src/logging/logger.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/audio_configuration.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/audio_player.cpp"
            "${CMAKE_SOURCE_DIR}/src/_nxdk_compat/fenv_compat.cpp")
    target_include_directories(audio_playback_probe PRIVATE
            "${CMAKE_SOURCE_DIR}"
            "${MOONLIGHT_FFMPEG_INCLUDE_DIR}")
    target_compile_definitions(audio_playback_probe PRIVATE NXDK XBOX)
    target_compile_options(audio_playback_probe PRIVATE -std=gnu++17)
    target_link_options(audio_playback_probe PRIVATE /safeseh:no)
    target_link_libraries(audio_playback_probe PRIVATE
            ${MOONLIGHT_FFMPEG_LIBRARIES}
            NXDK::NXDK NXDK::NXDK_CXX NXDK::SDL2 moonlight_nxdk_compat)
    set(_audio_playback_probe_xbe_dir "${CMAKE_CURRENT_BINARY_DIR}/audio-playback-probe")
    file(MAKE_DIRECTORY "${_audio_playback_probe_xbe_dir}")
    add_custom_target(audio_playback_probe_iso ALL
            COMMAND "${NXDK_DIR}/tools/cxbe/cxbe"
                "-OUT:${_audio_playback_probe_xbe_dir}/default.xbe"
                "-TITLE:Opus audio playback probe"
                "$<TARGET_FILE:audio_playback_probe>"
            COMMAND "${NXDK_DIR}/tools/extract-xiso/build/extract-xiso"
                -c "${_audio_playback_probe_xbe_dir}" "AudioPlaybackProbe.iso"
            DEPENDS audio_playback_probe
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Package the standalone Opus playback probe"
            VERBATIM)
endif()

if(MOONLIGHT_BUILD_VIDEO_EXPERIMENTS)
    add_executable(pvideo_presentation_probe
            "${CMAKE_SOURCE_DIR}/experiments/pvideo_presentation_probe.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/stats_overlay.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/video_acceleration.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/pvideo_presenter.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/video_overlay.cpp")
    target_include_directories(pvideo_presentation_probe PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(pvideo_presentation_probe PRIVATE NXDK XBOX)
    target_compile_options(pvideo_presentation_probe PRIVATE -std=gnu++17)
    target_link_libraries(pvideo_presentation_probe PRIVATE NXDK::NXDK NXDK::NXDK_CXX)
    set(_pvideo_probe_xbe_dir "${CMAKE_CURRENT_BINARY_DIR}/pvideo-presentation-probe")
    file(MAKE_DIRECTORY "${_pvideo_probe_xbe_dir}")
    add_custom_target(pvideo_presentation_probe_iso ALL
            COMMAND "${NXDK_DIR}/tools/cxbe/cxbe"
                "-OUT:${_pvideo_probe_xbe_dir}/default.xbe"
                "-TITLE:PVIDEO presentation probe"
                "$<TARGET_FILE:pvideo_presentation_probe>"
            COMMAND "${NXDK_DIR}/tools/extract-xiso/build/extract-xiso"
                -c "${_pvideo_probe_xbe_dir}" "PvideoPresentationProbe.iso"
            DEPENDS pvideo_presentation_probe
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Package the isolated overlay presentation and teardown probe"
            VERBATIM)

    add_executable(mpeg2_motion_probe
            "${CMAKE_SOURCE_DIR}/experiments/mpeg2_motion_probe.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/mpeg2_prediction.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/video_acceleration.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/pvideo_presenter.cpp"
            "${CMAKE_SOURCE_DIR}/src/streaming/video_overlay.cpp")
    target_include_directories(mpeg2_motion_probe PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(mpeg2_motion_probe PRIVATE NXDK XBOX)
    target_compile_options(mpeg2_motion_probe PRIVATE -std=gnu++17)
    target_link_libraries(mpeg2_motion_probe PRIVATE NXDK::NXDK NXDK::NXDK_CXX)
    set(_mpeg2_probe_xbe_dir "${CMAKE_CURRENT_BINARY_DIR}/mpeg2-motion-probe")
    file(MAKE_DIRECTORY "${_mpeg2_probe_xbe_dir}")
    add_custom_target(mpeg2_motion_probe_iso ALL
            COMMAND "${NXDK_DIR}/tools/cxbe/cxbe"
                "-OUT:${_mpeg2_probe_xbe_dir}/default.xbe"
                "-TITLE:MPEG-2 motion probe"
                "$<TARGET_FILE:mpeg2_motion_probe>"
            COMMAND "${NXDK_DIR}/tools/extract-xiso/build/extract-xiso"
                -c "${_mpeg2_probe_xbe_dir}" "Mpeg2MotionProbe.iso"
            DEPENDS mpeg2_motion_probe
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Package the isolated MPEG-2 motion prediction experiment"
            VERBATIM)
endif()
