/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32VW55x BLE 5.3 through the vendor's own BLE host (Plan B).
 *
 * The public prebuilt libble is an all-in-one controller + RivieraWaves
 * host: GAP/GATT/SMP live inside the blob and the controller talks to that
 * host internally, NOT over HCI (ble_register_hci_uart is a stub and
 * h4tl/hci_tl are empty).  The native Zephyr Bluetooth host therefore
 * cannot drive it; this layer starts the vendor host and advertises, the
 * way every GigaDevice BLE example does.  Ported from the
 * hardware-oriented reference gd32_ble.c.
 *
 * Mandatory order (vendor ble_ibeacon example):
 *   ble_power_on() -> ble_sw_init() -> ble_adp_callback_register()
 *   -> irq attach -> ble_irq_enable().
 * Advertising is created from the adapter "enable complete" event and
 * started from the advertising "created" state.
 *
 * Validated vendor HAL: SDK V1.0.3g (2026-04-23, commit 945c6e2).
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "wrapper_os.h"
#include "ble_export.h"
#include "ble_gap.h"
#include "ble_adapter.h"
#include "ble_adv.h"
#include "ble_conn.h"

#include <gd32vw55x.h>

LOG_MODULE_REGISTER(gdble_vendor, CONFIG_WIFI_LOG_LEVEL);

/* SDK wrapper (gdwifi_os.c) */

extern void *sys_malloc(size_t size);
extern void *sys_calloc(size_t count, size_t size);
extern void sys_mfree(void *ptr);
extern void sys_memset(void *s, uint8_t c, uint32_t count);
extern void sys_memcpy(void *des, const void *src, uint32_t n);
extern int32_t sys_memcmp(const void *buf1, const void *buf2, uint32_t count);
extern void *sys_task_create(void *static_tcb, const uint8_t *name,
			     uint32_t *stack_base, uint32_t stack_size,
			     uint32_t queue_size, uint32_t queue_item_size,
			     uint32_t priority, task_func_t func, void *ctx);
extern int sys_task_init_notification(void *task);
extern int sys_task_wait_notification(int timeout);
extern void sys_task_notify(void *task, bool isr);
extern void sys_task_delete(void *task);
extern void sys_ms_sleep(int ms);
extern os_task_t sys_current_task_handle_get(void);
extern int32_t sys_queue_init(os_queue_t *queue, int32_t queue_size,
			      uint32_t item_size);
extern void sys_queue_free(os_queue_t *queue);
extern int sys_queue_write(os_queue_t *queue, void *msg, int timeout,
			   bool isr);
extern int sys_queue_read(os_queue_t *queue, void *msg, int timeout,
			  bool isr);
extern int32_t sys_random_bytes_get(void *dst, uint32_t size);

extern void ble_power_on(void);
extern void ble_irq_enable(void);

/* Coexistence: both sides live in the blobs; only the wiring is ours */

extern void ble_coex_evt_notify_register(void (*cb)(uint32_t, uint32_t,
						    uint32_t));
extern void coex_ble_event_notify(uint32_t evt_start, uint32_t evt_window,
				  uint32_t iso_evt);

/* BLE radio ISR bodies (SDK gd32vw55x_it.c, CFG_BLE_SUPPORT) */

extern void BLE_WKUP_IRQHandler(void);
extern void BLE_POWER_STATUS_IRQHandler(void);
extern void BLE_SW_TRIG_IRQHandler(void);
extern void BLE_FINE_TIMER_TARGET_IRQHandler(void);
extern void BLE_STAMP_TARGET1_IRQHandler(void);
extern void BLE_STAMP_TARGET2_IRQHandler(void);
extern void BLE_STAMP_TARGET3_IRQHandler(void);
extern void BLE_ENCRYPTION_ENGINE_IRQHandler(void);
extern void BLE_SLEEP_MODE_IRQHandler(void);
extern void BLE_HALF_SLOT_IRQHandler(void);
extern void BLE_FIFO_ACTIVITY_IRQHandler(void);
extern void BLE_ERROR_IRQHandler(void);
extern void BLE_FREQ_SELECT_IRQHandler(void);

#define GDBLE_IRQ_LEVEL 8 /* ECLIC level 8, above the MTH threshold */

/* JTAG-readable bring-up state (the GD-Link VCP console may be absent) */

volatile uint32_t g_gdble_dbg_step;      /* 1=sw_init ok 2=irq on */
volatile uint32_t g_gdble_dbg_adp_evt = 0xffffffff;
volatile uint32_t g_gdble_dbg_adv_state = 0xffffffff;
volatile uint32_t g_gdble_dbg_create_st = 0xffffffff;
volatile uint32_t g_gdble_dbg_start_st = 0xffffffff;

#define GDBLE_ADV_NAME     CONFIG_GD32VW55X_BLE_VENDOR_NAME
#define GDBLE_ADV_CH_MAP   0x07 /* channels 37, 38, 39 */
#define GDBLE_ADV_INTV     160  /* 160 * 0.625 ms = 100 ms */

/* BLE stack task parameters (vendor examples; stack size in WORDS) */

#define GDBLE_TASK_STACK      1024
#define GDBLE_TASK_PRIO       OS_TASK_PRIORITY(2)
#define GDBLE_APP_TASK_STACK  1024
#define GDBLE_APP_TASK_PRIO   OS_TASK_PRIORITY(1)

static void gdble_irq_attach(void)
{
	irq_connect_dynamic(BLE_WKUP_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_WKUP_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_POWER_STATUS_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_POWER_STATUS_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_SW_TRIG_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_SW_TRIG_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_FINE_TIMER_TARGET_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_FINE_TIMER_TARGET_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_STAMP_TARGET1_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_STAMP_TARGET1_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_STAMP_TARGET2_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_STAMP_TARGET2_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_STAMP_TARGET3_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_STAMP_TARGET3_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_ENCRYPTION_ENGINE_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_ENCRYPTION_ENGINE_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_SLEEP_MODE_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_SLEEP_MODE_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_HALF_SLOT_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_HALF_SLOT_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_FIFO_ACTIVITY_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_FIFO_ACTIVITY_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_ERROR_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_ERROR_IRQHandler,
			    NULL, 0);
	irq_connect_dynamic(BLE_FREQ_SELECT_IRQn, GDBLE_IRQ_LEVEL,
			    (void (*)(const void *))BLE_FREQ_SELECT_IRQHandler,
			    NULL, 0);
}

/* Advertising: set flags + complete local name and start the set (called
 * once the set has been created).
 */

static void gdble_adv_start(uint8_t adv_idx)
{
	ble_adv_data_set_t adv;
	ble_adv_data_t adv_data;

	memset(&adv, 0, sizeof(adv));
	memset(&adv_data, 0, sizeof(adv_data));

	adv_data.flags = BLE_GAP_ADV_FLAG_LE_ONLY_GENERAL_DISC_MODE;
	adv_data.local_name.type = BLE_ADV_DATA_FULL_NAME;
	adv_data.local_name.p_name = (uint8_t *)GDBLE_ADV_NAME;
	adv_data.local_name.name_len = sizeof(GDBLE_ADV_NAME) - 1;

	adv.data_force = false;
	adv.data.p_data_enc = &adv_data;

	g_gdble_dbg_start_st = ble_adv_start(adv_idx, &adv, NULL, NULL);
}

static uint8_t g_adv_idx = 0xff; /* 0xff = no advertising set yet */

static void gdble_adv_evt_handler(ble_adv_evt_t adv_evt, void *p_data,
				  void *p_context)
{
	ble_adv_state_chg_t *chg;

	if (adv_evt != BLE_ADV_EVT_STATE_CHG) {
		return;
	}

	chg = (ble_adv_state_chg_t *)p_data;
	g_gdble_dbg_adv_state = chg->state;

	if (chg->state == BLE_ADV_STATE_CREATE) {
		g_adv_idx = chg->adv_idx;
		gdble_adv_start(chg->adv_idx);
	} else if (chg->state == BLE_ADV_STATE_START) {
		LOG_INF("BLE advertising '%s'", GDBLE_ADV_NAME);
	}
}

/* A connection stops the legacy advertising set and the stack leaves it
 * stopped, so after the first central connects (or aborts the attempt) the
 * device would never be discoverable again until a reset.  Restart the set
 * on every disconnection.
 */

static void gdble_conn_evt_handler(ble_conn_evt_t event,
				   ble_conn_data_u *p_data)
{
	if (event == BLE_CONN_EVT_STATE_CHG &&
	    p_data->conn_state.state == BLE_CONN_STATE_DISCONNECTD &&
	    g_adv_idx != 0xff) {
		LOG_INF("BLE disconnected (reason 0x%x), restarting advertising",
			p_data->conn_state.info.discon_info.reason);
		ble_adv_restart(g_adv_idx);
	}
}

/* Create a legacy, connectable, general-discoverable advertising set */

static void gdble_adv_create(void)
{
	ble_adv_param_t adv_param;

	memset(&adv_param, 0, sizeof(adv_param));

	adv_param.param.own_addr_type = BLE_GAP_LOCAL_ADDR_STATIC;
	adv_param.param.type = BLE_GAP_ADV_TYPE_LEGACY;
	adv_param.param.prop = BLE_GAP_ADV_PROP_UNDIR_CONN;
	adv_param.param.filter_pol = BLE_GAP_ADV_ALLOW_SCAN_ANY_CON_ANY;
	adv_param.param.disc_mode = BLE_GAP_ADV_MODE_GEN_DISC;
	adv_param.param.primary_phy = BLE_GAP_PHY_1MBPS;
	adv_param.param.ch_map = GDBLE_ADV_CH_MAP;
	adv_param.param.adv_intv_min = GDBLE_ADV_INTV;
	adv_param.param.adv_intv_max = GDBLE_ADV_INTV;

	g_gdble_dbg_create_st = ble_adv_create(&adv_param, gdble_adv_evt_handler,
					       NULL);
}

/* Adapter events: once the stack reports "enable complete" it is safe to
 * create the advertising set.
 */

static void gdble_adp_evt_handler(ble_adp_evt_t event, ble_adp_data_u *p_data)
{
	g_gdble_dbg_adp_evt = event;

	if (event == BLE_ADP_EVT_ENABLE_CMPL_INFO) {
		gdble_adv_create();
	}
}

/* Bring the controller and the vendor host up and start advertising.
 * Called from the radio bring-up thread after gdwifi_start().
 */

int gdble_vendor_start(void)
{
	static ble_os_api_t os_api = {
		.os_malloc = sys_malloc,
		.os_calloc = sys_calloc,
		.os_mfree = sys_mfree,
		.os_memset = sys_memset,
		.os_memcpy = sys_memcpy,
		.os_memcmp = sys_memcmp,
		.os_task_create = sys_task_create,
		.os_task_init_notification = sys_task_init_notification,
		.os_task_wait_notification = sys_task_wait_notification,
		.os_task_notify = sys_task_notify,
		.os_task_delete = sys_task_delete,
		.os_ms_sleep = sys_ms_sleep,
		.os_current_task_handle_get = sys_current_task_handle_get,
		.os_queue_init = sys_queue_init,
		.os_queue_free = sys_queue_free,
		.os_queue_write = sys_queue_write,
		.os_queue_read = sys_queue_read,
		.os_random_bytes_get = sys_random_bytes_get,
	};
	ble_init_param_t param;
	ble_status_t status;

	memset(&param, 0, sizeof(param));

	/* Mandatory order: power on -> ble_sw_init -> callbacks -> IRQs */

	ble_power_on();

	param.role = BLE_GAP_ROLE_PERIPHERAL;
	param.ble_task_stack_size = GDBLE_TASK_STACK;
	param.ble_task_priority = GDBLE_TASK_PRIO;
	param.ble_app_task_stack_size = GDBLE_APP_TASK_STACK;
	param.ble_app_task_priority = GDBLE_APP_TASK_PRIO;
	param.en_cfg = 0;
	param.p_os_api = &os_api;
	param.p_hci_uart_func = NULL; /* internal host, no HCI transport */

	status = ble_sw_init(&param);
	if (status != BLE_ERR_NO_ERROR) {
		LOG_ERR("ble_sw_init failed: 0x%x", status);
		return -EIO;
	}

	g_gdble_dbg_step = 1;
	ble_adp_callback_register(gdble_adp_evt_handler);

	/* Restart advertising when a central disconnects */

	ble_conn_callback_register(gdble_conn_evt_handler);

	/* Attach the radio handlers before enabling the sources */

	gdble_irq_attach();
	ble_irq_enable(); /* only after ble_sw_init */

#if defined(CFG_COEX)
	/* The BLE scheduler (libble) tells the MAC (libwifi) about every
	 * radio window.  Both sides are in the blobs; the registration is
	 * ours.
	 */

	ble_coex_evt_notify_register(coex_ble_event_notify);
#endif

	g_gdble_dbg_step = 2;
	LOG_INF("vendor BLE host started");
	return 0;
}
