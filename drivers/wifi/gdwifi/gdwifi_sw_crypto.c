/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Software stand-ins for the HAL crypto helpers that libwpas.a calls.
 *
 * The prebuilt WPA library was linked against the CAU and HAU HAL helpers,
 * and cau_deinit() among them resets the CAU. Those engines belong to the
 * Zephyr crypto drivers, so the CMake file redirects the library's
 * references to these functions, which keep the HAL signatures and run the
 * same primitives on the SDK mbedTLS copy built into this driver.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <gd32vw55x_cau.h>

#define GDWIFI_AES_BLOCK 16U

void gdwifi_sw_cau_deinit(void)
{
}

void gdwifi_sw_cau_struct_para_init(cau_parameter_struct *param)
{
	memset(param, 0, sizeof(*param));
	param->alg_dir = CAU_ENCRYPT;
}

ErrStatus gdwifi_sw_cau_aes_ecb(cau_parameter_struct *param, uint8_t *output)
{
	mbedtls_aes_context aes;
	const uint8_t *in = param->input;
	uint32_t left = param->in_length;
	int mode;
	int ret;

	mbedtls_aes_init(&aes);

	if (param->alg_dir == CAU_DECRYPT) {
		mode = MBEDTLS_AES_DECRYPT;
		ret = mbedtls_aes_setkey_dec(&aes, param->key, param->key_size);
	} else {
		mode = MBEDTLS_AES_ENCRYPT;
		ret = mbedtls_aes_setkey_enc(&aes, param->key, param->key_size);
	}

	/* The HAL zero-pads a short last block and writes a whole block out */
	while (ret == 0 && left > 0U) {
		uint8_t block[GDWIFI_AES_BLOCK] = {0};
		uint32_t chunk = (left < GDWIFI_AES_BLOCK) ? left : GDWIFI_AES_BLOCK;

		memcpy(block, in, chunk);
		ret = mbedtls_aes_crypt_ecb(&aes, mode, block, output);

		in += chunk;
		output += GDWIFI_AES_BLOCK;
		left -= chunk;
	}

	mbedtls_aes_free(&aes);

	return (ret == 0) ? SUCCESS : ERROR;
}

ErrStatus gdwifi_sw_hau_hash_md5(uint8_t *input, uint32_t in_length, uint8_t output[16])
{
	return (mbedtls_md5(input, in_length, output) == 0) ? SUCCESS : ERROR;
}

ErrStatus gdwifi_sw_hau_hash_sha_1(uint8_t *input, uint32_t in_length, uint8_t output[20])
{
	return (mbedtls_sha1(input, in_length, output) == 0) ? SUCCESS : ERROR;
}

ErrStatus gdwifi_sw_hau_hash_sha_256(uint8_t *input, uint32_t in_length, uint8_t output[32])
{
	return (mbedtls_sha256(input, in_length, output, 0) == 0) ? SUCCESS : ERROR;
}

/*
 * HMAC on stack contexts: mbedtls_md_hmac() would allocate its context from
 * the heap, and the library calls these helpers from a critical section.
 */
#define GDWIFI_HMAC_BLOCK 64U

union gdwifi_sw_hash_ctx {
	mbedtls_md5_context md5;
	mbedtls_sha1_context sha1;
	mbedtls_sha256_context sha256;
};

struct gdwifi_sw_hash {
	size_t digest_len;
	void (*init)(union gdwifi_sw_hash_ctx *ctx);
	void (*free)(union gdwifi_sw_hash_ctx *ctx);
	int (*starts)(union gdwifi_sw_hash_ctx *ctx);
	int (*update)(union gdwifi_sw_hash_ctx *ctx, const uint8_t *in, size_t len);
	int (*finish)(union gdwifi_sw_hash_ctx *ctx, uint8_t *out);
};

static void md5_init(union gdwifi_sw_hash_ctx *c)
{
	mbedtls_md5_init(&c->md5);
}
static void md5_free(union gdwifi_sw_hash_ctx *c)
{
	mbedtls_md5_free(&c->md5);
}
static int md5_starts(union gdwifi_sw_hash_ctx *c)
{
	return mbedtls_md5_starts(&c->md5);
}
static int md5_update(union gdwifi_sw_hash_ctx *c, const uint8_t *in, size_t len)
{
	return mbedtls_md5_update(&c->md5, in, len);
}
static int md5_finish(union gdwifi_sw_hash_ctx *c, uint8_t *out)
{
	return mbedtls_md5_finish(&c->md5, out);
}

static void sha1_init(union gdwifi_sw_hash_ctx *c)
{
	mbedtls_sha1_init(&c->sha1);
}
static void sha1_free(union gdwifi_sw_hash_ctx *c)
{
	mbedtls_sha1_free(&c->sha1);
}
static int sha1_starts(union gdwifi_sw_hash_ctx *c)
{
	return mbedtls_sha1_starts(&c->sha1);
}
static int sha1_update(union gdwifi_sw_hash_ctx *c, const uint8_t *in, size_t len)
{
	return mbedtls_sha1_update(&c->sha1, in, len);
}
static int sha1_finish(union gdwifi_sw_hash_ctx *c, uint8_t *out)
{
	return mbedtls_sha1_finish(&c->sha1, out);
}

static void sha256_init(union gdwifi_sw_hash_ctx *c)
{
	mbedtls_sha256_init(&c->sha256);
}
static void sha256_free(union gdwifi_sw_hash_ctx *c)
{
	mbedtls_sha256_free(&c->sha256);
}
static int sha256_starts(union gdwifi_sw_hash_ctx *c)
{
	return mbedtls_sha256_starts(&c->sha256, 0);
}
static int sha256_update(union gdwifi_sw_hash_ctx *c, const uint8_t *in, size_t len)
{
	return mbedtls_sha256_update(&c->sha256, in, len);
}
static int sha256_finish(union gdwifi_sw_hash_ctx *c, uint8_t *out)
{
	return mbedtls_sha256_finish(&c->sha256, out);
}

static const struct gdwifi_sw_hash gdwifi_sw_md5 = {16U,        md5_init,   md5_free,
						    md5_starts, md5_update, md5_finish};
static const struct gdwifi_sw_hash gdwifi_sw_sha1 = {20U,         sha1_init,   sha1_free,
						     sha1_starts, sha1_update, sha1_finish};
static const struct gdwifi_sw_hash gdwifi_sw_sha256 = {32U,           sha256_init,   sha256_free,
						       sha256_starts, sha256_update, sha256_finish};

static ErrStatus gdwifi_sw_hmac(const struct gdwifi_sw_hash *h, const uint8_t *key,
				uint32_t keysize, const uint8_t *input, uint32_t in_length,
				uint8_t *output)
{
	union gdwifi_sw_hash_ctx ctx;
	uint8_t pad[GDWIFI_HMAC_BLOCK];
	uint8_t inner[32];
	int ret;

	h->init(&ctx);

	/* A key longer than the block is replaced by its digest */
	memset(pad, 0, sizeof(pad));
	if (keysize > GDWIFI_HMAC_BLOCK) {
		ret = h->starts(&ctx);
		if (ret == 0) {
			ret = h->update(&ctx, key, keysize);
		}
		if (ret == 0) {
			ret = h->finish(&ctx, pad);
		}
	} else {
		memcpy(pad, key, keysize);
		ret = 0;
	}

	for (size_t i = 0U; i < sizeof(pad); i++) {
		pad[i] ^= 0x36U;
	}
	if (ret == 0) {
		ret = h->starts(&ctx);
	}
	if (ret == 0) {
		ret = h->update(&ctx, pad, sizeof(pad));
	}
	if (ret == 0) {
		ret = h->update(&ctx, input, in_length);
	}
	if (ret == 0) {
		ret = h->finish(&ctx, inner);
	}

	for (size_t i = 0U; i < sizeof(pad); i++) {
		pad[i] ^= 0x36U ^ 0x5cU;
	}
	if (ret == 0) {
		ret = h->starts(&ctx);
	}
	if (ret == 0) {
		ret = h->update(&ctx, pad, sizeof(pad));
	}
	if (ret == 0) {
		ret = h->update(&ctx, inner, h->digest_len);
	}
	if (ret == 0) {
		ret = h->finish(&ctx, output);
	}

	h->free(&ctx);
	memset(pad, 0, sizeof(pad));
	memset(inner, 0, sizeof(inner));

	return (ret == 0) ? SUCCESS : ERROR;
}

ErrStatus gdwifi_sw_hau_hmac_md5(uint8_t *key, uint32_t keysize, uint8_t *input, uint32_t in_length,
				 uint8_t output[16])
{
	return gdwifi_sw_hmac(&gdwifi_sw_md5, key, keysize, input, in_length, output);
}

ErrStatus gdwifi_sw_hau_hmac_sha_1(uint8_t *key, uint32_t keysize, uint8_t *input,
				   uint32_t in_length, uint8_t output[20])
{
	return gdwifi_sw_hmac(&gdwifi_sw_sha1, key, keysize, input, in_length, output);
}

ErrStatus gdwifi_sw_hau_hmac_sha_256(uint8_t *key, uint32_t keysize, uint8_t *input,
				     uint32_t in_length, uint8_t output[32])
{
	return gdwifi_sw_hmac(&gdwifi_sw_sha256, key, keysize, input, in_length, output);
}
