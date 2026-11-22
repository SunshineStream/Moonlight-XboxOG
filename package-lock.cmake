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
function(_moonlight_patched_source_dir out_var name revision patch)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    file(SHA256 "${patch}" patch_hash)
    string(SUBSTRING "${revision}" 0 12 revision_id)
    string(SUBSTRING "${patch_hash}" 0 12 patch_id)
    set(${out_var} "${FETCHCONTENT_BASE_DIR}/${name}-${revision_id}-${patch_id}-src" PARENT_SCOPE)
endfunction()

# FFmpeg, cross-compiled with its upstream configure script.
# renovate: datasource=github-tags depName=FFmpeg/FFmpeg versioning=semver-coerced
# extractVersion=^n(?<version>\d+\.\d+(?:\.\d+)?)$
set(FFMPEG_VERSION 9.1)
set(FFMPEG_NXDK_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/ffmpeg/ffmpeg-nxdk.patch")
_moonlight_patched_source_dir(FFMPEG_XBOX_SOURCE_DIR ffmpeg "${FFMPEG_VERSION}" "${FFMPEG_NXDK_PATCH}")
CPMDeclarePackage(ffmpeg
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
        DOWNLOAD_ONLY YES
)

# Lucide SVG icons
# renovate: datasource=github-tags depName=lucide-icons/lucide
set(LUCIDE_VERSION 1.77.0)
CPMDeclarePackage(lucide
        NAME lucide
        VERSION ${LUCIDE_VERSION}
        GIT_REPOSITORY https://github.com/lucide-icons/lucide.git
        GIT_TAG ${LUCIDE_VERSION}
        DOWNLOAD_ONLY YES
)

# Xbox SDL3 port, with its pinned SDL and xgu submodules.
# renovate: datasource=git-refs depName=Ryzee119/nxdk-sdl3
# ref=master
set(NXDK_SDL3_COMMIT 62455b72ac80d318961c05c1230ffcb471e9f2d4)
set(NXDK_SDL3_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/nxdk-sdl3.patch")
_moonlight_patched_source_dir(NXDK_SDL3_SOURCE_DIR nxdk_sdl3 "${NXDK_SDL3_COMMIT}" "${NXDK_SDL3_PATCH}")
CPMDeclarePackage(nxdk_sdl3
        NAME nxdk_sdl3
        GIT_REPOSITORY https://github.com/Ryzee119/nxdk-sdl3.git
        GIT_TAG ${NXDK_SDL3_COMMIT}
        SOURCE_DIR "${NXDK_SDL3_SOURCE_DIR}"
        GIT_CONFIG core.autocrlf=false
        UPDATE_DISCONNECTED YES
        PATCHES "${NXDK_SDL3_PATCH}"
        NO_CACHE YES
        DOWNLOAD_ONLY YES
)

# OpenSSL
# renovate: datasource=github-tags depName=openssl/openssl
# versioning=regex:^openssl-(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)$
set(OPENSSL_TAG openssl-4.1.0)
string(REGEX REPLACE "^openssl-" "" OPENSSL_VERSION "${OPENSSL_TAG}")
set(OPENSSL_XBOX_SOURCE_OPTIONS)
if(MOONLIGHT_BUILD_KIND STREQUAL "XBOX")
    set(OPENSSL_NXDK_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/openssl-nxdk.patch")
    _moonlight_patched_source_dir(OPENSSL_XBOX_SOURCE_DIR openssl "${OPENSSL_VERSION}" "${OPENSSL_NXDK_PATCH}")
    set(OPENSSL_XBOX_SOURCE_OPTIONS
            SOURCE_DIR "${OPENSSL_XBOX_SOURCE_DIR}"
            PATCHES "${OPENSSL_NXDK_PATCH}"
            NO_CACHE YES)
endif()
CPMDeclarePackage(openssl
        NAME openssl
        TLS_VERIFY TRUE
        # The source snapshot omits the upstream test submodules.
        URL https://github.com/openssl/openssl/archive/refs/tags/${OPENSSL_TAG}.tar.gz
        ${OPENSSL_XBOX_SOURCE_OPTIONS}
        DOWNLOAD_ONLY YES
)

# SDL3_image
# renovate: datasource=github-tags depName=libsdl-org/SDL_image versioning=semver
# extractVersion=^release-(?<version>\d+\.\d+\.\d+)$
set(SDL3_IMAGE_VERSION 3.2.6)
CPMDeclarePackage(sdl3_image
        NAME sdl3_image
        VERSION ${SDL3_IMAGE_VERSION}
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
        GIT_TAG release-${SDL3_IMAGE_VERSION}
        GIT_CONFIG "submodule.active=:(exclude)external"  # Leave the unused vendored codecs uninitialized.
        DOWNLOAD_ONLY YES
)

# SDL3_ttf
# renovate: datasource=github-tags depName=libsdl-org/SDL_ttf versioning=semver
# extractVersion=^release-(?<version>\d+\.\d+\.\d+)$
set(SDL3_TTF_VERSION 3.2.2)
CPMDeclarePackage(sdl3_ttf
        NAME sdl3_ttf
        VERSION ${SDL3_TTF_VERSION}
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
        GIT_TAG release-${SDL3_TTF_VERSION}
        GIT_SUBMODULES external/freetype
        DOWNLOAD_ONLY YES
)

# toml++
# renovate: datasource=git-refs depName=marzer/tomlplusplus
# ref=master
set(TOMLPLUSPLUS_COMMIT c38c9b922a68fc9772b25d7f606133d3ec4decfd)
CPMDeclarePackage(tomlplusplus
        NAME tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG ${TOMLPLUSPLUS_COMMIT}
        # nxdk's compiler wrappers do not report CMake compile features.
        DOWNLOAD_ONLY YES
)

# moonlight-common-c
# renovate: datasource=git-refs depName=moonlight-stream/moonlight-common-c
# ref=master
set(MOONLIGHT_COMMON_C_COMMIT 94c65fd364d17b223f00a8763c9a5d94a66cb8dd)
CPMDeclarePackage(moonlight-common-c
        NAME moonlight-common-c
        GIT_REPOSITORY https://github.com/moonlight-stream/moonlight-common-c.git
        GIT_TAG ${MOONLIGHT_COMMON_C_COMMIT}
        GIT_SUBMODULES enet nanors
        EXCLUDE_FROM_ALL YES
        OPTIONS "BUILD_SHARED_LIBS OFF"
)
