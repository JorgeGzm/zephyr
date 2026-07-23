/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth HCI driver for the GD32VW55x: the Zephyr BT host talks HCI to
 * the vendor BLE controller (libble blob) through the SDK "virtual HCI"
 * transport (virtual_hci.c, compiled from source with
 * CFG_VIRTUAL_HCI_MODE): a RAM uart emulation over cyclic buffers that the
 * controller drives via the p_hci_uart_func hook of ble_sw_init().
 *
 * Same model as the Espressif hci_esp32.c driver (VHCI), so all the
 * standard Zephyr bluetooth samples run with the native host.
 *
 * Bring-up mirrors the vendor ble_init.c CFG_VIRTUAL_HCI_MODE path plus
 * the hardware-validated reference port lessons:
 *  - all 13 BLE interrupt handler bodies (SDK gd32vw55x_it.c) must be
 *    attached BEFORE ble_irq_enable() or the first radio IRQ panics;
 *  - the radio sources keep ECLIC level 8 (above the sys_enter_critical
 *    MTH threshold);
 *  - with Wi-Fi active the coexistence callback must be registered.
 *
 * Validated vendor HAL: SDK V1.0.3g (2026-04-23, commit 945c6e2).
 */

#define DT_DRV_COMPAT gd_gd32vw55x_bt_hci

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/irq.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "wrapper_os.h"
#include "ble_export.h"
#include "comm_hci.h"
#include "virtual_hci.h"

#include <gd32vw55x.h>

LOG_MODULE_REGISTER(bt_hci_gdwifi, CONFIG_BT_HCI_DRIVER_LOG_LEVEL);

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

/* SDK BLE platform pieces */

extern void ble_power_on(void);
extern void ble_irq_enable(void);
extern int gdwifi_start(void);

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

struct gdble_data {
	const struct device *dev;
	bool started;
};

static struct gdble_data gdble;

/*
 * Controller -> host: virtual HCI receive callbacks.  They run on the
 * vir_hci task; the payload stays in the transport cyclic buffer until we
 * peek it (virtual_hci_get_payload); the transport drops it when the
 * callback returns.
 */

static void gdble_handle_event(uint8_t *p_header, uint16_t payload_length)
{
	struct net_buf *buf;
	bool discardable = p_header[0] == BT_HCI_EVT_LE_META_EVENT ? false :
			   p_header[0] == BT_HCI_EVT_CMD_COMPLETE ? false :
			   p_header[0] == BT_HCI_EVT_CMD_STATUS ? false : false;

	buf = bt_buf_get_evt(p_header[0], discardable, K_FOREVER);
	if (buf == NULL) {
		LOG_ERR("no evt buf (evt 0x%02x)", p_header[0]);
		return;
	}

	net_buf_add_mem(buf, p_header, HCI_EVT_HDR_LEN);

	if (payload_length > 0) {
		if (!virtual_hci_get_payload(net_buf_add(buf, payload_length),
					     payload_length)) {
			LOG_ERR("evt payload read failed");
			net_buf_unref(buf);
			return;
		}
	}

	bt_hci_recv(gdble.dev, buf);
}

static void gdble_handle_acl(uint8_t *p_header, uint16_t payload_length)
{
	struct net_buf *buf;

	buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
	if (buf == NULL) {
		LOG_ERR("no acl buf");
		return;
	}

	net_buf_add_mem(buf, p_header, HCI_ACL_HDR_LEN);

	if (payload_length > 0) {
		if (!virtual_hci_get_payload(net_buf_add(buf, payload_length),
					     payload_length)) {
			LOG_ERR("acl payload read failed");
			net_buf_unref(buf);
			return;
		}
	}

	bt_hci_recv(gdble.dev, buf);
}

static void gdble_handle_iso(uint8_t *p_header, uint16_t payload_length)
{
	/* ISO not enabled in the host configuration: drop */
}

static void gdble_handle_sco(uint8_t *p_header, uint16_t payload_length)
{
}

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

static int gdble_open(const struct device *dev)
{
	static const hci_recv_callback_t recv_cb = {
		gdble_handle_event,
		gdble_handle_acl,
		gdble_handle_iso,
		gdble_handle_sco,
	};
	static ble_os_api_t os_interface = {
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
	ble_init_param_t param = {0};
	ble_status_t ret;

	gdble.dev = dev;

	if (gdble.started) {
		return 0;
	}

	/* The radio platform (clocks, RF, NVDS, OS wrapper) comes up with
	 * the Wi-Fi bring-up; BLE depends on it.
	 */

	if (gdwifi_start() != 0) {
		return -EIO;
	}

	ble_power_on();

	param.role = BLE_GAP_ROLE_ALL;
	param.ble_task_stack_size = 768; /* words, vendor default */
	param.ble_task_priority = OS_TASK_PRIORITY(2);
	param.en_cfg = 0;
	param.p_os_api = &os_interface;

	if (!virtual_hci_init(recv_cb, &param.p_hci_uart_func)) {
		LOG_ERR("virtual HCI init failed");
		return -EIO;
	}

	ret = ble_sw_init(&param);
	if (ret != BLE_ERR_NO_ERROR) {
		LOG_ERR("ble_sw_init failed: 0x%x", ret);
		return -EIO;
	}

#if defined(CFG_COEX)
	ble_coex_evt_notify_register(coex_ble_event_notify);
#endif

	/* Attach ALL the handlers ble_irq_enable() will unmask BEFORE
	 * enabling them (and keep them referenced against --gc-sections).
	 */

	gdble_irq_attach();
	ble_irq_enable();

	gdble.started = true;
	LOG_INF("GD32VW55x BLE controller up (virtual HCI)");
	return 0;
}

static int gdble_send(const struct device *dev, struct net_buf *buf)
{
	uint8_t type = buf->data[0];
	uint8_t *pkt = buf->data + 1; /* strip the H4 type */
	int err = 0;

	switch (type) {
	case HCI_CMD_MSG_TYPE: {
		uint16_t opcode = sys_get_le16(pkt);
		uint8_t len = pkt[2];

		if (!virtual_hci_send_command(opcode, len,
					      len > 0 ? &pkt[3] : NULL)) {
			err = -ENOBUFS;
		}
		break;
	}
	case HCI_ACL_MSG_TYPE: {
		uint16_t hdl_flags = sys_get_le16(pkt);
		uint16_t len = sys_get_le16(&pkt[2]);

		if (!virtual_hci_send_acl_data(hdl_flags, len,
					       len > 0 ? &pkt[4] : NULL)) {
			err = -ENOBUFS;
		}
		break;
	}
	case HCI_ISO_MSG_TYPE: {
		uint16_t hdl_flags = sys_get_le16(pkt);
		uint16_t len = sys_get_le16(&pkt[2]);

		if (!virtual_hci_send_iso_data(hdl_flags, len,
					       len > 0 ? &pkt[4] : NULL)) {
			err = -ENOBUFS;
		}
		break;
	}
	default:
		LOG_ERR("unknown H4 type %u", type);
		err = -EINVAL;
		break;
	}

	if (err == 0) {
		net_buf_unref(buf);
	}

	return err;
}

static int gdble_close(const struct device *dev)
{
	/* The vendor stack has a deinit path but powering the radio down
	 * under an active Wi-Fi coexistence has not been validated: keep
	 * the controller up (matches other always-on VHCI drivers).
	 */

	return 0;
}

static DEVICE_API(bt_hci, gdble_drv) = {
	.open = gdble_open,
	.send = gdble_send,
	.close = gdble_close,
};

#define GDBLE_DEVICE_INIT(inst)                                                \
	static struct bt_hci_driver_data gdble_hci_data_##inst = {};           \
	static const struct bt_hci_driver_config gdble_config_##inst =         \
		BT_DT_HCI_DRIVER_CONFIG_INST_GET(inst);                        \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &gdble_hci_data_##inst,        \
			      &gdble_config_##inst, POST_KERNEL,               \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &gdble_drv)

GDBLE_DEVICE_INIT(0);
