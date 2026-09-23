/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Deep-sleep support.
 *
 * The suspend-to-idle state is the PMU deep-sleep mode: the core clocks
 * stop, SRAM and peripheral registers keep their contents, and the system
 * comes back on the IRC16M. The RTC keeps running from the 32 kHz clock
 * and provides both the wake-up and the time accounting:
 *
 * - The wake-up timer of the RTC is loaded with the time left until the
 *   next kernel timeout, read back from the system timer compare register,
 *   and its interrupt goes through EXTI line 21.
 * - The system timer (mtime) stops in deep-sleep, so on the way out the
 *   time spent asleep is measured on the RTC calendar and added to mtime;
 *   the kernel then accounts the elapsed ticks as usual.
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/interrupt_controller/gd32_exti.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/util.h>

#include <gd32vw55x.h>

/* Wake-up timer clock: the RTC clock divided by 16, 16-bit reload */
#define GD32_PM_WUT_DIV   16U
#define GD32_PM_WUT_MAX   0xFFFFU
#define GD32_PM_IRC32K_HZ 32000U
#define GD32_PM_LXTAL_HZ  32768U

#define GD32_PM_EXTI_LINE_RTC_WAKEUP 21U

#define GD32_PM_RTC_FLAG_TIMEOUT_US 20000U
#define GD32_PM_RTC_FLAG_POLL_US    10U

#define GD32_PM_SECONDS_PER_DAY 86400U

/* Vendor helper that rebuilds the PLLDIG clock tree, also used by SystemInit() */
extern void system_clock_config(void);

static uint32_t gd32_pm_rtc_hz(void)
{
	return ((RCU_BDCTL & RCU_BDCTL_RTCSRC) == RCU_RTCSRC_LXTAL) ? GD32_PM_LXTAL_HZ
								    : GD32_PM_IRC32K_HZ;
}

static int gd32_pm_rtc_wait_flag(uint32_t flag)
{
	uint32_t waited = 0U;

	while ((RTC_STAT & flag) == 0U) {
		if (waited >= GD32_PM_RTC_FLAG_TIMEOUT_US) {
			return -ETIMEDOUT;
		}
		k_busy_wait(GD32_PM_RTC_FLAG_POLL_US);
		waited += GD32_PM_RTC_FLAG_POLL_US;
	}

	return 0;
}

/*
 * Time of day in sub-second units, 1 / (synchronous prescaler + 1) of a
 * second. The shadow registers are refreshed after the calendar has been
 * resynchronized, which is needed after deep-sleep.
 */
static uint32_t gd32_pm_rtc_now(bool resync)
{
	uint32_t sync_prescaler = RTC_PSC & RTC_PSC_FACTOR_S;
	uint32_t subsecond;
	uint32_t time;
	uint32_t seconds;

	if (resync) {
		RTC_WPK = RTC_UNLOCK_KEY1;
		RTC_WPK = RTC_UNLOCK_KEY2;
		RTC_STAT &= ~RTC_STAT_RSYNF;
		(void)gd32_pm_rtc_wait_flag(RTC_STAT_RSYNF);
		RTC_WPK = RTC_LOCK_KEY;
	}

	/* Reading the sub-second or time register freezes the date shadow */
	subsecond = RTC_SS;
	time = RTC_TIME;
	(void)RTC_DATE;

	seconds = (uint32_t)bcd2bin((time >> 16) & 0x3FU) * 3600U +
		  (uint32_t)bcd2bin((time >> 8) & 0x7FU) * 60U + (uint32_t)bcd2bin(time & 0x7FU);

	if (subsecond > sync_prescaler) {
		subsecond = sync_prescaler;
	}

	return seconds * (sync_prescaler + 1U) + (sync_prescaler - subsecond);
}

/* Wait for the console to finish sending, so nothing is cut by the sleep */
static void gd32_pm_console_flush(void)
{
#if DT_HAS_CHOSEN(zephyr_console) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), gd_gd32_usart)
	uint32_t usart = DT_REG_ADDR(DT_CHOSEN(zephyr_console));

	if ((USART_CTL0(usart) & USART_CTL0_UEN) != 0U) {
		while (usart_flag_get(usart, USART_FLAG_TC) == RESET) {
		}
	}
#endif
}

static void gd32_pm_rtc_wakeup_isr(uint8_t line, void *user)
{
	ARG_UNUSED(line);
	ARG_UNUSED(user);

	rtc_flag_clear(RTC_STAT_WTF);
}

static void gd32_pm_deep_sleep(void)
{
	uint64_t now = SysTimer_GetLoadValue();
	uint64_t compare = SysTimer_GetCompareValue();
	uint32_t sync_prescaler = RTC_PSC & RTC_PSC_FACTOR_S;
	uint32_t wut_hz = gd32_pm_rtc_hz() / GD32_PM_WUT_DIV;
	uint64_t wut_ticks;
	uint32_t rtc_before;
	uint32_t rtc_after;
	uint32_t rtc_elapsed;
	uint64_t elapsed_cycles;
	uint64_t awake_cycles;

	/* Nothing to wait for: let the kernel run */
	if (compare <= now) {
		return;
	}

	wut_ticks = ((compare - now) * wut_hz) / CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
	if (wut_ticks == 0U) {
		return;
	}
	if (wut_ticks > GD32_PM_WUT_MAX) {
		wut_ticks = GD32_PM_WUT_MAX;
	}

	gd32_pm_console_flush();

	rtc_before = gd32_pm_rtc_now(false);

	/* The timer counts the reload value plus one clock down to zero */
	(void)rtc_wakeup_disable();
	(void)rtc_wakeup_clock_set(WAKEUP_RTCCK_DIV16);
	(void)rtc_wakeup_timer_set((uint16_t)(wut_ticks - 1U));
	rtc_flag_clear(RTC_STAT_WTF);
	rtc_interrupt_enable(RTC_INT_WAKEUP);
	rtc_wakeup_enable();

	gd32_exti_trigger(GD32_PM_EXTI_LINE_RTC_WAKEUP, GD32_EXTI_TRIG_RISING);
	gd32_exti_enable(GD32_PM_EXTI_LINE_RTC_WAKEUP);

	pmu_to_deepsleepmode(PMU_LDO_LOWPOWER, PMU_LOWDRIVER_ENABLE, WFI_CMD);

	/* Back on the IRC16M: rebuild the clock tree the boot set up */
	if (rcu_system_clock_source_get() != RCU_SCSS_PLLDIG) {
		system_clock_config();
	}
	SysTimer_SetControlValue(SysTimer_GetControlValue() | SysTimer_MTIMECTL_CLKSRC_Msk);

	gd32_exti_disable(GD32_PM_EXTI_LINE_RTC_WAKEUP);
	rtc_interrupt_disable(RTC_INT_WAKEUP);
	(void)rtc_wakeup_disable();
	rtc_flag_clear(RTC_STAT_WTF);

	/* Credit the system timer with the time it did not count */
	rtc_after = gd32_pm_rtc_now(true);
	rtc_elapsed = rtc_after - rtc_before;
	if (rtc_after < rtc_before) {
		rtc_elapsed += GD32_PM_SECONDS_PER_DAY * (sync_prescaler + 1U);
	}
	elapsed_cycles = ((uint64_t)rtc_elapsed * CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC) /
			 (sync_prescaler + 1U);
	awake_cycles = SysTimer_GetLoadValue() - now;
	if (elapsed_cycles > awake_cycles) {
		SysTimer_SetLoadValue(SysTimer_GetLoadValue() + (elapsed_cycles - awake_cycles));
	}
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		gd32_pm_deep_sleep();
	}
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(state);
	ARG_UNUSED(substate_id);
}

/*
 * The wake-up timer needs the RTC clock. The RTC driver, when present, sets
 * it up the same way; the selection is latched in the backup domain, the
 * IRC32K enable is not.
 */
static int gd32_pm_init(void)
{
	uint32_t selected;
	rcu_osci_type_enum osci;

	rcu_periph_clock_enable(RCU_PMU);
	pmu_backup_write_enable();

	selected = RCU_BDCTL & RCU_BDCTL_RTCSRC;
	if ((RCU_BDCTL & RCU_BDCTL_RTCEN) == 0U || selected == RCU_RTCSRC_NONE) {
		osci = RCU_IRC32K;
		selected = RCU_RTCSRC_IRC32K;
	} else {
		osci = (selected == RCU_RTCSRC_LXTAL) ? RCU_LXTAL : RCU_IRC32K;
	}

	rcu_osci_on(osci);
	if (rcu_osci_stab_wait(osci) != SUCCESS) {
		return -EIO;
	}

	if ((RCU_BDCTL & RCU_BDCTL_RTCEN) == 0U) {
		rcu_rtc_clock_config(selected);
		rcu_periph_clock_enable(RCU_RTC);
	}

	return gd32_exti_configure(GD32_PM_EXTI_LINE_RTC_WAKEUP, gd32_pm_rtc_wakeup_isr, NULL);
}

SYS_INIT(gd32_pm_init, PRE_KERNEL_2, 0);
