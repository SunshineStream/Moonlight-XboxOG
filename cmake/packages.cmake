# Share Sunshine/build-deps' checked-in CPM and annotated package-lock convention.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/cpm/CPM.cmake")

# Register prepared archives or checkouts before CPM's direct download-only Populate call.
# CMake 3.30.3 and newer otherwise ignore FETCHCONTENT_SOURCE_DIR overrides in that path.
function(_moonlight_register_prepared_source package)
    string(TOUPPER "${package}" key)
    set(prepared_source "${FETCHCONTENT_SOURCE_DIR_${key}}")
    if(CMAKE_VERSION VERSION_LESS "3.30.3" OR NOT prepared_source)
        return()
    endif()
    if(NOT IS_DIRECTORY "${prepared_source}")
        message(FATAL_ERROR "FETCHCONTENT_SOURCE_DIR_${key} does not exist: ${prepared_source}")
    endif()
    string(TOLOWER "${package}" package_dir)
    FetchContent_SetPopulated(${package} SOURCE_DIR "${prepared_source}"
            BINARY_DIR "${FETCHCONTENT_BASE_DIR}/${package_dir}-build")
endfunction()

# Declare a dependency whose caller configures or builds its sources separately.
macro(MOONLIGHT_DECLARE_SOURCE_PACKAGE package)
    _moonlight_register_prepared_source("${package}")
    CPMDeclarePackage("${package}" ${ARGN} DOWNLOAD_ONLY YES)
endmacro()

CPMUsePackageLock("${CMAKE_SOURCE_DIR}/package-lock.cmake")
