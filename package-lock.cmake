# CPM Package Lock
# This file should be committed to version control

# The first argument of CPMDeclarePackage can be freely chosen and is used as argument in CPMGetPackage.
# The NAME argument should be package name that would also be used in a find_package call.
# Ideally, both are the same, which might not always be possible: https://github.com/cpm-cmake/CPM.cmake/issues/603
# This is needed to support CPM_USE_LOCAL_PACKAGES

# Renovate-bot will update the versions and hashes in this file when a new version of a dependency is released.
# The comments above each dependency are used by renovate to identify the dependencies and extract the version numbers.
# See https://github.com/LizardByte/.github/blob/master/renovate/cmake.json5 for the configuration of renovate.
#
# Expected dependency structure for new entries:
# - Start each block with the dependency name, for example `# Example dependency`.
# - Follow it with consecutive renovate metadata comments.
# - The first metadata line must start with `# renovate:` and include `datasource=` and `depName=`.
# - Optional metadata keys are `versioning=`, `extractVersion=`, and `registryUrl=`.
# - Optional metadata may stay on the `# renovate:` line or continue on the next consecutive `#` lines.
# - Keep metadata keys in this order: `datasource`, `depName`, `versioning`, `extractVersion`, `registryUrl`.
# - After metadata, declare the tracked value with `set(NAME_VERSION ...)` or `set(NAME_TAG ...)`.
# - For `git-refs`, add a `# ref=<branch>` line and `set(NAME_COMMIT ...)`. Renovate reads the URL from
#   the literal `GIT_REPOSITORY` in the following `CPMDeclarePackage(...)`.
# - If the dependency also tracks a SHA256, keep `set(NAME_SHA256 ...)` immediately after the
#   matching `NAME_VERSION` or `NAME_TAG` line with no unrelated lines between them.
# - Keep `CPMDeclarePackage(...)` below the tracked values.
#
# Example layout:
# - `# Example dependency`
# - `# renovate: datasource=github-tags depName=owner/repo`
# - `# versioning=regex:^v(?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)$`
# - `set(EXAMPLE_TAG v1.2.3)`
# - `set(EXAMPLE_SHA256 <sha256>)`
# - `CPMDeclarePackage(...)`

# FFmpeg, cross-compiled with its upstream configure script.
# renovate: datasource=github-tags depName=FFmpeg/FFmpeg versioning=semver-coerced
# extractVersion=^n(?<version>\d+\.\d+(?:\.\d+)?)$
set(FFMPEG_VERSION 9.0.2)
set(FFMPEG_NXDK_PATCH "${CMAKE_CURRENT_LIST_DIR}/src/_nxdk_compat/ffmpeg/ffmpeg-nxdk.patch")
CPMDeclarePackage(ffmpeg
        NAME ffmpeg
        VERSION ${FFMPEG_VERSION}
        GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
        GIT_TAG n${FFMPEG_VERSION}
        # Preserve LF endings in the upstream shell scripts on Windows.
        GIT_CONFIG core.autocrlf=false
        UPDATE_DISCONNECTED YES
        PATCHES "${FFMPEG_NXDK_PATCH}"
        NO_CACHE YES
        DOWNLOAD_ONLY YES
)

# Lucide SVG icons
# renovate: datasource=github-tags depName=lucide-icons/lucide
set(LUCIDE_VERSION 1.76.0)
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
CPMDeclarePackage(nxdk_sdl3
        NAME nxdk_sdl3
        GIT_REPOSITORY https://github.com/Ryzee119/nxdk-sdl3.git
        GIT_TAG ${NXDK_SDL3_COMMIT}
        GIT_CONFIG core.autocrlf=false
        UPDATE_DISCONNECTED YES
        PATCHES "${NXDK_SDL3_PATCH}"
        NO_CACHE YES
        DOWNLOAD_ONLY YES
)

# OpenSSL
# renovate: datasource=github-tags depName=openssl/openssl
# versioning=regex:^(?:OpenSSL_|openssl-)(?<major>\d+)[._](?<minor>\d+)[._](?<patch>\d+)[a-z]*$
set(OPENSSL_TAG OpenSSL_1_1_1w)
CPMDeclarePackage(openssl
        NAME openssl
        TLS_VERIFY TRUE
        # The source snapshot omits the upstream test submodules.
        URL https://github.com/openssl/openssl/archive/refs/tags/${OPENSSL_TAG}.tar.gz
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
