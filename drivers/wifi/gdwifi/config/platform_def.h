/*
 * Copyright (c) 2023, GigaDevice Semiconductor Inc.
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Radio-build platform definition, shadowing the SDK config/platform_def.h
 * (which unconditionally defines CONFIG_BOARD -- a macro that collides
 * with the Zephyr Kconfig string of the same name).  The compiled SDK
 * sources do not test CONFIG_BOARD, so it is simply omitted.
 * CFG_WLAN_SUPPORT comes from the driver CMake as a compile definition.
 */

#ifndef _PLATFORM_DEF_H
#define _PLATFORM_DEF_H

#define PLATFORM_FPGA_32103_V7          1
#define PLATFORM_FPGA_32103_ULTRA       2
#define PLATFORM_ASIC_32103             103

#define CONFIG_PLATFORM                 PLATFORM_ASIC_32103

#if CONFIG_PLATFORM >= PLATFORM_ASIC_32103
#define CONFIG_PLATFORM_ASIC
#else
#define CONFIG_PLATFORM_FPGA
#endif

#define PLATFORM_BOARD_32VW55X_START    1
#define PLATFORM_BOARD_32VW55X_EVAL     2
#define PLATFORM_BOARD_32VW55X_F527     3
#define PLATFORM_BOARD_32VW55X_SONIC    4

#define RF_GDM32106                     1
#define RF_GDM32110                     2
#define RF_GDM32103                     3

#define CONFIG_RF_TYPE                  RF_GDM32103

#define CRYSTAL_26M                     1
#define CRYSTAL_40M                     2
#define CRYSTAL_48M                     3
#define PLATFORM_CRYSTAL                CRYSTAL_40M

#if defined(CFG_WLAN_SUPPORT) && defined(CFG_BLE_SUPPORT)
#define CFG_COEX
#endif

#define NVDS_FLASH_SUPPORT              1

#endif /* _PLATFORM_DEF_H */
