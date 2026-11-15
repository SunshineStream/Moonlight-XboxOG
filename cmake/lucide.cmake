# Fetch the static SVG distribution during the Xbox build. No JavaScript runtime
# or package-manager install is needed by the console or the host-native tests.
include_guard(GLOBAL)
include(ExternalProject)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(MOONLIGHT_LUCIDE_VERSION "1.73.0")
set(MOONLIGHT_LUCIDE_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/third-party/lucide-static")
ExternalProject_Add(lucide_static
        URL "https://registry.npmjs.org/lucide-static/-/lucide-static-${MOONLIGHT_LUCIDE_VERSION}.tgz"
        URL_HASH SHA256=be3b73fa4fc10727c1c2c21dc11bd8d91a36012ce3aa9436eda4c2020021f4d1
        SOURCE_DIR "${MOONLIGHT_LUCIDE_SOURCE_DIR}"
        PREFIX "${CMAKE_CURRENT_BINARY_DIR}/third-party/lucide-static-build"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        TEST_COMMAND "")
