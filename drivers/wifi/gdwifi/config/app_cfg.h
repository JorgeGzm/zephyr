/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Application feature knobs for the SDK radio sources, found before the
 * vendor MSDK/app/app_cfg.h on the include path. Only the debug print is
 * kept; the AT/CLI shells are not built, and CONFIG_SNTP is not defined
 * because it collides with the Zephyr Kconfig macro and the SDK SNTP
 * client is not compiled.
 */

#ifndef _APP_CFG_H_
#define _APP_CFG_H_

#include "platform_def.h"

#define CONFIG_DEBUG_PRINT_ENABLE

#endif /* _APP_CFG_H_ */
