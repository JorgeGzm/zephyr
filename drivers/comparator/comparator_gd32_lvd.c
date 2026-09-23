/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_lvd

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/comparator.h>
#include <zephyr/drivers/interrupt_controller/gd32_exti.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <gd32_pmu.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(comparator_gd32_lvd, CONFIG_COMPARATOR_LOG_LEVEL);

/*
 * The low voltage detector lives in the PMU: PMU_CTL0 selects the threshold
 * and enables it, PMU_CS0 reports whether VDD is below the threshold, and
 * its output is wired to EXTI line 16 for the interrupt.
 */
#define GD32_PMU_CTL0_OFFSET 0x00U
#define GD32_PMU_CS0_OFFSET  0x04U

#define GD32_LVD_EXTI_LINE 16U

struct comparator_gd32_lvd_config {
	uint32_t reg;
	uint16_t clkid;
	uint8_t threshold_sel;
};

struct comparator_gd32_lvd_data {
	comparator_callback_t callback;
	void *user_data;
	atomic_t triggered;
};

static int comparator_gd32_lvd_get_output(const struct device *dev)
{
	const struct comparator_gd32_lvd_config *cfg = dev->config;

	/* The flag is set while VDD is below the threshold. */
	return (sys_read32(cfg->reg + GD32_PMU_CS0_OFFSET) & PMU_CS0_LVDF) != 0U ? 1 : 0;
}

static int comparator_gd32_lvd_set_trigger(const struct device *dev,
					   enum comparator_trigger trigger)
{
	uint8_t exti_trigger;

	ARG_UNUSED(dev);

	/*
	 * The EXTI line follows the detector output, so a rising edge is
	 * VDD dropping below the threshold and a falling edge is VDD
	 * recovering.
	 */
	switch (trigger) {
	case COMPARATOR_TRIGGER_NONE:
		exti_trigger = GD32_EXTI_TRIG_NONE;
		break;
	case COMPARATOR_TRIGGER_RISING_EDGE:
		exti_trigger = GD32_EXTI_TRIG_RISING;
		break;
	case COMPARATOR_TRIGGER_FALLING_EDGE:
		exti_trigger = GD32_EXTI_TRIG_FALLING;
		break;
	case COMPARATOR_TRIGGER_BOTH_EDGES:
		exti_trigger = GD32_EXTI_TRIG_BOTH;
		break;
	default:
		return -EINVAL;
	}

	if (exti_trigger == GD32_EXTI_TRIG_NONE) {
		gd32_exti_disable(GD32_LVD_EXTI_LINE);
		gd32_exti_trigger(GD32_LVD_EXTI_LINE, GD32_EXTI_TRIG_NONE);
	} else {
		gd32_exti_trigger(GD32_LVD_EXTI_LINE, exti_trigger);
		gd32_exti_enable(GD32_LVD_EXTI_LINE);
	}

	return 0;
}

static int comparator_gd32_lvd_set_trigger_callback(const struct device *dev,
						    comparator_callback_t callback, void *user_data)
{
	struct comparator_gd32_lvd_data *data = dev->data;
	unsigned int key = irq_lock();

	data->callback = callback;
	data->user_data = user_data;

	irq_unlock(key);

	/* A trigger that happened while no callback was set is delivered now. */
	if (callback != NULL && atomic_cas(&data->triggered, 1, 0)) {
		callback(dev, user_data);
	}

	return 0;
}

static int comparator_gd32_lvd_trigger_is_pending(const struct device *dev)
{
	struct comparator_gd32_lvd_data *data = dev->data;

	return atomic_cas(&data->triggered, 1, 0) ? 1 : 0;
}

static void comparator_gd32_lvd_exti_isr(uint8_t line, void *user)
{
	const struct device *dev = user;
	struct comparator_gd32_lvd_data *data = dev->data;

	ARG_UNUSED(line);

	if (data->callback != NULL) {
		data->callback(dev, data->user_data);
	} else {
		atomic_set(&data->triggered, 1);
	}
}

static DEVICE_API(comparator, comparator_gd32_lvd_api) = {
	.get_output = comparator_gd32_lvd_get_output,
	.set_trigger = comparator_gd32_lvd_set_trigger,
	.set_trigger_callback = comparator_gd32_lvd_set_trigger_callback,
	.trigger_is_pending = comparator_gd32_lvd_trigger_is_pending,
};

static int comparator_gd32_lvd_init(const struct device *dev)
{
	const struct comparator_gd32_lvd_config *cfg = dev->config;
	uint32_t ctl0;
	int ret;

	ret = clock_control_on(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid);
	if (ret < 0) {
		LOG_ERR("Failed to enable the PMU clock (%d)", ret);
		return ret;
	}

	ret = gd32_exti_configure(GD32_LVD_EXTI_LINE, comparator_gd32_lvd_exti_isr, (void *)dev);
	if (ret < 0) {
		LOG_ERR("EXTI line %u is taken (%d)", GD32_LVD_EXTI_LINE, ret);
		return ret;
	}

	ctl0 = sys_read32(cfg->reg + GD32_PMU_CTL0_OFFSET);
	ctl0 &= ~PMU_CTL0_LVDT;
	ctl0 |= CTL0_LVDT(cfg->threshold_sel) | PMU_CTL0_LVDEN;
	sys_write32(ctl0, cfg->reg + GD32_PMU_CTL0_OFFSET);

	return 0;
}

#define COMPARATOR_GD32_LVD_INIT(inst)                                                             \
	static const struct comparator_gd32_lvd_config comparator_gd32_lvd_cfg_##inst = {          \
		.reg = DT_INST_REG_ADDR(inst),                                                     \
		.clkid = DT_INST_CLOCKS_CELL(inst, id),                                            \
		.threshold_sel = DT_INST_ENUM_IDX(inst, threshold_mv),                             \
	};                                                                                         \
                                                                                                   \
	static struct comparator_gd32_lvd_data comparator_gd32_lvd_data_##inst;                    \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, comparator_gd32_lvd_init, NULL,                                \
			      &comparator_gd32_lvd_data_##inst, &comparator_gd32_lvd_cfg_##inst,   \
			      POST_KERNEL, CONFIG_COMPARATOR_INIT_PRIORITY,                        \
			      &comparator_gd32_lvd_api);

DT_INST_FOREACH_STATUS_OKAY(COMPARATOR_GD32_LVD_INIT)
