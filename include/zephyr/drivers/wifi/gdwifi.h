/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief GD32VW55x Wi-Fi/BLE driver: vendor BLE host hooks
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_WIFI_GDWIFI_H_
#define ZEPHYR_INCLUDE_DRIVERS_WIFI_GDWIFI_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tell whether the vendor BLE host is up.
 *
 * With CONFIG_WIFI_GDWIFI_BLE_VENDOR the driver starts the RivieraWaves host
 * embedded in the radio blob during the radio bring-up, after the application
 * threads may already be running. The vendor API (ble_scan_*, ble_conn_*,
 * ble_sec_*, ble_gattc_*, callback registration included) must not be called
 * before the host reports its enable complete event.
 *
 * @retval true The host reported enable complete; the vendor API may be used.
 * @retval false The host is not started yet, or the vendor path is disabled.
 */
bool gdwifi_ble_vendor_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_WIFI_GDWIFI_H_ */
