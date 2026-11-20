# CPM Package Lock
# This file should be committed to version control.
# Renovate reads the metadata comments through the configured CMake regex managers.
# Keep tracked VERSION/TAG/COMMIT values immediately below their Renovate metadata.

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
# renovate: datasource=git-refs depName=Ryzee119/nxdk-sdl3 packageName=https://github.com/Ryzee119/nxdk-sdl3.git
# ref=master
set(NXDK_SDL3_COMMIT 62455b72ac80d318961c05c1230ffcb471e9f2d4)
CPMDeclarePackage(nxdk_sdl3
        NAME nxdk_sdl3
        GIT_REPOSITORY https://github.com/Ryzee119/nxdk-sdl3.git
        GIT_TAG ${NXDK_SDL3_COMMIT}
        DOWNLOAD_ONLY YES
)

# SDL3_image uses its built-in image decoders on Xbox.
# renovate: datasource=github-tags depName=libsdl-org/SDL_image versioning=semver
# extractVersion=^release-(?<version>\d+\.\d+\.\d+)$
set(SDL3_IMAGE_VERSION 3.2.6)
CPMDeclarePackage(sdl3_image
        NAME sdl3_image
        VERSION ${SDL3_IMAGE_VERSION}
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
        GIT_TAG release-${SDL3_IMAGE_VERSION}
        # Leave the unused vendored codecs uninitialized.
        GIT_CONFIG "submodule.active=:(exclude)external"
        DOWNLOAD_ONLY YES
)

# SDL3_ttf uses its vendored FreeType without desktop text-shaping dependencies.
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
