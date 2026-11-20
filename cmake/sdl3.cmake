# Build the Xbox SDL3 port and its image and font libraries in the Xbox child.
include_guard(GLOBAL)

# Configure the pinned port with Moonlight's Xbox driver adapters.
function(moonlight_prepare_sdl3)
    CPMGetPackage(nxdk_sdl3)
    add_subdirectory("${nxdk_sdl3_SOURCE_DIR}" "${nxdk_sdl3_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # These names belong to SDL3_image and SDL3_ttf.
    # cmake-lint: disable=C0103
    set(BUILD_SHARED_LIBS OFF)
    set(SDLIMAGE_VENDORED OFF)
    set(SDLIMAGE_BACKEND_STB ON)
    set(SDLIMAGE_AVIF OFF)
    set(SDLIMAGE_JXL OFF)
    set(SDLIMAGE_TIF OFF)
    set(SDLIMAGE_WEBP OFF)
    set(SDLIMAGE_JPG_SAVE OFF)
    set(SDLIMAGE_PNG_SAVE OFF)
    CPMGetPackage(sdl3_image)
    add_subdirectory("${sdl3_image_SOURCE_DIR}" "${sdl3_image_BINARY_DIR}" EXCLUDE_FROM_ALL)

    set(SDLTTF_VENDORED ON)
    set(SDLTTF_HARFBUZZ OFF)
    set(SDLTTF_PLUTOSVG OFF)
    CPMGetPackage(sdl3_ttf)
    add_subdirectory("${sdl3_ttf_SOURCE_DIR}" "${sdl3_ttf_BINARY_DIR}" EXCLUDE_FROM_ALL)
endfunction()
