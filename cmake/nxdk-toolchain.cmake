# Stage nxdk before CMake runs compiler checks, then use its stock toolchain.
include("${CMAKE_CURRENT_LIST_DIR}/nxdk-source.cmake")
if(CMAKE_HOST_WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/msys2.cmake")
    moonlight_prepend_windows_msys2_path(_nxdk_host_path)
    moonlight_get_windows_msys2_usr_bin(_nxdk_usr_bin)
    set(ENV{PATH} "${_nxdk_host_path}")
    set(ENV{MSYSTEM} "UCRT64")
    set(CMAKE_MAKE_PROGRAM "${_nxdk_usr_bin}/make.exe" CACHE FILEPATH "MSYS2 make for the Xbox build")
endif()
get_filename_component(moonlight_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MOONLIGHT_NXDK_DIR "${moonlight_source}/third-party/nxdk" CACHE PATH "Path to the nxdk source checkout")
set(MOONLIGHT_NXDK_BUILD_DIR "${CMAKE_BINARY_DIR}/nxdk" CACHE PATH "Build-directory copy of nxdk")
cmake_path(ABSOLUTE_PATH MOONLIGHT_NXDK_DIR BASE_DIRECTORY "${moonlight_source}" NORMALIZE)
cmake_path(ABSOLUTE_PATH MOONLIGHT_NXDK_BUILD_DIR BASE_DIRECTORY "${CMAKE_BINARY_DIR}" NORMALIZE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MOONLIGHT_NXDK_DIR MOONLIGHT_NXDK_BUILD_DIR)
get_property(in_try_compile GLOBAL PROPERTY IN_TRY_COMPILE)
if(NOT in_try_compile)
    moonlight_stage_nxdk("${MOONLIGHT_NXDK_DIR}" "${MOONLIGHT_NXDK_BUILD_DIR}")
endif()
set(NXDK_DIR "${MOONLIGHT_NXDK_BUILD_DIR}" CACHE PATH "Prepared nxdk build copy" FORCE)
set(ENV{NXDK_DIR} "${NXDK_DIR}")
include("${NXDK_DIR}/share/toolchain-nxdk.cmake")

if(CMAKE_HOST_WIN32 AND MOONLIGHT_NXDK_NATIVE_COMPILER)
    # CLion runs compiler probes directly. Use Win32 executables for the direct Xbox presets.
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MOONLIGHT_NXDK_NATIVE_COMPILER)
    moonlight_get_windows_msys2_msystem_bin(_nxdk_ucrt_bin)
    find_program(_nxdk_clang NAMES clang.exe HINTS "${_nxdk_ucrt_bin}" NO_DEFAULT_PATH REQUIRED)
    find_program(_nxdk_clangxx NAMES clang++.exe HINTS "${_nxdk_ucrt_bin}" NO_DEFAULT_PATH REQUIRED)
    set(CMAKE_C_COMPILER "${_nxdk_clang}")
    set(CMAKE_CXX_COMPILER "${_nxdk_clangxx}")
    set(_nxdk_target i386-pc-win32)
    set(CMAKE_C_COMPILER_TARGET "${_nxdk_target}")
    set(CMAKE_CXX_COMPILER_TARGET "${_nxdk_target}")

    # The stock toolchain supplies the SDK include directories and linker rules.
    # Keep the target flag in the File API's compile flags for CLion's compiler probes.
    string(CONCAT _nxdk_compile_flags "--target=${_nxdk_target} -march=pentium3 -ffreestanding -nostdlib -fno-builtin "
            "-Wno-builtin-macro-redefined -DNXDK -D__STDC__=1 -U__STDC_NO_THREADS__")
    set(CMAKE_C_FLAGS_INIT "${_nxdk_compile_flags}")
    set(CMAKE_CXX_FLAGS_INIT "${_nxdk_compile_flags} -fno-exceptions")
    moonlight_get_windows_msys2_bash(_nxdk_bash)
    foreach(language C CXX)
        # The SDK link script needs lld on PATH when CLion starts an independent build process.
        string(CONCAT CMAKE_${language}_LINK_EXECUTABLE
                "<CMAKE_COMMAND> -E env --modify \"PATH=path_list_prepend:${_nxdk_ucrt_bin}\" "
                "\"${_nxdk_bash}\" \"${NXDK_DIR}/bin/nxdk-link\" "
                "<CMAKE_${language}_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -out:<TARGET> <LINK_LIBRARIES>")
    endforeach()

    # FindPkgConfig launches commands directly on Windows. Run the SDK wrapper through Bash.
    set(PKG_CONFIG_EXECUTABLE "${_nxdk_bash}")
    set(PKG_CONFIG_ARGN "${NXDK_DIR}/bin/nxdk-pkg-config")
endif()
