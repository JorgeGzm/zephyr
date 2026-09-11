/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Applied after the SDK mbedtls_config.h (MBEDTLS_USER_CONFIG_FILE).
 *
 * The SDK configuration routes AES, DES, SHA-256 and the ECDSA primitives
 * to the CAU, HAU and PKCAU engines. Those engines belong to the Zephyr
 * crypto drivers, which own their state and serialize access; the SDK
 * cannot share them, so its mbedTLS copy runs the software implementations.
 */

#ifndef ZEPHYR_DRIVERS_WIFI_GDWIFI_CONFIG_GDWIFI_MBEDTLS_USER_CONFIG_H_
#define ZEPHYR_DRIVERS_WIFI_GDWIFI_CONFIG_GDWIFI_MBEDTLS_USER_CONFIG_H_

#undef CONFIG_HW_SECURITY_ENGINE
#undef MBEDTLS_AES_ALT
#undef MBEDTLS_DES_ALT
#undef MBEDTLS_SHA256_ALT
#undef MBEDTLS_ECDSA_VERIFY_ALT
#undef MBEDTLS_ECDSA_SIGN_ALT

/* The AES tables come precomputed from flash instead of being built in RAM */
#define MBEDTLS_AES_ROM_TABLES

#endif /* ZEPHYR_DRIVERS_WIFI_GDWIFI_CONFIG_GDWIFI_MBEDTLS_USER_CONFIG_H_ */
