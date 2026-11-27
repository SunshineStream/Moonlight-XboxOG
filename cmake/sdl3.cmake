# Build SDL3 and the shared image and font libraries for each toolchain.
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

# Build the pinned SDL libraries with the portable client's platform backends.
function(moonlight_prepare_native_sdl3)
    # CMake and SDL define these option names.
    # cmake-lint: disable=C0103
    # Android's Java glue loads SDL3 by name. Native desktop and Apple targets link it statically.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if(ANDROID)
        set(SDL_SHARED ON)
        set(SDL_STATIC OFF)
    else()
        set(SDL_SHARED OFF)
        set(SDL_STATIC ON)
    endif()
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_INSTALL OFF)
    CPMGetPackage(SDL3)
    add_subdirectory("${SDL3_SOURCE_DIR}" "${SDL3_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # Use the same image formats and font source as the original Xbox.
    set(BUILD_SHARED_LIBS OFF)
    set(SDLIMAGE_VENDORED OFF)
    set(SDLIMAGE_BACKEND_STB ON)
    set(SDLIMAGE_PNG_LIBPNG OFF)
    set(SDLIMAGE_AVIF OFF)
    set(SDLIMAGE_JXL OFF)
    set(SDLIMAGE_TIF OFF)
    set(SDLIMAGE_WEBP OFF)
    set(SDLIMAGE_JPG_SAVE OFF)
    set(SDLIMAGE_PNG_SAVE OFF)
    set(SDLIMAGE_INSTALL OFF)
    CPMGetPackage(sdl3_image)
    add_subdirectory("${sdl3_image_SOURCE_DIR}" "${sdl3_image_BINARY_DIR}" EXCLUDE_FROM_ALL)

    set(SDLTTF_VENDORED ON)
    set(SDLTTF_HARFBUZZ OFF)
    set(SDLTTF_PLUTOSVG OFF)
    set(SDLTTF_INSTALL OFF)
    CPMGetPackage(sdl3_ttf)
    add_subdirectory("${sdl3_ttf_SOURCE_DIR}" "${sdl3_ttf_BINARY_DIR}" EXCLUDE_FROM_ALL)
endfunction()
