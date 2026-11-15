# Stage only the UI's Lucide icons, preserving the downloaded originals.
if(NOT DEFINED LUCIDE_SOURCE_DIR OR NOT DEFINED LUCIDE_ASSET_DIR)
    message(FATAL_ERROR "Lucide staging requires LUCIDE_SOURCE_DIR and LUCIDE_ASSET_DIR")
endif()

file(MAKE_DIRECTORY "${LUCIDE_ASSET_DIR}/icons" "${LUCIDE_ASSET_DIR}/licenses")
foreach(icon IN ITEMS settings life-buoy circle-plus monitor-check monitor-dot monitor-off monitor gamepad-2)
    set(_icon_source "${LUCIDE_SOURCE_DIR}/icons/${icon}.svg")
    if(NOT EXISTS "${_icon_source}")
        message(FATAL_ERROR "Required Lucide icon not found: ${_icon_source}")
    endif()
    file(READ "${_icon_source}" _icon_svg)
    # NanoSVG does not resolve CSS currentColor. Bake the shell's foreground
    # color into the build copy so standalone SVGs work without CSS support.
    if(icon STREQUAL "monitor-check")
        set(_icon_color "#79DBB0")
    elseif(icon STREQUAL "monitor-dot")
        set(_icon_color "#82B4FF")
    elseif(icon STREQUAL "monitor-off")
        set(_icon_color "#A3ABB5")
    else()
        set(_icon_color "#F2F5F8")
    endif()
    string(REPLACE "currentColor" "${_icon_color}" _icon_svg "${_icon_svg}")
    set(_icon_output "${LUCIDE_ASSET_DIR}/icons/${icon}.svg")
    # configure_file avoids replacing unchanged files during incremental builds.
    file(WRITE "${_icon_output}.tmp" "${_icon_svg}")
    configure_file("${_icon_output}.tmp" "${_icon_output}" COPYONLY)
    file(REMOVE "${_icon_output}.tmp")
endforeach()

configure_file("${LUCIDE_SOURCE_DIR}/LICENSE" "${LUCIDE_ASSET_DIR}/licenses/lucide.txt" COPYONLY)

# Remove obsolete first-party icons from existing build directories, too.
foreach(legacy_icon IN ITEMS
        gear support add-host host-monitor-online host-monitor-pairing host-monitor-offline circle-help)
    file(REMOVE "${LUCIDE_ASSET_DIR}/icons/${legacy_icon}.svg")
endforeach()
