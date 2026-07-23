/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform glue for the GD32VW55x SDK Wi-Fi stack running on Zephyr:
 * console output for dbg_print, the Wi-Fi interrupt attach, newlib/SNTP
 * hooks, and the radio bring-up sequence (gdwifi_start).
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

/* The SDK gd32vw55x.h guards BIT and BITS together: with Zephyr's BIT
 * already defined it would leave BITS undefined.  Let the SDK header
 * define both (same semantics).
 */
#undef BIT
#include <gd32vw55x.h>

LOG_MODULE_REGISTER(gdwifi, CONFIG_WIFI_LOG_LEVEL);

/* Console output used by the SDK debug_print machinery.  The SDK log_uart
 * driver is not compiled; write straight to the console UART by polling.
 * NEVER route this through printk/log: with LOG_PRINTK every character
 * would become one log message and the radio debug chatter (heavy with
 * BLE+coex) saturates the log core and dead-locks the shell backend.
 */

static const struct device *const gdwifi_console =
	DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

void log_uart_putc_noint(char c)
{
	if (gdwifi_console != NULL) {
		if (c == '\n') {
			uart_poll_out(gdwifi_console, '\r');
		}
		uart_poll_out(gdwifi_console, c);
	}
}

void uart_putc_noint(uint32_t uartx, char c)
{
	log_uart_putc_noint(c);
}

/* The prebuilt supplicant tunes stdout buffering at init; neutralize it
 * (linked with -Wl,--wrap=setvbuf).
 */

int __wrap_setvbuf(void *stream, char *buf, int mode, unsigned int size)
{
	return 0;
}

/* SDK stack-depth helper referenced by the wrapper/debug code */

int32_t xGetCurrentTaskStackDepth(unsigned long sp)
{
	return 1024;
}

/*
 * Wi-Fi interrupt wiring.  The SDK gd32vw55x_it.c provides the handler
 * bodies (WIFI_INT_IRQHandler etc.) that call into the prebuilt MAC
 * library.  Attach them to the Zephyr ISR table; the SDK platform code
 * enables and levels the sources in the ECLIC itself.
 */

extern void WIFI_INT_IRQHandler(void);
extern void WIFI_INTGEN_IRQHandler(void);
extern void WIFI_PROT_IRQHandler(void);
extern void WIFI_RX_IRQHandler(void);
extern void WIFI_TX_IRQHandler(void);
extern void LA_IRQHandler(void);
extern void WIFI_WKUP_IRQHandler(void);

static void gdwifi_isr(const void *arg)
{
	((void (*)(void))arg)();
}

/* The radio sources run at ECLIC level 8 (vendor programming): with the
 * 4 intctl bits of this core, Zephyr priority 8 encodes exactly that
 * (0x8F).  They must stay ABOVE the MTH threshold used by
 * sys_enter_critical() (0x10) or the PMU wake handshake dead-locks.
 */

#define GDWIFI_IRQ_LEVEL 8

static void gdwifi_irq_attach(void)
{
	irq_connect_dynamic(WIFI_PROT_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    WIFI_PROT_IRQHandler, 0);
	irq_connect_dynamic(WIFI_INTGEN_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    WIFI_INTGEN_IRQHandler, 0);
	irq_connect_dynamic(WIFI_TX_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    WIFI_TX_IRQHandler, 0);
	irq_connect_dynamic(WIFI_RX_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    WIFI_RX_IRQHandler, 0);
	irq_connect_dynamic(LA_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    LA_IRQHandler, 0);
	irq_connect_dynamic(WIFI_WKUP_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    WIFI_WKUP_IRQHandler, 0);
	irq_connect_dynamic(WIFI_INT_IRQn, GDWIFI_IRQ_LEVEL, gdwifi_isr,
			    WIFI_INT_IRQHandler, 0);
}

/* Radio blob assert handler.  The public V1.0.3g blobs ship with the
 * RivieraWaves debug asserts compiled IN (production RW firmware compiles
 * them OUT and the code paths are written to continue).  The blob's
 * dbg_assert_err spins forever with the MAC task dead; it is weakened at
 * build time (see CMakeLists.txt) so this strong definition overrides it.
 *
 * mm_task.c:2411 guards nxmac_current_state == HW_IDLE before applying a
 * MAC config message (MM_SET_BSSID_REQ et al.).  A blob-internal race
 * activates the HW mid-connect (~1 in 5 back-to-back connects, no upstream
 * fix in V1.0.3g); the disassembly shows the assert call falls through to
 * the normal path, so for this KNOWN assert we log and continue -- release
 * RW behavior.  Anything else stays fatal.
 */

unsigned int g_gdwifi_blob_asserts_ignored;

void dbg_assert_err(const char *condition, const char *file, int line)
{
	if (line == 2411 && file != NULL &&
	    strstr(file, "mm_task") != NULL) {
		g_gdwifi_blob_asserts_ignored++;
		printk("gdwifi: blob assert ignored (%u): (%s) at %s:%d\n",
		       g_gdwifi_blob_asserts_ignored, condition, file, line);
		return;
	}

	printk("gdwifi: FATAL blob assert: (%s) at %s:%d\n",
	       condition, file, line);
	k_panic();
}

/* SNTP hooks referenced through lwipopts.h (the SDK sntp_api.c is not
 * compiled; wifi_net_ip.c only needs the symbols).
 */

void sntp_set_system_time(uint32_t sec)
{
	/* Zephyr owns wall-clock time (SNTP of the SDK is not compiled) */
	LOG_INF("SNTP time hook: %u", sec);
}

uint32_t sntp_get_update_intv(void)
{
	return 86400 * 1000;
}

/* system_clock_config() (used by deep_sleep_exit) comes from the vendor
 * system_gd32vw55x.c already compiled in the HAL module.
 */

/*
 * Radio bring-up: platform subset of the SDK platform_init() (skipping the
 * pieces Zephyr already owns: clocks, ECLIC, console, tick) followed by the
 * Wi-Fi stack start.  Order copied from the vendor demo and the
 * hardware-validated reference port -- it is not negotiable.
 */

extern void sys_os_init(void);
extern void systick_init(void);
extern void rom_init(void);

/* Clock part of the vendor rtc_32k_config() (static in platform.c): the
 * Wi-Fi PMU power state machine runs from the 32K domain and its LPDS
 * handshake stalls without it.  The full RTC calendar setup stays out --
 * Zephyr owns the RTC.
 */

static void gdwifi_32k_clock_config(void)
{
	pmu_backup_write_enable();
	rcu_osci_on(RCU_IRC32K);
	rcu_osci_stab_wait(RCU_IRC32K);
	rcu_rtc_clock_config(RCU_RTCSRC_IRC32K);
	rcu_periph_clock_enable(RCU_RTC);
}
extern void dma_config(void);
extern void sysctrl_init(void);
extern void rf_power_on(void);
extern int wifi_power_on(void);
extern void wifi_power_off(void);
extern void raw_flash_init(void);
extern int nvds_flash_internal_init(void);
extern int wifi_init(void);
extern void sys_wakelock_acquire(uint32_t lock_id);

#define GDWIFI_LOCK_ID_WLAN 0

int gdwifi_start(void)
{
	static bool started;

	if (started) {
		return 0;
	}

#define GDWIFI_STEP(name, call)					\
	do {							\
		LOG_INF("bring-up: " name);			\
		call;						\
	} while (0)

	GDWIFI_STEP("sys_os_init", sys_os_init());
	GDWIFI_STEP("systick_init", systick_init());

	/* Peripheral clocks platform_init() would have enabled before
	 * touching TRNG/CRC/PMU registers (unclocked AHB access bus-hangs).
	 */

	rcu_periph_clock_enable(RCU_PMU);
	rcu_periph_clock_enable(RCU_TRNG);
	rcu_periph_clock_enable(RCU_CRC);

	GDWIFI_STEP("32k_clock_config", gdwifi_32k_clock_config());

	GDWIFI_STEP("rom_init", rom_init());
	GDWIFI_STEP("dma_config", dma_config());
	GDWIFI_STEP("sysctrl_init", sysctrl_init());
	GDWIFI_STEP("rf_power_on", rf_power_on());

	/* Cycle the Wi-Fi power domain: a JTAG/system reset does not clear
	 * the radio PMU state left by a previous run, and the MAC init
	 * asserts on dirty registers.
	 */

	GDWIFI_STEP("wifi_power_off", wifi_power_off());
	GDWIFI_STEP("wifi_power_on", wifi_power_on());
	GDWIFI_STEP("raw_flash_init", raw_flash_init());

	if (nvds_flash_internal_init() != 0) {
		LOG_ERR("nvds flash init failed");
	}

	GDWIFI_STEP("irq_attach", gdwifi_irq_attach());

	/* Hold the WLAN wakelock for good: the LPDS/doze path of the vendor
	 * firmware needs a PMU wake-up this port does not implement (PM
	 * phase).  With the lock held the MAC never enters low-power.
	 */

	sys_wakelock_acquire(GDWIFI_LOCK_ID_WLAN);

	LOG_INF("bring-up: wifi_init");
	if (wifi_init() != 0) {
		LOG_ERR("wifi_init failed");
		return -1;
	}

	LOG_INF("bring-up: wifi_init done");

	started = true;
	return 0;
}
