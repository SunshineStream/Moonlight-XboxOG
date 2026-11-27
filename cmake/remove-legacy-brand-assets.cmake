# Copying a directory into a reused staging tree leaves removed assets behind.
if(NOT IS_ABSOLUTE "${ASSET_DIR}")
    message(FATAL_ERROR "Removing legacy branding requires an absolute asset directory")
endif()
file(REMOVE
        "${ASSET_DIR}/moonlight-logo.svg"
        "${ASSET_DIR}/moonlight-logo-wedges.svg"
        "${ASSET_DIR}/moonlight-logo.ppm"
        "${ASSET_DIR}/hosts/moon-full.svg"
        "${ASSET_DIR}/hosts/moon-half.svg"
        "${ASSET_DIR}/hosts/moon-crescent.svg"
        "${ASSET_DIR}/hosts/sun-offline.svg"
        "${ASSET_DIR}/hosts/sun-paired.svg"
        "${ASSET_DIR}/hosts/sun-unpaired.svg")
