/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Force-included in the SDK sources (never in the Zephyr-side glue).
 * Four functions of the SDK lwIP seam collide with public symbols of the
 * Zephyr network stack; rename them at the preprocessor level here and at
 * the archive level (objcopy --redefine-sym, see CMakeLists.txt) in the
 * prebuilt libraries.  The implementations live in gdwifi_drv.c and
 * gdwifi_netif_compat.c under the gdal_ names.
 */

#ifndef _GDWIFI_RENAME_H_
#define _GDWIFI_RENAME_H_

#define net_if_up          gdal_net_if_up
#define net_if_down        gdal_net_if_down
#define net_if_get_name    gdal_net_if_get_name
#define net_if_set_default gdal_net_if_set_default
#define net_l2_send        gdal_net_l2_send

/* Zephyr's Kconfig defines CONFIG_BOARD as a string; the SDK platform
 * headers compare it against PLATFORM_BOARD_32VW55X_* numeric codes.
 * Give the SDK sources the numeric value (START board = 1).
 */

#undef CONFIG_BOARD
#define CONFIG_BOARD 1

/* Some SDK sources (nvds_flash.c under CFG_BLE_SUPPORT) rely on includes
 * and macros that other vendor headers used to drag in; gcc 14 makes the
 * implicit declarations hard errors.  Provide them up front (identical to
 * the SDK definitions -- its own guarded block then no-ops).
 */

#include <stdint.h>
#include <string.h>

#ifndef BIT
#define BIT(x)                       ((uint32_t)((uint32_t)0x00000001U<<(x)))
#define BITS(start, end)             ((0xFFFFFFFFUL << (start)) & (0xFFFFFFFFUL >> (31U - (uint32_t)(end))))
#define GET_BITS(regval, start, end) (((regval) & BITS((start),(end))) >> (start))
#endif

#endif /* _GDWIFI_RENAME_H_ */
