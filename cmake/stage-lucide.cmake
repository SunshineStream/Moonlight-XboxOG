# Stage only the UI's Lucide icons, preserving the downloaded originals.
if(NOT DEFINED LUCIDE_SOURCE_DIR OR NOT DEFINED LUCIDE_ASSET_DIR)
    message(FATAL_ERROR "Lucide staging requires LUCIDE_SOURCE_DIR and LUCIDE_ASSET_DIR")
endif()

file(MAKE_DIRECTORY "${LUCIDE_ASSET_DIR}/icons" "${LUCIDE_ASSET_DIR}/licenses")
foreach(icon IN ITEMS
        settings circle-question-mark circle-plus monitor gamepad-2 scroll-text rotate-ccw
        network hash activity key-round save loader-circle circle-check circle-x info
        keyboard mouse power panels-top-left share-2 camera chart-no-axes-combined)
    set(_icon_source "${LUCIDE_SOURCE_DIR}/icons/${icon}.svg")
    if(NOT EXISTS "${_icon_source}")
        message(FATAL_ERROR "Required Lucide icon not found: ${_icon_source}")
    endif()
    file(READ "${_icon_source}" _icon_svg)
    # NanoSVG does not resolve CSS currentColor. Bake the shell's foreground
    # colors into the build copy so standalone SVGs work without CSS support.
    set(_icon_color "#F2F5F8")
    if(icon STREQUAL "loader-circle" OR icon STREQUAL "info")
        set(_icon_color "#86B7FF")
    elseif(icon STREQUAL "circle-check")
        set(_icon_color "#7DD3A8")
    elseif(icon STREQUAL "circle-x")
        set(_icon_color "#F28B82")
    endif()
    string(REPLACE "currentColor" "${_icon_color}" _icon_svg "${_icon_svg}")
    # Older NanoSVG requires separators between arc flags and coordinates.
    # Expand the compact question-mark flags in the build copy only.
    if(icon STREQUAL "circle-question-mark")
        string(REPLACE "0 016" "0 0 1 6" _icon_svg "${_icon_svg}")
    endif()
    set(_icon_output "${LUCIDE_ASSET_DIR}/icons/${icon}.svg")
    # configure_file avoids replacing unchanged files during incremental builds.
    file(WRITE "${_icon_output}.tmp" "${_icon_svg}")
    configure_file("${_icon_output}.tmp" "${_icon_output}" COPYONLY)
    file(REMOVE "${_icon_output}.tmp")
endforeach()

configure_file("${LUCIDE_SOURCE_DIR}/LICENSE" "${LUCIDE_ASSET_DIR}/licenses/lucide.txt" COPYONLY)

# Remove obsolete first-party icons from existing build directories, too.
foreach(legacy_icon IN ITEMS
        gear support add-host host-monitor-online host-monitor-pairing host-monitor-offline
        circle-help life-buoy monitor-check monitor-dot monitor-off ethernet-port question-mark)
    file(REMOVE "${LUCIDE_ASSET_DIR}/icons/${legacy_icon}.svg")
endforeach()
