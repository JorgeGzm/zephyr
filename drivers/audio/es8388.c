/*
 * Copyright (c) 2026 GZM Embarcados
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Everest Semiconductor ES8388 stereo audio codec.
 *
 * Zephyr has no driver for this part. The register writes below follow the
 * ESP-ADF driver (components/audio_hal/driver/es8388, Apache-2.0), which is the
 * sequence the silicon vendor's own boards use; the comments say what each one
 * is for, because the datasheet's register descriptions do not always make the
 * intent obvious.
 *
 * The codec is an I2S slave: the SoC supplies MCLK, BCLK and LRCK. Only
 * playback is implemented - the ES8388's ADC is unused on the board this was
 * written for, where capture comes from a separate ES7210.
 */

#define DT_DRV_COMPAT everest_es8388

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(es8388, CONFIG_AUDIO_CODEC_LOG_LEVEL);

/* Register map. Names follow the datasheet. */
#define ES8388_CONTROL1     0x00
#define ES8388_CONTROL2     0x01
#define ES8388_CHIPPOWER    0x02
#define ES8388_ADCPOWER     0x03
#define ES8388_DACPOWER     0x04
#define ES8388_MASTERMODE   0x08
#define ES8388_DACCONTROL1  0x17
#define ES8388_DACCONTROL2  0x18
#define ES8388_DACCONTROL3  0x19
#define ES8388_DACCONTROL4  0x1a /* LDACVOL */
#define ES8388_DACCONTROL5  0x1b /* RDACVOL */
#define ES8388_DACCONTROL16 0x26
#define ES8388_DACCONTROL17 0x27
#define ES8388_DACCONTROL20 0x2a
#define ES8388_DACCONTROL21 0x2b
#define ES8388_DACCONTROL23 0x2d
#define ES8388_DACCONTROL24 0x2e /* LOUT1VOL */
#define ES8388_DACCONTROL25 0x2f /* ROUT1VOL */
#define ES8388_DACCONTROL26 0x30 /* LOUT2VOL */
#define ES8388_DACCONTROL27 0x31 /* ROUT2VOL */

/* DACPOWER output enables */
#define ES8388_DACPOWER_ROUT1 BIT(2)
#define ES8388_DACPOWER_LOUT1 BIT(3)
#define ES8388_DACPOWER_ROUT2 BIT(4)
#define ES8388_DACPOWER_LOUT2 BIT(5)

/* DACCONTROL3: mute is bit 2 */
#define ES8388_DACCONTROL3_MUTE BIT(2)

/* DACCONTROL1: serial format in bits 2:1, word length in bits 5:3 */
#define ES8388_FMT_I2S              0
#define ES8388_FMT_LEFT_JUSTIFIED   1
#define ES8388_FMT_RIGHT_JUSTIFIED  2
#define ES8388_FMT_PCM              3

#define ES8388_WORD_LEN_24 0
#define ES8388_WORD_LEN_20 1
#define ES8388_WORD_LEN_18 2
#define ES8388_WORD_LEN_16 3
#define ES8388_WORD_LEN_32 4

/* LDACVOL/RDACVOL are attenuation in 0.5 dB steps: 0 is 0 dB, 192 is -96 dB. */
#define ES8388_DAC_VOL_MAX_ATTEN 192

/* LOUT/ROUT analogue volume, 0..0x21. 0x1e is 0 dB, which is what the vendor
 * driver leaves them at.
 */
#define ES8388_OUT_VOL_0DB 0x1e

enum es8388_dac_output {
	ES8388_DAC_OUTPUT_LOUT1_ROUT1,
	ES8388_DAC_OUTPUT_LOUT2_ROUT2,
	ES8388_DAC_OUTPUT_BOTH,
};

struct es8388_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec spk_en;
	enum es8388_dac_output dac_output;
};

struct es8388_data {
	int out_volume;  /* dB, <= 0 */
	bool out_muted;
};

static int es8388_write(const struct device *dev, uint8_t reg, uint8_t val)
{
	const struct es8388_config *cfg = dev->config;
	int ret = i2c_reg_write_byte_dt(&cfg->i2c, reg, val);

	if (ret < 0) {
		LOG_ERR("write reg 0x%02x = 0x%02x failed (%d)", reg, val, ret);
	}
	return ret;
}

static int es8388_read(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct es8388_config *cfg = dev->config;
	int ret = i2c_reg_read_byte_dt(&cfg->i2c, reg, val);

	if (ret < 0) {
		LOG_ERR("read reg 0x%02x failed (%d)", reg, ret);
	}
	return ret;
}

static int es8388_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t cur;
	int ret = es8388_read(dev, reg, &cur);

	if (ret < 0) {
		return ret;
	}
	return es8388_write(dev, reg, (cur & ~mask) | (val & mask));
}

static uint8_t es8388_dacpower_mask(enum es8388_dac_output out)
{
	switch (out) {
	case ES8388_DAC_OUTPUT_LOUT1_ROUT1:
		return ES8388_DACPOWER_LOUT1 | ES8388_DACPOWER_ROUT1;
	case ES8388_DAC_OUTPUT_LOUT2_ROUT2:
		return ES8388_DACPOWER_LOUT2 | ES8388_DACPOWER_ROUT2;
	default:
		return ES8388_DACPOWER_LOUT1 | ES8388_DACPOWER_ROUT1 |
		       ES8388_DACPOWER_LOUT2 | ES8388_DACPOWER_ROUT2;
	}
}

static int es8388_set_volume(const struct device *dev, int db)
{
	int atten;

	/* The register attenuates in half-decibel steps and cannot amplify, so
	 * a positive request is clamped to unity rather than refused.
	 */
	db = MIN(db, 0);
	atten = MIN(-db * 2, ES8388_DAC_VOL_MAX_ATTEN);

	return es8388_write(dev, ES8388_DACCONTROL4, (uint8_t)atten) ||
	       es8388_write(dev, ES8388_DACCONTROL5, (uint8_t)atten);
}

static int es8388_set_mute(const struct device *dev, bool mute)
{
	return es8388_update(dev, ES8388_DACCONTROL3, ES8388_DACCONTROL3_MUTE,
			     mute ? ES8388_DACCONTROL3_MUTE : 0);
}

static int es8388_set_format(const struct device *dev, audio_dai_type_t dai_type,
			     uint8_t word_size)
{
	uint8_t fmt;
	uint8_t len;

	switch (dai_type) {
	case AUDIO_DAI_TYPE_I2S:
		fmt = ES8388_FMT_I2S;
		break;
	case AUDIO_DAI_TYPE_LEFT_JUSTIFIED:
		fmt = ES8388_FMT_LEFT_JUSTIFIED;
		break;
	case AUDIO_DAI_TYPE_RIGHT_JUSTIFIED:
		fmt = ES8388_FMT_RIGHT_JUSTIFIED;
		break;
	default:
		LOG_ERR("unsupported DAI type %d", dai_type);
		return -EINVAL;
	}

	switch (word_size) {
	case 16:
		len = ES8388_WORD_LEN_16;
		break;
	case 18:
		len = ES8388_WORD_LEN_18;
		break;
	case 20:
		len = ES8388_WORD_LEN_20;
		break;
	case 24:
		len = ES8388_WORD_LEN_24;
		break;
	case 32:
		len = ES8388_WORD_LEN_32;
		break;
	default:
		LOG_ERR("unsupported word size %u", word_size);
		return -EINVAL;
	}

	return es8388_update(dev, ES8388_DACCONTROL1, GENMASK(5, 1),
			     (len << 3) | (fmt << 1));
}

static int es8388_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	const struct es8388_config *dev_cfg = dev->config;
	struct es8388_data *data = dev->data;
	int ret;

	if (cfg->dai_route == AUDIO_ROUTE_CAPTURE ||
	    cfg->dai_route == AUDIO_ROUTE_PLAYBACK_CAPTURE) {
		LOG_ERR("capture is not implemented");
		return -ENOTSUP;
	}

	/* Mute first: everything below changes clocking and routing, and an
	 * unmuted DAC makes that audible.
	 */
	ret = es8388_write(dev, ES8388_DACCONTROL3, ES8388_DACCONTROL3_MUTE);
	ret |= es8388_write(dev, ES8388_CONTROL2, 0x50);
	ret |= es8388_write(dev, ES8388_CHIPPOWER, 0x00); /* everything powered */

	/* Bypass the internal DLL. Without this the codec misbehaves at low
	 * sample rates; the vendor driver notes 8 kHz specifically.
	 */
	ret |= es8388_write(dev, 0x35, 0xa0);
	ret |= es8388_write(dev, 0x37, 0xd0);
	ret |= es8388_write(dev, 0x39, 0xd0);

	ret |= es8388_write(dev, ES8388_MASTERMODE, 0x00); /* slave */

	/* Outputs off while the DAC path is set up. */
	ret |= es8388_write(dev, ES8388_DACPOWER, 0xc0);
	ret |= es8388_write(dev, ES8388_CONTROL1, 0x12);
	ret |= es8388_write(dev, ES8388_DACCONTROL2, 0x02); /* single speed, 256 fs */
	ret |= es8388_write(dev, ES8388_DACCONTROL16, 0x00);
	ret |= es8388_write(dev, ES8388_DACCONTROL17, 0x90); /* left DAC to left mixer, 0 dB */
	ret |= es8388_write(dev, ES8388_DACCONTROL20, 0x90); /* right DAC to right mixer, 0 dB */
	ret |= es8388_write(dev, ES8388_DACCONTROL21, 0x80); /* ADC and DAC share one LRCK */
	ret |= es8388_write(dev, ES8388_DACCONTROL23, 0x00);

	/* Analogue output volumes at 0 dB; digital volume does the attenuating. */
	ret |= es8388_write(dev, ES8388_DACCONTROL24, ES8388_OUT_VOL_0DB);
	ret |= es8388_write(dev, ES8388_DACCONTROL25, ES8388_OUT_VOL_0DB);
	ret |= es8388_write(dev, ES8388_DACCONTROL26, ES8388_OUT_VOL_0DB);
	ret |= es8388_write(dev, ES8388_DACCONTROL27, ES8388_OUT_VOL_0DB);

	/* The ADC is unused here, and left powered it both draws current and
	 * couples noise into the shared analogue supply.
	 */
	ret |= es8388_write(dev, ES8388_ADCPOWER, 0xff);

	if (ret < 0) {
		return -EIO;
	}

	ret = es8388_set_format(dev, cfg->dai_type, cfg->dai_cfg.i2s.word_size);
	if (ret < 0) {
		return ret;
	}

	ret = es8388_set_volume(dev, data->out_volume);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("configured: %u Hz, %u bit, MCLK %u Hz", cfg->dai_cfg.i2s.frame_clk_freq,
		cfg->dai_cfg.i2s.word_size, cfg->mclk_freq);
	return es8388_write(dev, ES8388_DACPOWER,
			    es8388_dacpower_mask(dev_cfg->dac_output));
}

static void es8388_start_output(const struct device *dev)
{
	const struct es8388_config *cfg = dev->config;
	struct es8388_data *data = dev->data;

	es8388_write(dev, ES8388_DACCONTROL21, 0x80);
	es8388_write(dev, ES8388_DACPOWER, es8388_dacpower_mask(cfg->dac_output));
	es8388_set_mute(dev, data->out_muted);

	/* Amplifier last, so it does not reproduce the DAC settling. */
	if (cfg->spk_en.port != NULL) {
		gpio_pin_set_dt(&cfg->spk_en, 1);
	}
}

static void es8388_stop_output(const struct device *dev)
{
	const struct es8388_config *cfg = dev->config;

	if (cfg->spk_en.port != NULL) {
		gpio_pin_set_dt(&cfg->spk_en, 0);
	}

	es8388_set_mute(dev, true);
	es8388_write(dev, ES8388_DACPOWER, 0x00);
}

static int es8388_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	struct es8388_data *data = dev->data;

	/* The two DAC channels are attenuated by one pair of registers written
	 * together, so a per-channel request cannot be honoured as asked.
	 */
	if (channel != AUDIO_CHANNEL_ALL) {
		return -EINVAL;
	}

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		data->out_volume = val.vol;
		return 0;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		data->out_muted = val.mute;
		return 0;
	default:
		return -EINVAL;
	}
}

static int es8388_apply_properties(const struct device *dev)
{
	struct es8388_data *data = dev->data;
	int ret = es8388_set_volume(dev, data->out_volume);

	if (ret < 0) {
		return ret;
	}
	return es8388_set_mute(dev, data->out_muted);
}

static int es8388_init(const struct device *dev)
{
	const struct es8388_config *cfg = dev->config;
	uint8_t val;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	/* No ID register on this part, so the presence check is that the chip
	 * acknowledges and its power register reads back what was written.
	 */
	ret = es8388_write(dev, ES8388_CHIPPOWER, 0xff);
	if (ret < 0) {
		return -ENODEV;
	}
	ret = es8388_read(dev, ES8388_CHIPPOWER, &val);
	if (ret < 0) {
		return -ENODEV;
	}
	if (val != 0xff) {
		LOG_ERR("ES8388 not responding as expected (CHIPPOWER = 0x%02x)", val);
		return -ENODEV;
	}

	if (cfg->spk_en.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->spk_en)) {
			LOG_ERR("speaker enable GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->spk_en, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("speaker enable GPIO config failed (%d)", ret);
			return ret;
		}
	}

	LOG_INF("ES8388 at 0x%02x", cfg->i2c.addr);
	return 0;
}

static DEVICE_API(audio_codec, es8388_driver_api) = {
	.configure = es8388_configure,
	.start_output = es8388_start_output,
	.stop_output = es8388_stop_output,
	.set_property = es8388_set_property,
	.apply_properties = es8388_apply_properties,
};

#define ES8388_DAC_OUTPUT(n)                                                                       \
	UTIL_CAT(ES8388_DAC_OUTPUT_,                                                               \
		 DT_INST_STRING_UPPER_TOKEN_OR(n, dac_output, BOTH))

#define ES8388_INIT(n)                                                                             \
	static struct es8388_data es8388_data_##n = {                                              \
		.out_volume = 0,                                                                   \
		.out_muted = false,                                                                \
	};                                                                                         \
	static const struct es8388_config es8388_config_##n = {                                    \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                    \
		.spk_en = GPIO_DT_SPEC_INST_GET_OR(n, spk_en_gpios, {0}),                          \
		.dac_output = ES8388_DAC_OUTPUT(n),                                                \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, es8388_init, NULL, &es8388_data_##n, &es8388_config_##n,          \
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY,                       \
			      &es8388_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ES8388_INIT)
