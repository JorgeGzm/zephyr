/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GigaDevice GD32VW55x I2C ("v2" controller: TIMINGR / NBYTES / AUTOEND,
 * STM32F0-style). The in-tree i2c_gd32.c targets the classic GD32 I2C
 * (CTL0/CTL1/CKCFG/START/STOP) whose registers do not exist on this SoC.
 *
 * Polling master driver. The transfer sequence mirrors the bare-metal
 * implementation validated on the GD32VW553K-START (SHT3x @ 100 kHz).
 */

#define DT_DRV_COMPAT gd_gd32_i2c_v2

#include <errno.h>

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/i2c.h>

#include <gd32_i2c.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_gd32_v2, CONFIG_I2C_LOG_LEVEL);

#include "i2c-priv.h"

/* Generous polling bound for one byte at >= 100 kHz. */
#define I2C_GD32_V2_TIMEOUT_US   10000U

struct i2c_gd32_v2_config {
	uint32_t reg;
	uint32_t bitrate;
	uint16_t clkid;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
};

struct i2c_gd32_v2_data {
	struct k_sem bus_mutex;
	uint32_t dev_config;
};

/*
 * Build the TIMINGR value for a given I2C input clock and target bitrate.
 * ~50% duty SCL (fine for sensors); prescaler picked to keep the timing
 * counter around 4 MHz so SCLL/SCLH fit their 8-bit fields.
 */
static uint32_t i2c_gd32_v2_timing(uint32_t pclk, uint32_t bitrate)
{
	uint32_t presc;
	uint32_t tclk;
	uint32_t ticks;
	uint32_t scll;
	uint32_t sclh;
	uint32_t scldel = 4U;   /* data setup time (in timing ticks) */
	uint32_t sdadel = 2U;   /* data hold time  (in timing ticks) */

	if (pclk == 0U) {
		pclk = 16000000U;   /* safe fallback: assume 16 MHz */
	}

	/* presc = ceil(pclk / 4 MHz), clamped to the 4-bit field. */
	presc = (pclk + 3999999U) / 4000000U;
	if (presc == 0U) {
		presc = 1U;
	}
	if (presc > 16U) {
		presc = 16U;
	}
	tclk = pclk / presc;

	if (bitrate == 0U) {
		bitrate = I2C_BITRATE_STANDARD;
	}
	ticks = tclk / bitrate;         /* timing ticks per SCL period */
	scll = ticks / 2U;
	sclh = ticks - scll;
	scll = (scll > 0U) ? (scll - 1U) : 0U;
	sclh = (sclh > 0U) ? (sclh - 1U) : 0U;
	if (scll > 255U) {
		scll = 255U;
	}
	if (sclh > 255U) {
		sclh = 255U;
	}

	return ((presc - 1U) << 28) | (scldel << 20) | (sdadel << 16) |
	       (sclh << 8) | scll;
}

static int i2c_gd32_v2_wait(uint32_t reg, uint32_t flag)
{
	uint32_t elapsed = 0U;

	while ((I2C_STAT(reg) & flag) == 0U) {
		if (I2C_STAT(reg) & I2C_STAT_NACK) {
			I2C_STATC(reg) = I2C_STATC_NACKC | I2C_STATC_STPDETC;
			return -EIO;   /* address or data not acknowledged */
		}
		if (++elapsed > I2C_GD32_V2_TIMEOUT_US) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}
	return 0;
}

static int i2c_gd32_v2_wait_idle(uint32_t reg)
{
	uint32_t elapsed = 0U;

	while (I2C_STAT(reg) & I2C_STAT_I2CBSY) {
		if (++elapsed > I2C_GD32_V2_TIMEOUT_US) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}
	return 0;
}

/* One message: START (or repeated START) -> data -> STOP or transfer-complete. */
static int i2c_gd32_v2_msg(uint32_t reg, uint16_t addr,
			   struct i2c_msg *msg, bool do_stop)
{
	bool is_read = (msg->flags & I2C_MSG_READ) != 0U;
	uint32_t ctl1;
	int err;

	/* Clear any stale STOP/NACK from a previous transfer. */
	I2C_STATC(reg) = I2C_STATC_STPDETC | I2C_STATC_NACKC;

	ctl1 = I2C_CTL1(reg);
	ctl1 &= ~(I2C_CTL1_SADDRESS | I2C_CTL1_TRDIR | I2C_CTL1_BYTENUM |
		  I2C_CTL1_RELOAD | I2C_CTL1_AUTOEND | I2C_CTL1_START |
		  I2C_CTL1_STOP);
	ctl1 |= ((uint32_t)addr << 1) & I2C_CTL1_SADDRESS;
	if (is_read) {
		ctl1 |= I2C_CTL1_TRDIR;   /* master receive */
	}
	ctl1 |= ((uint32_t)msg->len << 16) & I2C_CTL1_BYTENUM;
	if (do_stop) {
		ctl1 |= I2C_CTL1_AUTOEND;  /* HW issues STOP after len bytes */
	}
	ctl1 |= I2C_CTL1_START;
	I2C_CTL1(reg) = ctl1;

	for (uint32_t i = 0U; i < msg->len; i++) {
		if (is_read) {
			err = i2c_gd32_v2_wait(reg, I2C_STAT_RBNE);
			if (err < 0) {
				return err;
			}
			msg->buf[i] = (uint8_t)I2C_RDATA(reg);
		} else {
			err = i2c_gd32_v2_wait(reg, I2C_STAT_TI);
			if (err < 0) {
				return err;
			}
			I2C_TDATA(reg) = msg->buf[i];
		}
	}

	if (do_stop) {
		err = i2c_gd32_v2_wait(reg, I2C_STAT_STPDET);
		if (err < 0) {
			return err;
		}
		I2C_STATC(reg) = I2C_STATC_STPDETC;
	} else {
		/* No STOP: wait transfer-complete so the next message can
		 * issue a repeated START.
		 */
		err = i2c_gd32_v2_wait(reg, I2C_STAT_TC);
		if (err < 0) {
			return err;
		}
	}
	return 0;
}

static int i2c_gd32_v2_transfer(const struct device *dev, struct i2c_msg *msgs,
				uint8_t num_msgs, uint16_t addr)
{
	const struct i2c_gd32_v2_config *cfg = dev->config;
	struct i2c_gd32_v2_data *data = dev->data;
	int err;

	if (num_msgs == 0U) {
		return 0;
	}

	k_sem_take(&data->bus_mutex, K_FOREVER);

	err = i2c_gd32_v2_wait_idle(cfg->reg);
	if (err < 0) {
		goto out;
	}

	for (uint8_t i = 0U; i < num_msgs; i++) {
		bool do_stop = ((i == (num_msgs - 1U)) ||
				(msgs[i].flags & I2C_MSG_STOP) != 0U);

		err = i2c_gd32_v2_msg(cfg->reg, addr, &msgs[i], do_stop);
		if (err < 0) {
			/* Force a STOP so the bus is released on error. */
			I2C_CTL1(cfg->reg) |= I2C_CTL1_STOP;
			break;
		}
	}

out:
	k_sem_give(&data->bus_mutex);
	return err;
}

static int i2c_gd32_v2_configure(const struct device *dev, uint32_t dev_config)
{
	const struct i2c_gd32_v2_config *cfg = dev->config;
	struct i2c_gd32_v2_data *data = dev->data;
	uint32_t pclk = 0U;
	uint32_t bitrate;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U) {
		return -ENOTSUP;   /* controller (master) only */
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		bitrate = I2C_BITRATE_STANDARD;
		break;
	case I2C_SPEED_FAST:
		bitrate = I2C_BITRATE_FAST;
		break;
	default:
		return -ENOTSUP;
	}

	(void)clock_control_get_rate(GD32_CLOCK_CONTROLLER,
				     (clock_control_subsys_t)&cfg->clkid, &pclk);

	k_sem_take(&data->bus_mutex, K_FOREVER);

	I2C_CTL0(cfg->reg) &= ~I2C_CTL0_I2CEN;
	I2C_TIMING(cfg->reg) = i2c_gd32_v2_timing(pclk, bitrate);
	I2C_CTL0(cfg->reg) |= I2C_CTL0_I2CEN;

	data->dev_config = dev_config;

	k_sem_give(&data->bus_mutex);
	return 0;
}

static DEVICE_API(i2c, i2c_gd32_v2_driver_api) = {
	.configure = i2c_gd32_v2_configure,
	.transfer = i2c_gd32_v2_transfer,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

static int i2c_gd32_v2_init(const struct device *dev)
{
	const struct i2c_gd32_v2_config *cfg = dev->config;
	struct i2c_gd32_v2_data *data = dev->data;
	uint32_t bitrate_cfg;
	int err;

	err = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		return err;
	}

	k_sem_init(&data->bus_mutex, 1, 1);

	(void)clock_control_on(GD32_CLOCK_CONTROLLER,
			       (clock_control_subsys_t)&cfg->clkid);

	(void)reset_line_toggle_dt(&cfg->reset);

	bitrate_cfg = i2c_map_dt_bitrate(cfg->bitrate);

	return i2c_gd32_v2_configure(dev, I2C_MODE_CONTROLLER | bitrate_cfg);
}

#define I2C_GD32_V2_INIT(inst)							\
	PINCTRL_DT_INST_DEFINE(inst);						\
	static struct i2c_gd32_v2_data i2c_gd32_v2_data_##inst;			\
	const static struct i2c_gd32_v2_config i2c_gd32_v2_cfg_##inst = {	\
		.reg = DT_INST_REG_ADDR(inst),					\
		.bitrate = DT_INST_PROP(inst, clock_frequency),			\
		.clkid = DT_INST_CLOCKS_CELL(inst, id),				\
		.reset = RESET_DT_SPEC_INST_GET(inst),				\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),			\
	};									\
	I2C_DEVICE_DT_INST_DEFINE(inst,						\
				  i2c_gd32_v2_init, NULL,			\
				  &i2c_gd32_v2_data_##inst,			\
				  &i2c_gd32_v2_cfg_##inst,			\
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,	\
				  &i2c_gd32_v2_driver_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_GD32_V2_INIT)
