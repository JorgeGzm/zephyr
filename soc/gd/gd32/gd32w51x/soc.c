/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>

#include <soc.h>

/*
 * Zephyr runs as a Secure-only image with the SAU disabled, so every bus
 * transaction is Secure. The peripherals (TZSPC) and the RCU are Non-secure
 * out of reset and answer on their Non-secure aliases (0x4xxxxxxx); the GPIO
 * driver releases the pins to the Non-secure peripherals.
 */
void soc_early_init_hook(void)
{
	/* Clocks (PLL at 180 MHz from HXTAL) and instruction cache */
	SystemInit();
}
