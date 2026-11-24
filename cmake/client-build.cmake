include("${CMAKE_CURRENT_LIST_DIR}/client-media.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/sdl3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/lucide.cmake")

if(WIN32 AND MOONLIGHT_HOST_TOOL_DIR)
    enable_language(RC)
    if(CMAKE_RC_COMPILER MATCHES "windres[.]exe$")
        # windres runs a separate preprocessor, and CLion's build may lack MSYS2 on PATH.
        string(PREPEND CMAKE_RC_FLAGS "--preprocessor=\"${CMAKE_C_COMPILER}\" "
                "--preprocessor-arg=-E --preprocessor-arg=-xc-header --preprocessor-arg=-DRC_INVOKED ")
    endif()
endif()

moonlight_prepare_portable_sdl3()
moonlight_prepare_client_media()

set(_client_sources ${MOONLIGHT_SOURCES})
list(REMOVE_ITEM _client_sources
        "${CMAKE_SOURCE_DIR}/src/platform/usb_input.cpp"
        "${CMAKE_SOURCE_DIR}/src/startup/memory_stats.cpp")

if(ANDROID)
    # SDLActivity loads libmain.so through JNI.
    add_library(moonlight_client SHARED ${_client_sources})
    set_target_properties(moonlight_client PROPERTIES OUTPUT_NAME main)
elseif(APPLE)
    add_executable(moonlight_client MACOSX_BUNDLE ${_client_sources})
    set_target_properties(moonlight_client PROPERTIES OUTPUT_NAME Moonlight
            MACOSX_BUNDLE_GUI_IDENTIFIER dev.lizardbyte.app.Moonlight
            MACOSX_BUNDLE_BUNDLE_NAME Moonlight
            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/apple/Info.plist.in")
elseif(WIN32)
    add_executable(moonlight_client WIN32 ${_client_sources})
    set_target_properties(moonlight_client PROPERTIES OUTPUT_NAME Moonlight)
else()
    add_executable(moonlight_client ${_client_sources})
    set_target_properties(moonlight_client PROPERTIES OUTPUT_NAME moonlight)
endif()

target_sources(moonlight_client PRIVATE "${CMAKE_SOURCE_DIR}/src/_nxdk_compat/ffmpeg_memory.c")
target_compile_definitions(moonlight_client PRIVATE MOONLIGHT_PORTABLE TOML_EXCEPTIONS=0)
target_include_directories(moonlight_client PRIVATE "${CMAKE_SOURCE_DIR}" "${tomlplusplus_SOURCE_DIR}/include")
target_link_libraries(moonlight_client PRIVATE
        Moonlight::OpenSSL moonlight-common-c PkgConfig::MOONLIGHT_CLIENT_FFMPEG
        SDL3::SDL3 SDL3_ttf::SDL3_ttf-static SDL3_image::SDL3_image-static)
if(APPLE AND NOT CMAKE_CROSSCOMPILING)
    # Use the headers matching the bundled libraries before Homebrew's include directories.
    set_property(TARGET moonlight_client PROPERTY NO_SYSTEM_FROM_IMPORTED TRUE)
    target_include_directories(moonlight_client BEFORE PRIVATE "${OPENSSL_INCLUDE_DIR}")
endif()
if(ANDROID)
    # FFmpeg's AArch64 assembly addresses private tables directly. Keep static
    # library symbols local to libmain.so so those references cannot be interposed.
    target_link_options(moonlight_client PRIVATE "LINKER:--exclude-libs,ALL")
elseif(WIN32)
    target_link_libraries(moonlight_client PRIVATE ws2_32)
    target_link_options(moonlight_client PRIVATE -static-libgcc -static-libstdc++)
endif()

set(MOONLIGHT_CLIENT_ASSET_ROOT "${CMAKE_CURRENT_BINARY_DIR}/client-assets/assets"
        CACHE PATH "Output directory for staged client assets")
set(_client_assets "${MOONLIGHT_CLIENT_ASSET_ROOT}")
add_custom_target(stage_client_assets
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/xbe/assets" "${_client_assets}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_client_assets}/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third-party/nxdk/samples/sdl_ttf/vegur-regular.ttf"
            "${_client_assets}/fonts/vegur-regular.ttf"
        COMMAND "${CMAKE_COMMAND}" "-DLUCIDE_SOURCE_DIR=${MOONLIGHT_LUCIDE_SOURCE_DIR}"
            "-DLUCIDE_ASSET_DIR=${_client_assets}" -P "${CMAKE_CURRENT_LIST_DIR}/stage-lucide.cmake"
        COMMENT "Stage Moonlight client assets"
        VERBATIM)
add_dependencies(moonlight_client stage_client_assets)

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_bundle_assets "$<TARGET_BUNDLE_CONTENT_DIR:moonlight_client>/Resources/assets")
    else()
        set(_bundle_assets "$<TARGET_BUNDLE_DIR:moonlight_client>/assets")
    endif()
    add_custom_command(TARGET moonlight_client POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_client_assets}"
                "${_bundle_assets}"
            COMMENT "Copy assets into the app bundle"
            VERBATIM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        install(TARGETS moonlight_client BUNDLE DESTINATION .)
        set(CPACK_GENERATOR DragNDrop)
        set(CPACK_PACKAGE_NAME Moonlight)
        set(CPACK_PACKAGE_VERSION "0.1.0")
        include(CPack)
    endif()
elseif(NOT ANDROID)
    add_custom_command(TARGET moonlight_client POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_client_assets}"
                "$<TARGET_FILE_DIR:moonlight_client>/assets"
            COMMENT "Copy assets alongside the client executable"
            VERBATIM)
    include(GNUInstallDirs)
    install(TARGETS moonlight_client RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(WIN32)
        install(DIRECTORY "${_client_assets}/" DESTINATION "${CMAKE_INSTALL_BINDIR}/assets")
    else()
        target_compile_definitions(moonlight_client PRIVATE
                MOONLIGHT_INSTALLED_ASSETS="${CMAKE_INSTALL_FULL_DATADIR}/moonlight/assets")
        install(DIRECTORY "${_client_assets}/" DESTINATION "${CMAKE_INSTALL_DATADIR}/moonlight/assets")
    endif()
endif()
