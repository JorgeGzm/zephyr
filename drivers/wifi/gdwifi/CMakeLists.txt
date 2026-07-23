# Copyright (c) 2026 Jorge Guzman
# SPDX-License-Identifier: Apache-2.0
#
# Compiles the open-source layers of the GD32VW55x SDK Wi-Fi stack
# (wifi_manager, util, platform bsp, mbedTLS) and links the prebuilt BSD-3
# MAC/RF/supplicant libraries.  The OS binding is gdwifi_os.c (sys_*
# facade); the IP stack is the Zephyr one (the SDK lwIP does not enter the
# build: the net_compat headers shadow it and gdwifi_drv.c reimplements the
# binary seam of the MAC firmware).
#
# Build flags replicate the vendor demo (compile_commands.json): CFG_RTOS +
# EXEC_USING_STD_PRINTF, libwpas.a (lightweight WPA), mbedTLS from source,
# and NO CONFIG_WPA_SUPPLICANT.

zephyr_library()

# Vendor SDK sources come from the gigadevice_wifi_ble west module
# (BSD-3 sources checked in; prebuilt libraries are west blobs fetched
# from the official GigaDevice repository -- see the module's
# zephyr/module.yml).  A local SDK checkout can still be used through
# CONFIG_WIFI_GDWIFI_SDK_PATH.

if(CONFIG_WIFI_GDWIFI_SDK_PATH STREQUAL "")
  set(GDWIFI_SDK ${ZEPHYR_GIGADEVICE_WIFI_BLE_MODULE_DIR})
  set(GDWIFI_BLOB_LIB ${ZEPHYR_GIGADEVICE_WIFI_BLE_MODULE_DIR}/zephyr/blobs/lib)
else()
  set(GDWIFI_SDK ${CONFIG_WIFI_GDWIFI_SDK_PATH})
  set(GDWIFI_BLOB_LIB ${GDWIFI_SDK}/MSDK/lib)
endif()

set(MSDK ${GDWIFI_SDK}/MSDK)

if(NOT EXISTS ${MSDK}/wifi_manager/wifi_init.c)
  message(FATAL_ERROR
    "GD32VW55x SDK sources not found at ${GDWIFI_SDK} -- run "
    "'west update gigadevice_wifi_ble' (and 'west blobs fetch "
    "gigadevice_wifi_ble' for the radio libraries)")
endif()

if(NOT EXISTS ${GDWIFI_BLOB_LIB}/libwifi.a)
  message(FATAL_ERROR
    "GD32VW55x radio blobs not found at ${GDWIFI_BLOB_LIB} -- run "
    "'west blobs fetch gigadevice_wifi_ble'")
endif()

# Zephyr-side glue

# Note: the reference port also carried a newlib compat (_ctype_/errno) for
# the blobs; under picolibc nothing references those symbols.

zephyr_library_sources(
  gdwifi_os.c
  gdwifi_glue.c
  gdwifi_netif_compat.c
  gdwifi_drv.c
)

# Include paths: the local config dir MUST come first (its platform_def.h,
# app_cfg.h and the net_compat lwIP shims shadow the SDK ones)

zephyr_library_include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/config
  ${CMAKE_CURRENT_SOURCE_DIR}/config/net_compat
  ${GDWIFI_SDK}/config
  ${GDWIFI_SDK}/ROM-EXPORT/bootloader
  ${GDWIFI_SDK}/ROM-EXPORT/halcomm
  ${MSDK}/plf/riscv/gd32vw55x
  ${MSDK}/plf/riscv/NMSIS/Core/Include
  ${MSDK}/plf/riscv/NMSIS/DSP/Include
  ${MSDK}/plf/riscv/arch
  ${MSDK}/plf/riscv/arch/boot
  ${MSDK}/plf/riscv/arch/lib
  ${MSDK}/plf/riscv/arch/ll
  ${MSDK}/plf/riscv/arch/compiler
  ${MSDK}/plf/GD32VW55x_standard_peripheral
  ${MSDK}/plf/GD32VW55x_standard_peripheral/Include
  ${MSDK}/plf/src
  ${MSDK}/plf/src/dma
  ${MSDK}/plf/src/nvds
  ${MSDK}/plf/src/raw_flash
  ${MSDK}/plf/src/reg
  ${MSDK}/plf/src/rf
  ${MSDK}/plf/src/time
  ${MSDK}/plf/src/trng
  ${MSDK}/plf/src/uart
  ${MSDK}/plf/src/wdt
  ${MSDK}/macsw/export
  ${MSDK}/macsw/import
  ${MSDK}/util/include
  ${MSDK}/wifi_manager
  ${MSDK}/wifi_manager/wpas
  ${MSDK}/wpa_supplicant/src
  ${MSDK}/wpa_supplicant/src/utils
  ${MSDK}/wpa_supplicant/src/crypto
  ${MSDK}/rtos/rtos_wrapper
  ${MSDK}/mbedtls/mbedtls/include
  ${MSDK}/mbedtls/mbedtls/library
  ${MSDK}/mbedtls/mbedtls/tests/include/spe
)

# Flags identical to the vendor demo.  GDWIFI_RTOS/GDWIFI_RTOS_NET enable
# the (guarded) external-RTOS hunks patched into the SDK checkout.

zephyr_library_compile_definitions(
  CFG_RTOS
  EXEC_USING_STD_PRINTF
  GDWIFI_RTOS
  GDWIFI_RTOS_NET
  CFG_WLAN_SUPPORT
  # mbedTLS x509: use the built-in software inet_pton (Zephyr only has
  # zsock_inet_pton under this configuration)
  MBEDTLS_TEST_SW_INET_PTON
)

# The SDK sources are not warning-clean under Zephyr's defaults

# When the Zephyr mbedTLS module is enabled (e.g. by the BT host), its
# global MBEDTLS_CONFIG_FILE must not leak into the SDK mbedTLS copy.

zephyr_library_compile_options(
  -UMBEDTLS_CONFIG_FILE
  -UTF_PSA_CRYPTO_CONFIG_FILE
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-unused-function
  -Wno-implicit-fallthrough
)

# wifi_manager (STA + softAP control plane, compiled from source)

set(GDWIFI_MGR ${MSDK}/wifi_manager)
set(GDWIFI_UTIL ${MSDK}/util/src)
set(GDWIFI_PLF ${MSDK}/plf/src)

set(GDWIFI_SDK_SRCS
  ${GDWIFI_MGR}/wifi_eloop.c
  ${GDWIFI_MGR}/wifi_init.c
  ${GDWIFI_MGR}/wifi_management.c
  ${GDWIFI_MGR}/wifi_net_ip.c
  ${GDWIFI_MGR}/wifi_netlink.c
  ${GDWIFI_MGR}/wifi_vif.c
  ${GDWIFI_MGR}/wifi_wpa.c
  ${GDWIFI_UTIL}/debug_print.c
  ${GDWIFI_UTIL}/crc.c
  ${GDWIFI_UTIL}/dlist.c
  ${GDWIFI_UTIL}/slist.c
  ${GDWIFI_UTIL}/trace_ext.c
  ${GDWIFI_UTIL}/util.c
  ${GDWIFI_UTIL}/cyclic_buffer.c
  ${GDWIFI_UTIL}/user_setting.c
  ${GDWIFI_UTIL}/aes_ecb.c
  ${GDWIFI_PLF}/gd32vw55x_platform.c
  ${GDWIFI_PLF}/init_rom.c
  ${GDWIFI_PLF}/plf_assert.c
  ${GDWIFI_PLF}/wakelock.c
  ${GDWIFI_PLF}/dsp.c
  ${GDWIFI_PLF}/time/systime.c
  ${GDWIFI_PLF}/trng/trng.c
  ${GDWIFI_PLF}/nvds/nvds_flash.c
  ${GDWIFI_PLF}/raw_flash/raw_flash_api.c
  ${GDWIFI_PLF}/dma/dma.c
  ${MSDK}/plf/riscv/gd32vw55x/gd32vw55x_it.c
)

zephyr_library_sources(${GDWIFI_SDK_SRCS})

# The SDK sources call the renamed seam functions under their original
# names: force-include the alias map (see gdwifi_rename.h)

set_source_files_properties(${GDWIFI_SDK_SRCS} PROPERTIES COMPILE_OPTIONS
  "-include;${CMAKE_CURRENT_SOURCE_DIR}/config/gdwifi_rename.h")

# mbedTLS 3.6 compiled from source, exactly like the vendor demo.  Some
# crypto symbols still resolve in the mask ROM (rom_symbol_m.gcc below).
# net_sockets.c is TLS-over-BSD-sockets and collides with the Zephyr
# socket headers -- the demo does not compile it either.

file(GLOB GDWIFI_MBEDTLS_SRCS ${MSDK}/mbedtls/mbedtls/library/*.c)
list(FILTER GDWIFI_MBEDTLS_SRCS EXCLUDE REGEX "net_sockets\\.c$")
# The PSA front-end is not used by the SDK stack (wpas/ROM are classic
# mbedTLS API) and collides with the Zephyr PSA when the BT host enables
# the Zephyr mbedTLS module.
list(FILTER GDWIFI_MBEDTLS_SRCS EXCLUDE REGEX "psa_[a-z_]*\\.c$")
zephyr_library_sources(${GDWIFI_MBEDTLS_SRCS})

# When the Zephyr mbedTLS module is also in the build (BT host) it carries
# its own mbedtls_ms_time; rename the SDK copy (same semantics).

set_source_files_properties(${MSDK}/mbedtls/mbedtls/library/platform.c
  PROPERTIES COMPILE_DEFINITIONS "mbedtls_ms_time=gd_sdk_mbedtls_ms_time")

# BLE: either the virtual-HCI controller driver for the native Zephyr
# host (needs a partner libble with HCI) or the vendor-host layer (works
# with the public blobs).

if(CONFIG_BT_GD32VW55X)
  zephyr_library_sources(
    gdwifi_bt.c
    ${MSDK}/ble/app/virtual_hci.c
  )
endif()

zephyr_library_sources_ifdef(CONFIG_GD32VW55X_BLE_VENDOR gdwifi_ble_vendor.c)

if(CONFIG_BT_GD32VW55X OR CONFIG_GD32VW55X_BLE_VENDOR)

  # CFG_BLE_SUPPORT compiles the BLE ISR bodies in gd32vw55x_it.c and,
  # together with CFG_WLAN_SUPPORT, turns the coexistence glue on --
  # exactly the vendor Wi-Fi+BLE build.

  zephyr_library_compile_definitions(
    CFG_BLE_SUPPORT
    CFG_VIRTUAL_HCI_MODE
  )

  # config_max matches libble_max (all roles); it must come BEFORE
  # blesw/src/export where a duplicate (MIN) ble_config.h would shadow it.

  zephyr_library_include_directories(
    ${MSDK}/blesw/src/export/config_max
    ${MSDK}/blesw/src/export
    ${MSDK}/ble/app
  )

  set(GDWIFI_BLE_LIB ${GDWIFI_BLOB_LIB}/libble_max.a)
endif()

# Prebuilt libraries (BSD-3), demo link set: MAC firmware + lightweight
# WPA + RF, closed in a group with our library, plus the NMSIS DSP built
# for the same ISA/ABI (ilp32f).  net_if_up/net_if_down are renamed in a
# patched copy of the archives: they collide with the Zephyr net stack.

set(GDWIFI_BLOB_DIR ${CMAKE_BINARY_DIR}/gdwifi_blobs)
file(MAKE_DIRECTORY ${GDWIFI_BLOB_DIR})

foreach(blob wifi wpas rf)
  add_custom_command(
    OUTPUT ${GDWIFI_BLOB_DIR}/lib${blob}.a
    COMMAND ${CMAKE_OBJCOPY}
            --redefine-sym net_if_up=gdal_net_if_up
            --redefine-sym net_if_down=gdal_net_if_down
            --redefine-sym net_l2_send=gdal_net_l2_send
            # dbg_assert_err is weakened so the port's handler (in
            # gdwifi_glue.c) overrides it: the V1.0.3g blobs ship with the
            # RivieraWaves debug asserts compiled IN, and one of them
            # (mm_task.c:2411, config-vs-HW-state race) fires ~1 in 5
            # connects and would spin the MAC task forever.
            --weaken-symbol dbg_assert_err
            ${GDWIFI_BLOB_LIB}/lib${blob}.a ${GDWIFI_BLOB_DIR}/lib${blob}.a
    DEPENDS ${GDWIFI_BLOB_LIB}/lib${blob}.a
    COMMENT "Renaming colliding symbols in lib${blob}.a"
  )
endforeach()

add_custom_target(gdwifi_blobs DEPENDS
  ${GDWIFI_BLOB_DIR}/libwifi.a
  ${GDWIFI_BLOB_DIR}/libwpas.a
  ${GDWIFI_BLOB_DIR}/librf.a
)

add_dependencies(zephyr_interface gdwifi_blobs)

zephyr_library_link_libraries(
  -Wl,--start-group
  ${GDWIFI_BLOB_DIR}/libwifi.a
  ${GDWIFI_BLOB_DIR}/libwpas.a
  ${GDWIFI_BLOB_DIR}/librf.a
  ${GDWIFI_BLE_LIB}
  ${GDWIFI_BLOB_LIB}/libnmsis_dsp_rv32imafc.a
  kernel
  -Wl,--end-group
)

# Part of the crypto lives in the chip mask ROM: keep the ROM symbol table
# in the link.  The prebuilt supplicant calls setvbuf(stdout) at init;
# thread stdio is not set up -- neutralize it.

zephyr_link_libraries(
  -T${GDWIFI_SDK}/ROM-EXPORT/symbol/rom_symbol_m.gcc
  -Wl,--wrap=setvbuf
)
