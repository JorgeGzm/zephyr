/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Application feature knobs for the SDK radio sources (replaces
 * MSDK/app/app_cfg.h).  Same knobs as the vendor demo, minus the AT/CLI
 * shells.  CONFIG_SNTP is deliberately NOT defined: it collides with the
 * Zephyr Kconfig macro and the SDK SNTP client is not compiled.
 */

#ifndef _APP_CFG_H_
#define _APP_CFG_H_

#include "platform_def.h"

#define CONFIG_DEBUG_PRINT_ENABLE

#endif /* _APP_CFG_H_ */
