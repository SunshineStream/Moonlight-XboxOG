# Build Moonlight's adapters against unmodified upstream nxdk sources.
include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/patch.cmake")

# Build Moonlight's private network port and the runtime used by dependencies.
function(moonlight_prepare_nxdk_compat)
    set(compat "${CMAKE_SOURCE_DIR}/src/_nxdk_compat")
    set(port "${CMAKE_CURRENT_BINARY_DIR}/nxdk-compat")
    moonlight_patch_sources("${NXDK_DIR}" "${port}" "${compat}/network/nxdk-network.patch"
            lib/net/nforceif/include/arch/sys_arch.h
            lib/net/nforceif/src/sys_arch.c
            lib/net/nvnetdrv/nvnetdrv_lwip.c)

    # opt.h includes this before defining defaults. Keep nxdk's base settings.
    configure_file("${compat}/network/lwipopts.h.in" "${port}/lwipopts.h" @ONLY)

    set(LWIP_DIR "${NXDK_DIR}/lib/net/lwip")  # cmake-lint: disable=C0103
    set(LWIP_EXCLUDE_SLIPIF TRUE)  # cmake-lint: disable=C0103
    include("${LWIP_DIR}/src/Filelists.cmake")
    add_library(moonlight_nxdk_net STATIC
            ${lwipcore_SRCS} ${lwipcore4_SRCS} ${lwipcore6_SRCS}
            ${lwipapi_SRCS} ${lwipnetif_SRCS} ${lwipmdns_SRCS}
            "${NXDK_DIR}/lib/net/nvnetdrv/nvnetdrv.c"
            "${port}/lib/net/nvnetdrv/nvnetdrv_lwip.c"
            "${port}/lib/net/nforceif/src/sys_arch.c")
    target_compile_definitions(moonlight_nxdk_net PRIVATE RX_BUFF_CNT=PBUF_POOL_SIZE)
    target_include_directories(moonlight_nxdk_net BEFORE PUBLIC
            "${port}/lib/net/nforceif/include" "${port}" "${compat}/include")
    target_include_directories(moonlight_nxdk_net SYSTEM PUBLIC
            "${NXDK_DIR}/lib/net/lwip/src/include"
            "${NXDK_DIR}/lib/net/nforceif/include"
            "${NXDK_DIR}/lib/net/nvnetdrv")

    # nxNetInit owns a struct netif: it must use the same mDNS-enabled layout.
    # Include it as an object so the stock libnxdk net.obj is never extracted.
    add_library(moonlight_nxdk_net_init OBJECT "${NXDK_DIR}/lib/nxdk/net.c")
    target_link_libraries(moonlight_nxdk_net_init PRIVATE moonlight_nxdk_net)
    get_target_property(sdk_libraries NXDK::NXDK INTERFACE_LINK_LIBRARIES)
    list(REMOVE_ITEM sdk_libraries nxdk_net)
    set_property(TARGET NXDK::NXDK PROPERTY INTERFACE_LINK_LIBRARIES "${sdk_libraries};moonlight_nxdk_net")
    set_property(TARGET NXDK::Net PROPERTY INTERFACE_LINK_LIBRARIES moonlight_nxdk_net)

    # FFmpeg configures before ordinary targets build. Build its real timing
    # helpers now as well, so configure still links genuine Xbox executables.
    moonlight_run_nxdk_command("Compatibility probe object" "${NXDK_DIR}" "${port}"
            "${NXDK_DIR}/bin/nxdk-cc" "-I${compat}/include"
            -c "${compat}/runtime.c" -o runtime.obj)
    moonlight_run_nxdk_command("FFmpeg allocation probe object" "${NXDK_DIR}" "${port}"
            "${NXDK_DIR}/bin/nxdk-cc"
            -c "${compat}/ffmpeg_memory.c" -o ffmpeg_memory.obj)
    moonlight_run_nxdk_command("Compatibility probe archive" "${NXDK_DIR}" "${port}"
            llvm-ar rcs compat.lib runtime.obj ffmpeg_memory.obj)
    set(MOONLIGHT_NXDK_COMPAT_LIBRARY "${port}/compat.lib" PARENT_SCOPE)
    add_library(moonlight_nxdk_compat STATIC "${compat}/runtime.c" "${compat}/ffmpeg_memory.c")
    target_include_directories(moonlight_nxdk_compat PUBLIC "${compat}/include")
    target_compile_options(moonlight_nxdk_compat INTERFACE
            -include "${compat}/include/moonlight_posix.h")

    # PVIDEO needs one immutable background page at 1080i. Link the adapted
    # object directly so nxdk's stock pbkit core is not also extracted.
    moonlight_patch_sources("${NXDK_DIR}" "${port}" "${compat}/pbkit-static-scanout.patch"
            lib/pbkit/pbkit.c)
    add_library(moonlight_pbkit_scanout OBJECT "${port}/lib/pbkit/pbkit.c")
    target_include_directories(moonlight_pbkit_scanout PRIVATE "${NXDK_DIR}/lib/pbkit")

    file(GLOB_RECURSE compat_inputs CONFIGURE_DEPENDS "${compat}/*.h" "${compat}/*.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${compat_inputs})
endfunction()
