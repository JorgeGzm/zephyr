/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_crc

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(crc_gd32, CONFIG_CRC_LOG_LEVEL);

/*
 * The unit is a CRC-32 engine with the polynomial 0x04C11DB7 and the
 * initial value 0xFFFFFFFF both fixed. It only takes 32-bit words, which
 * it shifts in MSB first, and has no input or output reversal.
 */
#define GD32_CRC_DATA_OFFSET 0x00U
#define GD32_CRC_CTL_OFFSET  0x08U

#define GD32_CRC_CTL_RST BIT(0)

#define GD32_CRC_POLY     0x04C11DB7U
#define GD32_CRC_INIT_VAL 0xFFFFFFFFU

struct crc_gd32_config {
	uint32_t reg;
	uint16_t clkid;
	struct reset_dt_spec reset;
};

struct crc_gd32_data {
	struct k_sem dev_lock;
	/* Bytes of an incomplete word, waiting for the next update or finish */
	uint8_t pending[sizeof(uint32_t)];
	uint8_t pending_len;
	bool reflect;
};

/*
 * Reverse the bits of each byte of a word, all four at once. Register-only
 * on purpose: a lookup table would live in flash, and on this SoC every
 * table access costs a flash read, which is slower than the CRC unit
 * itself.
 */
static inline uint32_t crc_gd32_rev8x4(uint32_t word)
{
	word = ((word & 0xF0F0F0F0U) >> 4) | ((word & 0x0F0F0F0FU) << 4);
	word = ((word & 0xCCCCCCCCU) >> 2) | ((word & 0x33333333U) << 2);
	word = ((word & 0xAAAAAAAAU) >> 1) | ((word & 0x55555555U) << 1);

	return word;
}

static inline uint8_t crc_gd32_rev8(uint8_t byte)
{
	return (uint8_t)crc_gd32_rev8x4(byte);
}

static inline uint32_t crc_gd32_rev32(uint32_t word)
{
	return BSWAP_32(crc_gd32_rev8x4(word));
}

/*
 * A reflected CRC (CRC32_IEEE) over a byte stream equals the bit-reversed
 * result of the MSB-first engine fed with each input byte bit-reversed,
 * so that variant only costs a byte reversal on the way in and a word
 * reversal on the way out.
 */
/* The engine consumes the word MSB first: the first byte goes on top. */
static void crc_gd32_feed_word(const struct crc_gd32_config *cfg, const struct crc_gd32_data *data,
			       const uint8_t *bytes)
{
	uint32_t word = sys_get_be32(bytes);

	if (data->reflect) {
		word = crc_gd32_rev8x4(word);
	}

	sys_write32(word, cfg->reg + GD32_CRC_DATA_OFFSET);
}

/* MSB-first CRC-32 step for the bytes that do not fill a word. */
static uint32_t crc_gd32_step_byte(uint32_t crc, uint8_t byte)
{
	crc ^= (uint32_t)byte << 24;
	for (int i = 0; i < 8; i++) {
		crc = (crc & BIT(31)) ? ((crc << 1) ^ GD32_CRC_POLY) : (crc << 1);
	}

	return crc;
}

/*
 * The initial value is fixed, but the engine can still be brought to any
 * state: feeding one word runs 32 shift steps on (state ^ word), and since
 * the polynomial is odd each step can be undone (its bit 0 tells whether
 * the polynomial was applied). Undo 32 steps from the wanted state to find
 * the word that leads there from the reset value.
 */
static uint32_t crc_gd32_preload_word(uint32_t wanted)
{
	uint32_t crc = wanted;

	for (int i = 0; i < 32; i++) {
		crc = (crc & BIT(0)) ? (((crc ^ GD32_CRC_POLY) >> 1) | BIT(31)) : (crc >> 1);
	}

	return crc ^ GD32_CRC_INIT_VAL;
}

static void crc_gd32_release(const struct device *dev, struct crc_ctx *ctx)
{
	struct crc_gd32_data *data = dev->data;

	ctx->state = CRC_STATE_IDLE;
	k_sem_give(&data->dev_lock);
}

static int crc_gd32_begin(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_gd32_config *cfg = dev->config;
	struct crc_gd32_data *data = dev->data;
	uint32_t state;
	bool reflect;

	switch (ctx->type) {
	case CRC32_MPEG2:
		reflect = false;
		break;
	case CRC32_IEEE:
		reflect = true;
		break;
	default:
		return -ENOTSUP;
	}

	/* The polynomial and the reflection are fixed by the hardware. */
	if (ctx->polynomial != GD32_CRC_POLY) {
		return -ENOTSUP;
	}
	if ((ctx->reversed != 0U) != reflect) {
		return -ENOTSUP;
	}
	if (reflect && ctx->reversed != (CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT)) {
		return -ENOTSUP;
	}

	/*
	 * The seed is the state to continue from. For the reflected variant it
	 * is given in the reflected domain (the complement of the previous
	 * result), so bring it back to the engine's bit order.
	 */
	state = reflect ? crc_gd32_rev32(ctx->seed) : ctx->seed;

	k_sem_take(&data->dev_lock, K_FOREVER);

	data->reflect = reflect;
	data->pending_len = 0U;
	sys_write32(GD32_CRC_CTL_RST, cfg->reg + GD32_CRC_CTL_OFFSET);
	if (state != GD32_CRC_INIT_VAL) {
		sys_write32(crc_gd32_preload_word(state), cfg->reg + GD32_CRC_DATA_OFFSET);
	}

	ctx->state = CRC_STATE_IN_PROGRESS;

	return 0;
}

static int crc_gd32_update(const struct device *dev, struct crc_ctx *ctx, const void *buffer,
			   size_t bufsize)
{
	const struct crc_gd32_config *cfg = dev->config;
	struct crc_gd32_data *data = dev->data;
	const uint8_t *buf = buffer;

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	/* Complete a word left over by the previous update first. */
	while (data->pending_len != 0U && bufsize > 0U) {
		data->pending[data->pending_len++] = *buf++;
		bufsize--;

		if (data->pending_len == sizeof(data->pending)) {
			crc_gd32_feed_word(cfg, data, data->pending);
			data->pending_len = 0U;
		}
	}

	/* Then feed whole words straight from the buffer. */
	while (bufsize >= sizeof(uint32_t)) {
		crc_gd32_feed_word(cfg, data, buf);
		buf += sizeof(uint32_t);
		bufsize -= sizeof(uint32_t);
	}

	/* And keep the rest for the next update or for finish. */
	while (bufsize > 0U) {
		data->pending[data->pending_len++] = *buf++;
		bufsize--;
	}

	return 0;
}

static int crc_gd32_finish(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_gd32_config *cfg = dev->config;
	struct crc_gd32_data *data = dev->data;
	uint32_t crc;

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	crc = sys_read32(cfg->reg + GD32_CRC_DATA_OFFSET);

	/* The engine cannot take a partial word: finish the tail in software. */
	for (uint8_t i = 0U; i < data->pending_len; i++) {
		uint8_t byte = data->reflect ? crc_gd32_rev8(data->pending[i]) : data->pending[i];

		crc = crc_gd32_step_byte(crc, byte);
	}

	if (data->reflect) {
		crc = crc_gd32_rev32(crc) ^ 0xFFFFFFFFU;
	}

	ctx->result = crc;

	crc_gd32_release(dev, ctx);

	return 0;
}

static DEVICE_API(crc, crc_gd32_driver_api) = {
	.begin = crc_gd32_begin,
	.update = crc_gd32_update,
	.finish = crc_gd32_finish,
};

static int crc_gd32_init(const struct device *dev)
{
	const struct crc_gd32_config *cfg = dev->config;
	struct crc_gd32_data *data = dev->data;
	int ret;

	ret = clock_control_on(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid);
	if (ret < 0) {
		LOG_ERR("Failed to enable the CRC clock (%d)", ret);
		return ret;
	}

	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret < 0) {
		LOG_ERR("Failed to reset the CRC unit (%d)", ret);
		return ret;
	}

	k_sem_init(&data->dev_lock, 1, 1);

	return 0;
}

#define CRC_GD32_INIT(inst)                                                                        \
	static const struct crc_gd32_config crc_gd32_cfg_##inst = {                                \
		.reg = DT_INST_REG_ADDR(inst),                                                     \
		.clkid = DT_INST_CLOCKS_CELL(inst, id),                                            \
		.reset = RESET_DT_SPEC_INST_GET(inst),                                             \
	};                                                                                         \
                                                                                                   \
	static struct crc_gd32_data crc_gd32_data_##inst;                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, crc_gd32_init, NULL, &crc_gd32_data_##inst,                    \
			      &crc_gd32_cfg_##inst, POST_KERNEL, CONFIG_CRC_DRIVER_INIT_PRIORITY,  \
			      &crc_gd32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CRC_GD32_INIT)
