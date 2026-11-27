# Native clients can use the shared build-deps archives or target SDK packages.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/build-deps.cmake")

set(FFMPEG_PREPARED_BINARIES "" CACHE PATH "FFmpeg installation prefix for the target architecture")
option(SUNSHINE_USE_PREBUILT_FFMPEG "Download the pinned desktop FFmpeg package from LizardByte/build-deps" OFF)

# Select and verify the desktop media archive for the configured target.
function(sunshine_fetch_client_ffmpeg output_prefix)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(arch "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(arch STREQUAL "amd64")
        set(arch "x86_64")
    elseif(arch STREQUAL "arm64")
        set(arch "aarch64")
    endif()

    set(platform "${CMAKE_SYSTEM_NAME}-${arch}")
    if(WIN32 AND arch STREQUAL "x86_64")
        set(platform Windows-AMD64)
    elseif(WIN32 AND arch STREQUAL "aarch64")
        set(platform Windows-ARM64)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND arch STREQUAL "aarch64")
        set(platform Darwin-arm64)
    endif()
    set(root "${CMAKE_BINARY_DIR}/_deps/build-deps-${SUNSHINE_BUILD_DEPS_TAG}/${platform}")
    set(ffmpeg_prefix "${root}/ffmpeg")
    if(NOT EXISTS "${root}/.verified" OR NOT EXISTS "${ffmpeg_prefix}/lib/pkgconfig/libavcodec.pc")
        file(MAKE_DIRECTORY "${root}")
        set(asset "${platform}-ffmpeg.tar.gz")
        sunshine_build_deps_asset("${asset}" url sha256)
        set(archive "${root}/${asset}")
        if(EXISTS "${archive}")
            file(SHA256 "${archive}" archive_hash)
        endif()
        if(NOT sha256 STREQUAL archive_hash)
            file(DOWNLOAD "${url}" "${archive}.part" EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON TIMEOUT 300)
            file(RENAME "${archive}.part" "${archive}")
        endif()
        execute_process(
                COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
                WORKING_DIRECTORY "${root}"
                RESULT_VARIABLE unpacked)
        if(NOT unpacked EQUAL 0)
            message(FATAL_ERROR "Could not extract ${archive}")
        endif()
        file(TOUCH "${root}/.verified")
    endif()
    set(${output_prefix} "${ffmpeg_prefix}" PARENT_SCOPE)
endfunction()

# Resolve relocatable static archives or the media packages installed in the target SDK.
function(sunshine_prepare_client_media)
    if(ANDROID AND CMAKE_HOST_WIN32)
        # Android Studio may run without MSYS2 on PATH.
        find_program(PKG_CONFIG_EXECUTABLE NAMES pkg-config pkgconf
                HINTS "$ENV{MSYS2_ROOT}/ucrt64/bin" "C:/msys64/ucrt64/bin")
    endif()
    find_package(PkgConfig REQUIRED)
    set(ffmpeg_prefix "${FFMPEG_PREPARED_BINARIES}")
    if(SUNSHINE_USE_PREBUILT_FFMPEG AND NOT ffmpeg_prefix)
        sunshine_fetch_client_ffmpeg(ffmpeg_prefix)
    endif()

    if(ANDROID AND ffmpeg_prefix AND EXISTS "${ffmpeg_prefix}/.sunshine-android-deps")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${ffmpeg_prefix}/.sunshine-android-deps")
    endif()

    if(ffmpeg_prefix)
        if(NOT EXISTS "${ffmpeg_prefix}/lib/pkgconfig/libavcodec.pc")
            message(FATAL_ERROR "FFMPEG_PREPARED_BINARIES must point to the installed target FFmpeg prefix.")
        endif()
        set(original_pkg_path "$ENV{PKG_CONFIG_PATH}")
        set(original_pkg_libdir "$ENV{PKG_CONFIG_LIBDIR}")
        set(original_pkg_sysroot "$ENV{PKG_CONFIG_SYSROOT_DIR}")
        # These names are part of pkg-config's interface.
        # cmake-lint: disable=C0103
        set(ENV{PKG_CONFIG_PATH} "${ffmpeg_prefix}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_LIBDIR} "$ENV{PKG_CONFIG_PATH}")
        unset(ENV{PKG_CONFIG_SYSROOT_DIR})
        set(PKG_CONFIG_ARGN --static)
        if(WIN32)
            # Match the archive's C++ runtime to the app's static MinGW runtime.
            set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
        endif()
    elseif(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Provide target FFmpeg and Opus with FFMPEG_PREPARED_BINARIES for cross-compilation.")
    endif()

    # FindPkgConfig caches absolute library paths even if the prefix changes.
    get_cmake_property(cache_variables CACHE_VARIABLES)
    foreach(variable IN LISTS cache_variables)
        if(variable MATCHES "^(__pkg_config_checked_|pkgcfg_lib_)SUNSHINE_CLIENT_FFMPEG")
            unset(${variable} CACHE)
        endif()
    endforeach()
    pkg_check_modules(SUNSHINE_CLIENT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL libavcodec libavutil libswscale)
    if(ffmpeg_prefix)
        # cmake-lint: disable=C0103
        set(ENV{PKG_CONFIG_PATH} "${original_pkg_path}")
        set(ENV{PKG_CONFIG_LIBDIR} "${original_pkg_libdir}")
        set(ENV{PKG_CONFIG_SYSROOT_DIR} "${original_pkg_sysroot}")
    endif()
endfunction()
