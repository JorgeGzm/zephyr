/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gd_gd32_cau

#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <gd32_cau.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(crypto_gd32_cau, CONFIG_CRYPTO_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "The CAU HAL drives a single unit");

#define CRYPTO_GD32_CAU_CAPS (CAP_RAW_KEY | CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS | CAP_NO_IV_PREFIX)

#define CRYPTO_GD32_CAU_BLOCK_SIZE  16U
#define CRYPTO_GD32_CAU_BLOCK_WORDS (CRYPTO_GD32_CAU_BLOCK_SIZE / sizeof(uint32_t))
#define CRYPTO_GD32_CAU_KEY_WORDS   (32U / sizeof(uint32_t))

/* Polling bound for one block or one key schedule, in loop iterations */
#define CRYPTO_GD32_CAU_TIMEOUT 0x10000U

struct crypto_gd32_cau_config {
	uint16_t clkid;
	struct reset_dt_spec reset;
};

struct crypto_gd32_cau_data {
	struct k_sem device_sem;
	struct k_sem session_sem;
};

struct crypto_gd32_cau_session {
	/* Key as the engine takes it: big-endian words, right-aligned */
	uint32_t key[CRYPTO_GD32_CAU_KEY_WORDS];
	uint32_t key_size;
	uint32_t mode;
	uint32_t dir;
	bool in_use;
};

static struct crypto_gd32_cau_session crypto_gd32_cau_sessions[CONFIG_CRYPTO_GD32_CAU_MAX_SESSIONS];

static int crypto_gd32_cau_wait(uint32_t flag, FlagStatus wanted)
{
	for (uint32_t i = 0U; i < CRYPTO_GD32_CAU_TIMEOUT; i++) {
		if (cau_flag_get(flag) == wanted) {
			return 0;
		}
	}

	return -ETIMEDOUT;
}

/*
 * Load the key and, for the modes that run the cipher backwards, derive the
 * decryption key schedule; then configure the mode, the IV and start the
 * engine. The IV is given as the four big-endian words of the block.
 */
static int crypto_gd32_cau_start(const struct crypto_gd32_cau_session *session, const uint32_t *iv)
{
	cau_key_parameter_struct key = {
		.key_0_high = session->key[0],
		.key_0_low = session->key[1],
		.key_1_high = session->key[2],
		.key_1_low = session->key[3],
		.key_2_high = session->key[4],
		.key_2_low = session->key[5],
		.key_3_high = session->key[6],
		.key_3_low = session->key[7],
	};
	int ret;

	cau_disable();
	cau_aes_keysize_config(session->key_size);
	cau_key_init(&key);

	if (session->dir == CAU_DECRYPT &&
	    (session->mode == CAU_MODE_AES_ECB || session->mode == CAU_MODE_AES_CBC)) {
		cau_fifo_flush();
		cau_init(CAU_DECRYPT, CAU_MODE_AES_KEY, CAU_SWAPPING_32BIT);
		cau_enable();

		ret = crypto_gd32_cau_wait(CAU_FLAG_BUSY, RESET);
		if (ret != 0) {
			cau_disable();
			return ret;
		}
	}

	cau_init(session->dir, session->mode, CAU_SWAPPING_8BIT);

	if (iv != NULL) {
		cau_iv_parameter_struct ivp = {
			.iv_0_high = iv[0],
			.iv_0_low = iv[1],
			.iv_1_high = iv[2],
			.iv_1_low = iv[3],
		};

		cau_iv_init(&ivp);
	}

	cau_fifo_flush();
	cau_enable();

	return 0;
}

/*
 * Push the data through the running engine one block at a time. A short
 * last block is zero-padded on the way in and cut on the way out, which is
 * what the stream modes need; the block modes reject such lengths earlier.
 * Bytes go through the byte-order helpers, so the buffers need no alignment.
 */
static int crypto_gd32_cau_run(const uint8_t *in, uint8_t *out, size_t len)
{
	uint8_t block[CRYPTO_GD32_CAU_BLOCK_SIZE];
	int ret;

	while (len > 0U) {
		size_t chunk = MIN(len, CRYPTO_GD32_CAU_BLOCK_SIZE);
		const uint8_t *src = in;

		if (chunk < CRYPTO_GD32_CAU_BLOCK_SIZE) {
			memset(block, 0, sizeof(block));
			memcpy(block, in, chunk);
			src = block;
		}

		ret = crypto_gd32_cau_wait(CAU_FLAG_INFIFO_EMPTY, SET);
		if (ret != 0) {
			return ret;
		}

		for (size_t i = 0U; i < CRYPTO_GD32_CAU_BLOCK_WORDS; i++) {
			cau_data_write(sys_get_le32(src + i * sizeof(uint32_t)));
		}

		ret = crypto_gd32_cau_wait(CAU_FLAG_BUSY, RESET);
		if (ret != 0) {
			return ret;
		}

		if (chunk < CRYPTO_GD32_CAU_BLOCK_SIZE) {
			for (size_t i = 0U; i < CRYPTO_GD32_CAU_BLOCK_WORDS; i++) {
				sys_put_le32(cau_data_read(), block + i * sizeof(uint32_t));
			}
			memcpy(out, block, chunk);
		} else {
			for (size_t i = 0U; i < CRYPTO_GD32_CAU_BLOCK_WORDS; i++) {
				sys_put_le32(cau_data_read(), out + i * sizeof(uint32_t));
			}
		}

		in += chunk;
		out += chunk;
		len -= chunk;
	}

	return 0;
}

static int crypto_gd32_cau_process(struct cipher_ctx *ctx, const uint32_t *iv, const uint8_t *in,
				   uint8_t *out, size_t len)
{
	struct crypto_gd32_cau_data *data = ctx->device->data;
	const struct crypto_gd32_cau_session *session = ctx->drv_sessn_state;
	int ret;

	k_sem_take(&data->device_sem, K_FOREVER);

	ret = crypto_gd32_cau_start(session, iv);
	if (ret == 0) {
		ret = crypto_gd32_cau_run(in, out, len);
	}

	cau_disable();

	k_sem_give(&data->device_sem);

	if (ret != 0) {
		LOG_ERR("The engine did not finish (%d)", ret);
	}

	return ret;
}

static int crypto_gd32_cau_check_pkt(const struct cipher_pkt *pkt, bool whole_blocks,
				     size_t out_len)
{
	if (pkt->in_buf == NULL || pkt->out_buf == NULL || pkt->in_len < 0) {
		return -EINVAL;
	}

	if (whole_blocks && ((size_t)pkt->in_len % CRYPTO_GD32_CAU_BLOCK_SIZE) != 0U) {
		LOG_ERR("%d bytes is not a whole number of blocks", pkt->in_len);
		return -EINVAL;
	}

	if ((size_t)pkt->out_buf_max < out_len) {
		LOG_ERR("Output buffer too small");
		return -ENOMEM;
	}

	return 0;
}

static void crypto_gd32_cau_iv_words(const uint8_t *iv, uint32_t *words)
{
	for (size_t i = 0U; i < CRYPTO_GD32_CAU_BLOCK_WORDS; i++) {
		words[i] = sys_get_be32(iv + i * sizeof(uint32_t));
	}
}

static int crypto_gd32_cau_ecb_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt)
{
	int ret;

	ret = crypto_gd32_cau_check_pkt(pkt, true, (size_t)pkt->in_len);
	if (ret != 0) {
		return ret;
	}

	ret = crypto_gd32_cau_process(ctx, NULL, pkt->in_buf, pkt->out_buf, pkt->in_len);
	if (ret == 0) {
		pkt->out_len = pkt->in_len;
	}

	return ret;
}

/*
 * Unless the session asks for CAP_NO_IV_PREFIX, the IV is prefixed to the
 * ciphertext: it is written ahead of the output when encrypting and read
 * from the head of the input when decrypting.
 */
static int crypto_gd32_cau_cbc_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	const struct crypto_gd32_cau_session *session = ctx->drv_sessn_state;
	bool prefix = (ctx->flags & CAP_NO_IV_PREFIX) == 0U;
	uint32_t words[CRYPTO_GD32_CAU_BLOCK_WORDS];
	const uint8_t *in = pkt->in_buf;
	uint8_t *out = pkt->out_buf;
	size_t len;
	int ret;

	if (session->dir == CAU_ENCRYPT) {
		len = (size_t)pkt->in_len;
		ret = crypto_gd32_cau_check_pkt(pkt, true,
						prefix ? len + CRYPTO_GD32_CAU_BLOCK_SIZE : len);
		if (ret != 0) {
			return ret;
		}

		if (prefix) {
			memcpy(out, iv, CRYPTO_GD32_CAU_BLOCK_SIZE);
			out += CRYPTO_GD32_CAU_BLOCK_SIZE;
		}
	} else {
		if (prefix && pkt->in_len < (int)CRYPTO_GD32_CAU_BLOCK_SIZE) {
			return -EINVAL;
		}

		len = (size_t)pkt->in_len;
		if (prefix) {
			in += CRYPTO_GD32_CAU_BLOCK_SIZE;
			len -= CRYPTO_GD32_CAU_BLOCK_SIZE;
		}

		ret = crypto_gd32_cau_check_pkt(pkt, true, len);
		if (ret != 0) {
			return ret;
		}
	}

	crypto_gd32_cau_iv_words(iv, words);

	ret = crypto_gd32_cau_process(ctx, words, in, out, len);
	if (ret == 0) {
		pkt->out_len = (session->dir == CAU_ENCRYPT && prefix)
				       ? (int)(len + CRYPTO_GD32_CAU_BLOCK_SIZE)
				       : (int)len;
	}

	return ret;
}

/* CFB and OFB take a full-block IV and any length, like CBC without prefix */
static int crypto_gd32_cau_stream_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	uint32_t words[CRYPTO_GD32_CAU_BLOCK_WORDS];
	int ret;

	ret = crypto_gd32_cau_check_pkt(pkt, false, (size_t)pkt->in_len);
	if (ret != 0) {
		return ret;
	}

	crypto_gd32_cau_iv_words(iv, words);

	ret = crypto_gd32_cau_process(ctx, words, pkt->in_buf, pkt->out_buf, pkt->in_len);
	if (ret == 0) {
		pkt->out_len = pkt->in_len;
	}

	return ret;
}

/*
 * The counter block is the nonce followed by a zero counter of ctr_len bits;
 * the engine increments the low word of the block after each block.
 */
static int crypto_gd32_cau_ctr_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *nonce)
{
	uint8_t block[CRYPTO_GD32_CAU_BLOCK_SIZE] = {0};
	uint32_t words[CRYPTO_GD32_CAU_BLOCK_WORDS];
	uint32_t ctr_len = ctx->mode_params.ctr_info.ctr_len;
	int ret;

	if (ctr_len == 0U || ctr_len > 32U || (ctr_len % 8U) != 0U) {
		LOG_ERR("Unsupported counter length %u", ctr_len);
		return -EINVAL;
	}

	ret = crypto_gd32_cau_check_pkt(pkt, false, (size_t)pkt->in_len);
	if (ret != 0) {
		return ret;
	}

	memcpy(block, nonce, CRYPTO_GD32_CAU_BLOCK_SIZE - ctr_len / 8U);
	crypto_gd32_cau_iv_words(block, words);

	ret = crypto_gd32_cau_process(ctx, words, pkt->in_buf, pkt->out_buf, pkt->in_len);
	if (ret == 0) {
		pkt->out_len = pkt->in_len;
	}

	return ret;
}

static int crypto_gd32_cau_get_session(const struct device *dev)
{
	struct crypto_gd32_cau_data *data = dev->data;
	int idx = -1;

	k_sem_take(&data->session_sem, K_FOREVER);

	for (int i = 0; i < CONFIG_CRYPTO_GD32_CAU_MAX_SESSIONS; i++) {
		if (!crypto_gd32_cau_sessions[i].in_use) {
			crypto_gd32_cau_sessions[i].in_use = true;
			idx = i;
			break;
		}
	}

	k_sem_give(&data->session_sem);

	return idx;
}

static int crypto_gd32_cau_begin_session(const struct device *dev, struct cipher_ctx *ctx,
					 enum cipher_algo algo, enum cipher_mode mode,
					 enum cipher_op op_type)
{
	struct crypto_gd32_cau_session *session;
	size_t key_words;
	int idx;

	if ((ctx->flags & ~CRYPTO_GD32_CAU_CAPS) != 0U) {
		LOG_ERR("Unsupported flags 0x%x", ctx->flags);
		return -ENOTSUP;
	}

	if (algo != CRYPTO_CIPHER_ALGO_AES) {
		LOG_ERR("Unsupported algorithm %d", algo);
		return -ENOTSUP;
	}

	if (ctx->keylen != 16U && ctx->keylen != 24U && ctx->keylen != 32U) {
		LOG_ERR("Unsupported key size %u", ctx->keylen);
		return -ENOTSUP;
	}

	if (ctx->key.bit_stream == NULL) {
		return -EINVAL;
	}

	idx = crypto_gd32_cau_get_session(dev);
	if (idx < 0) {
		LOG_ERR("No free session");
		return -ENOSPC;
	}
	session = &crypto_gd32_cau_sessions[idx];

	switch (mode) {
	case CRYPTO_CIPHER_MODE_ECB:
		session->mode = CAU_MODE_AES_ECB;
		ctx->ops.block_crypt_hndlr = crypto_gd32_cau_ecb_op;
		break;
	case CRYPTO_CIPHER_MODE_CBC:
		session->mode = CAU_MODE_AES_CBC;
		ctx->ops.cbc_crypt_hndlr = crypto_gd32_cau_cbc_op;
		break;
	case CRYPTO_CIPHER_MODE_CTR:
		session->mode = CAU_MODE_AES_CTR;
		ctx->ops.ctr_crypt_hndlr = crypto_gd32_cau_ctr_op;
		break;
	case CRYPTO_CIPHER_MODE_CFB:
		session->mode = CAU_MODE_AES_CFB;
		ctx->ops.cfb_crypt_hndlr = crypto_gd32_cau_stream_op;
		break;
	case CRYPTO_CIPHER_MODE_OFB:
		session->mode = CAU_MODE_AES_OFB;
		ctx->ops.ofb_crypt_hndlr = crypto_gd32_cau_stream_op;
		break;
	default:
		LOG_ERR("Unsupported mode %d", mode);
		session->in_use = false;
		return -ENOTSUP;
	}

	switch (ctx->keylen) {
	case 16U:
		session->key_size = CAU_KEYSIZE_128BIT;
		break;
	case 24U:
		session->key_size = CAU_KEYSIZE_192BIT;
		break;
	default:
		session->key_size = CAU_KEYSIZE_256BIT;
		break;
	}

	/* Shorter keys occupy the low words of the key registers */
	memset(session->key, 0, sizeof(session->key));
	key_words = ctx->keylen / sizeof(uint32_t);
	for (size_t i = 0U; i < key_words; i++) {
		session->key[CRYPTO_GD32_CAU_KEY_WORDS - key_words + i] =
			sys_get_be32(ctx->key.bit_stream + i * sizeof(uint32_t));
	}

	session->dir = (op_type == CRYPTO_CIPHER_OP_ENCRYPT) ? CAU_ENCRYPT : CAU_DECRYPT;

	ctx->drv_sessn_state = session;

	return 0;
}

static int crypto_gd32_cau_free_session(const struct device *dev, struct cipher_ctx *ctx)
{
	struct crypto_gd32_cau_session *session = ctx->drv_sessn_state;

	ARG_UNUSED(dev);

	if (session == NULL) {
		return -EINVAL;
	}

	memset(session, 0, sizeof(*session));
	ctx->drv_sessn_state = NULL;

	return 0;
}

static int crypto_gd32_cau_query_caps(const struct device *dev)
{
	ARG_UNUSED(dev);

	return CRYPTO_GD32_CAU_CAPS;
}

static DEVICE_API(crypto, crypto_gd32_cau_api) = {
	.query_hw_caps = crypto_gd32_cau_query_caps,
	.cipher_begin_session = crypto_gd32_cau_begin_session,
	.cipher_free_session = crypto_gd32_cau_free_session,
};

static int crypto_gd32_cau_init(const struct device *dev)
{
	const struct crypto_gd32_cau_config *cfg = dev->config;
	struct crypto_gd32_cau_data *data = dev->data;
	int ret;

	ret = clock_control_on(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid);
	if (ret < 0) {
		LOG_ERR("Failed to enable the CAU clock (%d)", ret);
		return ret;
	}

	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret < 0) {
		LOG_ERR("Failed to reset the CAU (%d)", ret);
		return ret;
	}

	k_sem_init(&data->device_sem, 1, 1);
	k_sem_init(&data->session_sem, 1, 1);

	return 0;
}

static const struct crypto_gd32_cau_config crypto_gd32_cau_cfg = {
	.clkid = DT_INST_CLOCKS_CELL(0, id),
	.reset = RESET_DT_SPEC_INST_GET(0),
};

static struct crypto_gd32_cau_data crypto_gd32_cau_data;

DEVICE_DT_INST_DEFINE(0, crypto_gd32_cau_init, NULL, &crypto_gd32_cau_data, &crypto_gd32_cau_cfg,
		      POST_KERNEL, CONFIG_CRYPTO_INIT_PRIORITY, &crypto_gd32_cau_api);
