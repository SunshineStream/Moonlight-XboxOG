# CPM Package Lock
# This file should be committed to version control

# The first argument of CPMDeclarePackage can be freely chosen and is used as argument in CPMGetPackage.
# The NAME argument should be package name that would also be used in a find_package call.
# Ideally, both are the same, which might not always be possible: https://github.com/cpm-cmake/CPM.cmake/issues/603
# This is needed to support CPM_USE_LOCAL_PACKAGES

# Renovate-bot will update the versions and hashes in this file when a new version of a dependency is released.
# The comments above each dependency are used by renovate to identify the dependencies and extract the version numbers.
# See https://github.com/LizardByte/.github/blob/master/renovate/cmake.json5 for the configuration of renovate.
# See https://github.com/LizardByte/.github/blob/master/renovate/README.md#cpm-package-lock for annotation instructions.

# Use a fresh checkout when the upstream revision or a local patch changes.
function(_sunshine_patched_source_dir out_var name revision patch)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    file(SHA256 "${patch}" patch_hash)
    string(SUBSTRING "${revision}" 0 12 revision_id)
    string(SUBSTRING "${patch_hash}" 0 12 patch_id)
    set(${out_var} "${FETCHCONTENT_BASE_DIR}/${name}-${revision_id}-${patch_id}-src" PARENT_SCOPE)
endfunction()

# Desktop FFmpeg SDK packages.
# renovate: datasource=github-releases depName=LizardByte/build-deps versioning=semver
set(SUNSHINE_BUILD_DEPS_TAG v2026.1017.131201)

# FFmpeg, cross-compiled with its upstream configure script.
# renovate: datasource=github-tags depName=FFmpeg/FFmpeg versioning=semver-coerced
# extractVersion=^n(?<version>\d+\.\d+(?:\.\d+)?)$
set(FFMPEG_VERSION 9.1)
set(FFMPEG_NXDK_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/ffmpeg/ffmpeg-nxdk.patch")
_sunshine_patched_source_dir(FFMPEG_XBOX_SOURCE_DIR ffmpeg "${FFMPEG_VERSION}" "${FFMPEG_NXDK_PATCH}")
sunshine_declare_source_package(ffmpeg
        NAME ffmpeg
        VERSION ${FFMPEG_VERSION}
        GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
        GIT_TAG n${FFMPEG_VERSION}
        GIT_SHALLOW TRUE
        SOURCE_DIR "${FFMPEG_XBOX_SOURCE_DIR}"
        # Preserve LF endings in the upstream shell scripts on Windows.
        GIT_CONFIG core.autocrlf=false
        UPDATE_DISCONNECTED YES
        PATCHES "${FFMPEG_NXDK_PATCH}"
        NO_CACHE YES
)

# Software AV1 decoding for mobile and TV SDK builds.
# renovate: datasource=github-tags depName=videolan/dav1d
set(DAV1D_CLIENT_VERSION 1.5.5)

# Lucide SVG icons
# renovate: datasource=github-tags depName=lucide-icons/lucide
set(LUCIDE_VERSION 1.82.0)
sunshine_declare_source_package(lucide
        NAME lucide
        VERSION ${LUCIDE_VERSION}
        GIT_REPOSITORY https://github.com/lucide-icons/lucide.git
        GIT_TAG ${LUCIDE_VERSION}
)

# Xbox SDL3 port, with its pinned SDL and xgu submodules.
# renovate: datasource=git-refs depName=Ryzee119/nxdk-sdl3
# ref=master
set(NXDK_SDL3_COMMIT 62455b72ac80d318961c05c1230ffcb471e9f2d4)
set(NXDK_SDL3_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/nxdk-sdl3.patch")
_sunshine_patched_source_dir(NXDK_SDL3_SOURCE_DIR nxdk_sdl3 "${NXDK_SDL3_COMMIT}" "${NXDK_SDL3_PATCH}")
sunshine_declare_source_package(nxdk_sdl3
        NAME nxdk_sdl3
        GIT_REPOSITORY https://github.com/Ryzee119/nxdk-sdl3.git
        GIT_TAG ${NXDK_SDL3_COMMIT}
        SOURCE_DIR "${NXDK_SDL3_SOURCE_DIR}"
        GIT_CONFIG core.autocrlf=false
        UPDATE_DISCONNECTED YES
        PATCHES "${NXDK_SDL3_PATCH}"
        NO_CACHE YES
)

# OpenSSL
# renovate: datasource=github-tags depName=openssl/openssl
# versioning=regex:^openssl-(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)$
set(OPENSSL_TAG openssl-4.1.0)
string(REGEX REPLACE "^openssl-" "" OPENSSL_VERSION "${OPENSSL_TAG}")
set(OPENSSL_XBOX_SOURCE_OPTIONS)
if(SUNSHINE_BUILD_KIND STREQUAL "XBOX")
    set(OPENSSL_NXDK_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/openssl-nxdk.patch")
    _sunshine_patched_source_dir(OPENSSL_XBOX_SOURCE_DIR openssl "${OPENSSL_VERSION}" "${OPENSSL_NXDK_PATCH}")
    set(OPENSSL_XBOX_SOURCE_OPTIONS
            SOURCE_DIR "${OPENSSL_XBOX_SOURCE_DIR}"
            PATCHES "${OPENSSL_NXDK_PATCH}"
            NO_CACHE YES)
endif()
sunshine_declare_source_package(openssl
        NAME openssl
        TLS_VERIFY TRUE
        # The source snapshot omits the upstream test submodules.
        URL https://github.com/openssl/openssl/archive/refs/tags/${OPENSSL_TAG}.tar.gz
        ${OPENSSL_XBOX_SOURCE_OPTIONS}
)

# SDL3_image
# renovate: datasource=github-tags depName=libsdl-org/SDL_image versioning=semver
# extractVersion=^release-(?<version>\d+\.\d+\.\d+)$
set(SDL3_IMAGE_VERSION 3.4.8)
sunshine_declare_source_package(sdl3_image
        NAME sdl3_image
        VERSION ${SDL3_IMAGE_VERSION}
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
        GIT_TAG release-${SDL3_IMAGE_VERSION}
        GIT_CONFIG "submodule.active=:(exclude)external"  # Leave the unused vendored codecs uninitialized.
)

# SDL3 for desktop and mobile clients
# renovate: datasource=github-tags depName=libsdl-org/SDL versioning=semver
# extractVersion=^release-(?<version>\d+\.\d+\.\d+)$
set(SDL3_VERSION 3.4.20)
set(SDL3_STEAM_CONTROLLER_PATCH
        "${CMAKE_CURRENT_LIST_DIR}/cmake/patches/sdl3-steam-controller-trigger-clicks-and-haptics.patch")
_sunshine_patched_source_dir(SDL3_PATCHED_SOURCE_DIR sdl3 "${SDL3_VERSION}" "${SDL3_STEAM_CONTROLLER_PATCH}")
sunshine_declare_source_package(SDL3
        NAME SDL3
        VERSION ${SDL3_VERSION}
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-${SDL3_VERSION}
        GIT_SHALLOW TRUE
        SOURCE_DIR "${SDL3_PATCHED_SOURCE_DIR}"
        GIT_CONFIG core.autocrlf=false
        PATCHES "${SDL3_STEAM_CONTROLLER_PATCH}"
        NO_CACHE YES
        UPDATE_DISCONNECTED YES
)

# SDL3_ttf
# renovate: datasource=github-tags depName=libsdl-org/SDL_ttf versioning=semver
# extractVersion=^release-(?<version>\d+\.\d+\.\d+)$
set(SDL3_TTF_VERSION 3.2.2)
sunshine_declare_source_package(sdl3_ttf
        NAME sdl3_ttf
        VERSION ${SDL3_TTF_VERSION}
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
        GIT_TAG release-${SDL3_TTF_VERSION}
        GIT_SUBMODULES external/freetype
)

# toml++
# renovate: datasource=github-tags depName=marzer/tomlplusplus versioning=semver
# extractVersion=^v(?<version>\d+\.\d+\.\d+)$
set(TOMLPLUSPLUS_VERSION 3.5.1)
sunshine_declare_source_package(tomlplusplus
        NAME tomlplusplus
        VERSION ${TOMLPLUSPLUS_VERSION}
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v${TOMLPLUSPLUS_VERSION}
        # nxdk's compiler wrappers do not report CMake compile features.
)

# moonlight-common-c
# renovate: datasource=git-refs depName=moonlight-stream/moonlight-common-c
# ref=master
set(MOONLIGHT_COMMON_C_COMMIT b618ef257573b49d779ed8bf47ff1f46deca7154)
CPMDeclarePackage(moonlight-common-c
        NAME moonlight-common-c
        GIT_REPOSITORY https://github.com/moonlight-stream/moonlight-common-c.git
        GIT_TAG ${MOONLIGHT_COMMON_C_COMMIT}
        GIT_SUBMODULES enet nanors
        EXCLUDE_FROM_ALL YES
        OPTIONS "BUILD_SHARED_LIBS OFF"
)
