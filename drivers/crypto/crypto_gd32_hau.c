/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_hau

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <gd32_hau.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(crypto_gd32_hau, CONFIG_CRYPTO_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "The HAU HAL drives a single unit");

#define CRYPTO_GD32_HAU_CAPS (CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS)

/* Polling bound for the digest of the last block, in loop iterations */
#define CRYPTO_GD32_HAU_TIMEOUT 0x10000U

struct crypto_gd32_hau_config {
	uint16_t clkid;
	struct reset_dt_spec reset;
};

struct crypto_gd32_hau_data {
	struct k_sem device_sem;
	struct k_sem session_sem;
};

struct crypto_gd32_hau_session {
	uint32_t algo;
	uint8_t digest_words;
	bool in_use;
};

static struct crypto_gd32_hau_session crypto_gd32_hau_sessions[CONFIG_CRYPTO_GD32_HAU_MAX_SESSIONS];

/*
 * Feed the whole message and read the digest. The unit swaps the bytes of
 * each input word, so the words are built from the bytes in memory order;
 * the length of the last word is told to the unit apart. The digest comes
 * out as big-endian words.
 */
static int crypto_gd32_hau_digest(const struct crypto_gd32_hau_session *session, const uint8_t *in,
				  size_t len, uint8_t *out)
{
	hau_init_parameter_struct init = {
		.algo = session->algo,
		.mode = HAU_MODE_HASH,
		.datatype = HAU_SWAPPING_8BIT,
		.keytype = 0U,
	};
	hau_digest_parameter_struct digest;
	size_t tail = len % sizeof(uint32_t);
	uint32_t i;

	hau_init(&init);
	hau_last_word_validbits_num_config(8U * tail);

	for (i = 0U; i + sizeof(uint32_t) <= len; i += sizeof(uint32_t)) {
		hau_data_write(sys_get_le32(in + i));
	}

	if (tail != 0U) {
		uint8_t last[sizeof(uint32_t)] = {0};

		memcpy(last, in + i, tail);
		hau_data_write(sys_get_le32(last));
	}

	hau_digest_calculation_enable();

	for (i = 0U; i < CRYPTO_GD32_HAU_TIMEOUT; i++) {
		if (hau_flag_get(HAU_FLAG_BUSY) == RESET) {
			break;
		}
	}
	if (i == CRYPTO_GD32_HAU_TIMEOUT) {
		return -ETIMEDOUT;
	}

	hau_digest_read(&digest);
	for (i = 0U; i < session->digest_words; i++) {
		sys_put_be32(digest.out[i], out + i * sizeof(uint32_t));
	}

	return 0;
}

static int crypto_gd32_hau_handler(struct hash_ctx *ctx, struct hash_pkt *pkt, bool finish)
{
	struct crypto_gd32_hau_data *data = ctx->device->data;
	const struct crypto_gd32_hau_session *session = ctx->drv_sessn_state;
	int ret;

	if (pkt->out_buf == NULL || (pkt->in_buf == NULL && pkt->in_len > 0U)) {
		return -EINVAL;
	}

	/* The unit keeps no state between calls: one message per call */
	if (!finish) {
		return -ENOTSUP;
	}

	k_sem_take(&data->device_sem, K_FOREVER);

	ret = crypto_gd32_hau_digest(session, pkt->in_buf, pkt->in_len, pkt->out_buf);

	k_sem_give(&data->device_sem);

	if (ret != 0) {
		LOG_ERR("The digest did not finish (%d)", ret);
	}

	return ret;
}

static int crypto_gd32_hau_get_session(const struct device *dev)
{
	struct crypto_gd32_hau_data *data = dev->data;
	int idx = -1;

	k_sem_take(&data->session_sem, K_FOREVER);

	for (int i = 0; i < CONFIG_CRYPTO_GD32_HAU_MAX_SESSIONS; i++) {
		if (!crypto_gd32_hau_sessions[i].in_use) {
			crypto_gd32_hau_sessions[i].in_use = true;
			idx = i;
			break;
		}
	}

	k_sem_give(&data->session_sem);

	return idx;
}

static int crypto_gd32_hau_begin_session(const struct device *dev, struct hash_ctx *ctx,
					 enum hash_algo algo)
{
	struct crypto_gd32_hau_session *session;
	uint32_t hau_algo;
	uint8_t digest_words;
	int idx;

	if ((ctx->flags & ~CRYPTO_GD32_HAU_CAPS) != 0U) {
		LOG_ERR("Unsupported flags 0x%x", ctx->flags);
		return -ENOTSUP;
	}

	switch (algo) {
	case CRYPTO_HASH_ALGO_SHA224:
		hau_algo = HAU_ALGO_SHA224;
		digest_words = 7U;
		break;
	case CRYPTO_HASH_ALGO_SHA256:
		hau_algo = HAU_ALGO_SHA256;
		digest_words = 8U;
		break;
	default:
		LOG_ERR("Unsupported algorithm %d", algo);
		return -ENOTSUP;
	}

	idx = crypto_gd32_hau_get_session(dev);
	if (idx < 0) {
		LOG_ERR("No free session");
		return -ENOSPC;
	}
	session = &crypto_gd32_hau_sessions[idx];

	session->algo = hau_algo;
	session->digest_words = digest_words;

	ctx->drv_sessn_state = session;
	ctx->hash_hndlr = crypto_gd32_hau_handler;
	ctx->started = false;

	return 0;
}

static int crypto_gd32_hau_free_session(const struct device *dev, struct hash_ctx *ctx)
{
	struct crypto_gd32_hau_session *session = ctx->drv_sessn_state;

	ARG_UNUSED(dev);

	if (session == NULL) {
		return -EINVAL;
	}

	memset(session, 0, sizeof(*session));
	ctx->drv_sessn_state = NULL;

	return 0;
}

static int crypto_gd32_hau_query_caps(const struct device *dev)
{
	ARG_UNUSED(dev);

	return CRYPTO_GD32_HAU_CAPS;
}

static DEVICE_API(crypto, crypto_gd32_hau_api) = {
	.query_hw_caps = crypto_gd32_hau_query_caps,
	.hash_begin_session = crypto_gd32_hau_begin_session,
	.hash_free_session = crypto_gd32_hau_free_session,
};

static int crypto_gd32_hau_init(const struct device *dev)
{
	const struct crypto_gd32_hau_config *cfg = dev->config;
	struct crypto_gd32_hau_data *data = dev->data;
	int ret;

	ret = clock_control_on(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid);
	if (ret < 0) {
		LOG_ERR("Failed to enable the HAU clock (%d)", ret);
		return ret;
	}

	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret < 0) {
		LOG_ERR("Failed to reset the HAU (%d)", ret);
		return ret;
	}

	k_sem_init(&data->device_sem, 1, 1);
	k_sem_init(&data->session_sem, 1, 1);

	return 0;
}

static const struct crypto_gd32_hau_config crypto_gd32_hau_cfg = {
	.clkid = DT_INST_CLOCKS_CELL(0, id),
	.reset = RESET_DT_SPEC_INST_GET(0),
};

static struct crypto_gd32_hau_data crypto_gd32_hau_data;

DEVICE_DT_INST_DEFINE(0, crypto_gd32_hau_init, NULL, &crypto_gd32_hau_data, &crypto_gd32_hau_cfg,
		      POST_KERNEL, CONFIG_CRYPTO_INIT_PRIORITY, &crypto_gd32_hau_api);
