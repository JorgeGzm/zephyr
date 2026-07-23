/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/irq.h>

#include <gd32vw55x.h>

void soc_early_init_hook(void)
{
	uint32_t key;

	key = irq_lock();

	/* Reproduce the vendor demo bring-up: IRC16M -> 160 MHz PLLDIG from
	 * the 40 MHz HXTAL (system_clock_160m_40m_hxtal).
	 */
	SystemInit();

	/* Switch the Nuclei SysTimer clock source to the full system clock
	 * so mtime ticks at SYS_CLOCK_HW_CYCLES_PER_SEC (160 MHz).
	 */
	SysTimer_SetControlValue(SysTimer_GetControlValue() |
				 SysTimer_MTIMECTL_CLKSRC_Msk);

	/* The vendor environment enables the instruction cache early; nobody
	 * else does it for us and UART bursts drop characters without it.
	 */
	EnableICache();

	irq_unlock(key);
}
