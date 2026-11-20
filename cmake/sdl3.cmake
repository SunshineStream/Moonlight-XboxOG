# Build the Xbox SDL3 port and its image and font libraries in the Xbox child.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/patch.cmake")

# Configure the pinned port with Moonlight's Xbox driver adapters.
function(moonlight_prepare_sdl3)
    CPMGetPackage(nxdk_sdl3)
    add_subdirectory("${nxdk_sdl3_SOURCE_DIR}" "${nxdk_sdl3_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # Keep Moonlight's USB and framebuffer hooks in build-directory copies.
    set(adapted_glue "${CMAKE_CURRENT_BINARY_DIR}/sdl3-compat")
    set(glue_sources
            nxdk_glue/audio/SDL_xboxaudio.c
            nxdk_glue/joystick/SDL_xboxjoystick.c
            nxdk_glue/render/SDL_render_xgu.c
            nxdk_glue/thread/SDL_systhread.c
            nxdk_glue/video/SDL_xboxvideo.c)
    moonlight_patch_sources("${nxdk_sdl3_SOURCE_DIR}" "${adapted_glue}"
            "${CMAKE_SOURCE_DIR}/src/_nxdk_compat/nxdk-sdl3.patch" ${glue_sources})
    get_target_property(sdl_sources SDL3-static SOURCES)
    foreach(glue_source IN LISTS glue_sources)
        list(REMOVE_ITEM sdl_sources "${nxdk_sdl3_SOURCE_DIR}/${glue_source}")
        list(APPEND sdl_sources "${adapted_glue}/${glue_source}")
        get_filename_component(glue_dir "${nxdk_sdl3_SOURCE_DIR}/${glue_source}" DIRECTORY)
        target_include_directories(SDL3-static PRIVATE "${glue_dir}")
    endforeach()
    set_property(TARGET SDL3-static PROPERTY SOURCES "${sdl_sources}")

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
