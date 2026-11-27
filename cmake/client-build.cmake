include("${CMAKE_CURRENT_LIST_DIR}/client-media.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/sdl3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/lucide.cmake")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Enable both languages before SDL registers its Objective-C sources.
    enable_language(OBJC OBJCXX)
elseif(WIN32)
    enable_language(RC)
    if(SUNSHINE_HOST_TOOL_DIR AND CMAKE_RC_COMPILER MATCHES "windres[.]exe$")
        # windres runs a separate preprocessor, and CLion's build may lack MSYS2 on PATH.
        string(PREPEND CMAKE_RC_FLAGS "--preprocessor=\"${CMAKE_C_COMPILER}\" "
                "--preprocessor-arg=-E --preprocessor-arg=-xc-header --preprocessor-arg=-DRC_INVOKED ")
    endif()
endif()

sunshine_prepare_native_sdl3()
sunshine_prepare_client_media()

set(_client_sources ${SUNSHINE_SOURCES})
list(REMOVE_ITEM _client_sources
        "${CMAKE_SOURCE_DIR}/src/platform/usb_input.cpp"
        "${CMAKE_SOURCE_DIR}/src/startup/memory_stats.cpp")

if(ANDROID)
    # SDLActivity loads libmain.so through JNI.
    add_library(sunshine_client SHARED ${_client_sources})
    set_target_properties(sunshine_client PROPERTIES OUTPUT_NAME main)
elseif(APPLE)
    add_executable(sunshine_client MACOSX_BUNDLE ${_client_sources})
    set_target_properties(sunshine_client PROPERTIES OUTPUT_NAME SunshineClient
            MACOSX_BUNDLE_GUI_IDENTIFIER dev.lizardbyte.app.SunshineClient
            MACOSX_BUNDLE_BUNDLE_NAME Sunshine
            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/apple/Info.plist.in")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_client_icon "${CMAKE_SOURCE_DIR}/packaging/apple/SunshineClient.icns")
        set(SUNSHINE_APPLE_ICON_PLIST "<key>CFBundleIconFile</key>\n    <string>SunshineClient.icns</string>")
    else()
        set(_client_icon "${CMAKE_SOURCE_DIR}/packaging/apple/${CMAKE_SYSTEM_NAME}.xcassets")
        set_target_properties(sunshine_client PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
    endif()
    set_source_files_properties("${_client_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(sunshine_client PRIVATE "${_client_icon}")
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        string(CONCAT SUNSHINE_IOS_PORTRAIT_ORIENTATIONS
                "<string>UIInterfaceOrientationPortrait</string>\n"
                "        <string>UIInterfaceOrientationPortraitUpsideDown</string>")
        set(_client_orientation "${CMAKE_SOURCE_DIR}/src/ui/mobile_orientation_ios.mm")
        set_source_files_properties("${_client_orientation}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_sources(sunshine_client PRIVATE "${_client_orientation}")
        target_compile_definitions(sunshine_client PRIVATE SUNSHINE_IOS)
        target_link_libraries(sunshine_client PRIVATE "-framework UIKit")
    endif()
elseif(WIN32)
    set(_windows_resources "${CMAKE_SOURCE_DIR}/packaging/windows/sunshine-client.rc")
    set_source_files_properties("${_windows_resources}" PROPERTIES
            OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/windows/sunshine-client.ico")
    add_executable(sunshine_client WIN32 ${_client_sources} "${_windows_resources}")
    set_target_properties(sunshine_client PROPERTIES OUTPUT_NAME SunshineClient)
else()
    add_executable(sunshine_client ${_client_sources})
    set_target_properties(sunshine_client PROPERTIES OUTPUT_NAME sunshine-client)
endif()

if(SUNSHINE_SCREENSHOT_DEMO AND WIN32)
    set_target_properties(sunshine_client PROPERTIES OUTPUT_NAME SunshineClientDemo)
endif()

target_sources(sunshine_client PRIVATE "${CMAKE_SOURCE_DIR}/src/_nxdk_compat/ffmpeg_memory.c")
# Select the desktop and mobile platform adapters in shared sources.
target_compile_definitions(sunshine_client PRIVATE SUNSHINE_NATIVE_CLIENT TOML_EXCEPTIONS=0)
if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
    target_compile_definitions(sunshine_client PRIVATE SUNSHINE_DESKTOP)
endif()
target_include_directories(sunshine_client PRIVATE "${CMAKE_SOURCE_DIR}" "${tomlplusplus_SOURCE_DIR}/include")
target_link_libraries(sunshine_client PRIVATE
        SunshineClient::OpenSSL moonlight-common-c PkgConfig::SUNSHINE_CLIENT_FFMPEG
        SDL3::SDL3 SDL3_ttf::SDL3_ttf-static SDL3_image::SDL3_image-static)
sunshine_prioritize_bundled_openssl_headers(sunshine_client)
if(ANDROID)
    # FFmpeg's AArch64 assembly addresses private tables directly. Keep static
    # library symbols local to libmain.so so those references cannot be interposed.
    target_link_options(sunshine_client PRIVATE "LINKER:--exclude-libs,ALL")
elseif(WIN32)
    target_link_libraries(sunshine_client PRIVATE ws2_32)
    target_link_options(sunshine_client PRIVATE -static-libgcc -static-libstdc++)
endif()

set(SUNSHINE_CLIENT_ASSET_ROOT "${CMAKE_CURRENT_BINARY_DIR}/client-assets/assets"
        CACHE PATH "Output directory for staged client assets")
set(_client_assets "${SUNSHINE_CLIENT_ASSET_ROOT}")
add_custom_target(stage_client_assets
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/xbe/assets" "${_client_assets}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_client_assets}/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third-party/nxdk/samples/sdl_ttf/vegur-regular.ttf"
            "${_client_assets}/fonts/vegur-regular.ttf"
        COMMAND "${CMAKE_COMMAND}" "-DLUCIDE_SOURCE_DIR=${SUNSHINE_LUCIDE_SOURCE_DIR}"
            "-DLUCIDE_ASSET_DIR=${_client_assets}" -P "${CMAKE_CURRENT_LIST_DIR}/stage-lucide.cmake"
        COMMENT "Stage Sunshine client assets"
        VERBATIM)
add_dependencies(sunshine_client stage_client_assets)

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(_bundle_assets "$<TARGET_BUNDLE_CONTENT_DIR:sunshine_client>/Resources/assets")
    else()
        set(_bundle_assets "$<TARGET_BUNDLE_DIR:sunshine_client>/assets")
    endif()
    add_custom_command(TARGET sunshine_client POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_client_assets}"
                "${_bundle_assets}"
            COMMAND "${CMAKE_COMMAND}" "-DASSET_DIR=${_bundle_assets}"
                -P "${CMAKE_CURRENT_LIST_DIR}/remove-legacy-brand-assets.cmake"
            COMMENT "Copy assets into the app bundle"
            VERBATIM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        install(TARGETS sunshine_client BUNDLE DESTINATION .)
        set(CPACK_GENERATOR DragNDrop)
        set(CPACK_PACKAGE_NAME Sunshine-client)
        set(CPACK_PACKAGE_VERSION "0.1.0")
        include(CPack)
    endif()
elseif(NOT ANDROID)
    add_custom_command(TARGET sunshine_client POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_client_assets}"
                "$<TARGET_FILE_DIR:sunshine_client>/assets"
            COMMAND "${CMAKE_COMMAND}" "-DASSET_DIR=$<TARGET_FILE_DIR:sunshine_client>/assets"
                -P "${CMAKE_CURRENT_LIST_DIR}/remove-legacy-brand-assets.cmake"
            COMMENT "Copy assets alongside the client executable"
            VERBATIM)
    include(GNUInstallDirs)
    install(TARGETS sunshine_client RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(WIN32)
        install(DIRECTORY "${_client_assets}/" DESTINATION "${CMAKE_INSTALL_BINDIR}/assets")
    else()
        target_compile_definitions(sunshine_client PRIVATE
                SUNSHINE_INSTALLED_ASSETS="${CMAKE_INSTALL_FULL_DATADIR}/sunshine-client/assets")
        install(DIRECTORY "${_client_assets}/" DESTINATION "${CMAKE_INSTALL_DATADIR}/sunshine-client/assets")
        set(_linux_app_id dev.lizardbyte.app.SunshineClient)
        set(_linux_metadata "${CMAKE_SOURCE_DIR}/packaging/linux/flatpak")
        install(FILES "${_linux_metadata}/${_linux_app_id}.desktop"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
        install(FILES "${_linux_metadata}/${_linux_app_id}.metainfo.xml"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
        install(FILES "${CMAKE_SOURCE_DIR}/sunshine-logo.svg"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
                RENAME "${_linux_app_id}.svg")
    endif()
endif()
