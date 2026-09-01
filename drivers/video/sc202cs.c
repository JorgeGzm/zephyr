/*
 * Copyright (c) 2026 GZM
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sc202cs.c
 * @brief SmartSens SC202CS 2 MP MIPI-CSI image sensor.
 *
 * One CSI data lane, a 24 MHz input clock the sensor takes from its own
 * oscillator, and RAW Bayer out - the sensor has no ISP of its own, so what
 * leaves it is mosaiced and something downstream has to demosaic it.
 *
 * The register sequence below is the vendor's, and is opaque by nature: most of
 * these addresses are undocumented. It is reproduced rather than derived, and
 * the only ones worth naming are broken out as SC202CS_REG_* and used by code.
 *
 * Boards often call this part SC2356 - M5Stack's description of the Tab5 does -
 * but the silicon answers with the SC202CS product ID and takes the SC202CS
 * register map.
 */

#define DT_DRV_COMPAT smartsens_sc202cs

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/video/video.h>

#include "video_common.h"

LOG_MODULE_REGISTER(sc202cs, CONFIG_VIDEO_LOG_LEVEL);

#define SC202CS_PID              0xeb52U

#define SC202CS_REG_SW_RESET     0x0103
#define SC202CS_REG_SLEEP_MODE   0x0100
#define SC202CS_REG_ID_H         0x3107
#define SC202CS_REG_ID_L         0x3108

/* Sentinels used inside the vendor sequence. */
#define SC202CS_REG_END          0xffff
#define SC202CS_REG_DELAY        0xfffe

#define SC202CS_WIDTH            1280
#define SC202CS_HEIGHT           720

struct sc202cs_reg {
	uint16_t addr;
	uint8_t val;
};

/* MIPI, 1 lane, 24 MHz input, RAW8 1280x720 at 30 fps. */
static const struct sc202cs_reg sc202cs_720p_raw8[] = {
	{0x0103, 0x01},
	{0x36e9, 0x80},
	{0x36ea, 0x06},
	{0x36eb, 0x0a},
	{0x36ec, 0x01},
	{0x36ed, 0x18},
	{0x36e9, 0x24},
	{0x301f, 0x18},
	{0x3031, 0x08},
	{0x3037, 0x00},
	{0x3200, 0x00},
	{0x3201, 0xa0},
	{0x3202, 0x00},
	{0x3203, 0xf0},
	{0x3204, 0x05},
	{0x3205, 0xa7},
	{0x3206, 0x03},
	{0x3207, 0xc7},
	{0x3208, 0x05},
	{0x3209, 0x00},
	{0x320a, 0x02},
	{0x320b, 0xd0},
	{0x3210, 0x00},
	{0x3211, 0x04},
	{0x3212, 0x00},
	{0x3213, 0x04},
	{0x3301, 0xff},
	{0x3304, 0x68},
	{0x3306, 0x40},
	{0x3308, 0x08},
	{0x3309, 0xa8},
	{0x330b, 0xd0},
	{0x330c, 0x18},
	{0x330d, 0xff},
	{0x330e, 0x20},
	{0x331e, 0x59},
	{0x331f, 0x99},
	{0x3333, 0x10},
	{0x335e, 0x06},
	{0x335f, 0x08},
	{0x3364, 0x1f},
	{0x337c, 0x02},
	{0x337d, 0x0a},
	{0x338f, 0xa0},
	{0x3390, 0x01},
	{0x3391, 0x03},
	{0x3392, 0x1f},
	{0x3393, 0xff},
	{0x3394, 0xff},
	{0x3395, 0xff},
	{0x33a2, 0x04},
	{0x33ad, 0x0c},
	{0x33b1, 0x20},
	{0x33b3, 0x38},
	{0x33f9, 0x40},
	{0x33fb, 0x48},
	{0x33fc, 0x0f},
	{0x33fd, 0x1f},
	{0x349f, 0x03},
	{0x34a6, 0x03},
	{0x34a7, 0x1f},
	{0x34a8, 0x38},
	{0x34a9, 0x30},
	{0x34ab, 0xd0},
	{0x34ad, 0xd8},
	{0x34f8, 0x1f},
	{0x34f9, 0x20},
	{0x3630, 0xa0},
	{0x3631, 0x92},
	{0x3632, 0x64},
	{0x3633, 0x43},
	{0x3637, 0x49},
	{0x363a, 0x85},
	{0x363c, 0x0f},
	{0x3650, 0x31},
	{0x3670, 0x0d},
	{0x3674, 0xc0},
	{0x3675, 0xa0},
	{0x3676, 0xa0},
	{0x3677, 0x92},
	{0x3678, 0x96},
	{0x3679, 0x9a},
	{0x367c, 0x03},
	{0x367d, 0x0f},
	{0x367e, 0x01},
	{0x367f, 0x0f},
	{0x3698, 0x83},
	{0x3699, 0x86},
	{0x369a, 0x8c},
	{0x369b, 0x94},
	{0x36a2, 0x01},
	{0x36a3, 0x03},
	{0x36a4, 0x07},
	{0x36ae, 0x0f},
	{0x36af, 0x1f},
	{0x36bd, 0x22},
	{0x36be, 0x22},
	{0x36bf, 0x22},
	{0x36d0, 0x01},
	{0x370f, 0x02},
	{0x3721, 0x6c},
	{0x3722, 0x8d},
	{0x3725, 0xc5},
	{0x3727, 0x14},
	{0x3728, 0x04},
	{0x37b7, 0x04},
	{0x37b8, 0x04},
	{0x37b9, 0x06},
	{0x37bd, 0x07},
	{0x37be, 0x0f},
	{0x3901, 0x02},
	{0x3903, 0x40},
	{0x3905, 0x8d},
	{0x3907, 0x00},
	{0x3908, 0x41},
	{0x391f, 0x41},
	{0x3933, 0x80},
	{0x3934, 0x02},
	{0x3937, 0x6f},
	{0x393a, 0x01},
	{0x393d, 0x01},
	{0x393e, 0xc0},
	{0x39dd, 0x41},
	{0x3e00, 0x00},
	{0x3e01, 0x3d},
	{0x3e02, 0xc0},
	{0x3e09, 0x00},
	{0x4509, 0x28},
	{0x450d, 0x61},
	{0x3902, 0x80},
};

struct sc202cs_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec enable_gpio;
};

struct sc202cs_ctrls {
	struct video_ctrl exposure;
	struct video_ctrl gain;
	struct video_ctrl test_pattern;
	struct video_ctrl hflip;
	struct video_ctrl vflip;
};

struct sc202cs_data {
	struct sc202cs_ctrls ctrls;
	struct video_format fmt;
	bool streaming;
};

static const struct video_format_cap sc202cs_fmts[] = {
	{
		.pixelformat = VIDEO_PIX_FMT_SBGGR8,
		.width_min = SC202CS_WIDTH,
		.width_max = SC202CS_WIDTH,
		.height_min = SC202CS_HEIGHT,
		.height_max = SC202CS_HEIGHT,
		.width_step = 0,
		.height_step = 0,
	},
	{0},
};

/* The sensor addresses registers with 16 bits and holds 8, which is not what
 * i2c_reg_write_byte_dt() assumes, so the address goes out by hand.
 */
static int sc202cs_write(const struct device *dev, uint16_t addr, uint8_t val)
{
	const struct sc202cs_config *cfg = dev->config;
	uint8_t buf[3] = {addr >> 8, addr & 0xff, val};

	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

static int sc202cs_read(const struct device *dev, uint16_t addr, uint8_t *val)
{
	const struct sc202cs_config *cfg = dev->config;
	uint8_t buf[2] = {addr >> 8, addr & 0xff};

	return i2c_write_read_dt(&cfg->i2c, buf, sizeof(buf), val, 1);
}

static int sc202cs_write_table(const struct device *dev, const struct sc202cs_reg *regs, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		int ret;

		if (regs[i].addr == SC202CS_REG_END) {
			break;
		}
		if (regs[i].addr == SC202CS_REG_DELAY) {
			k_msleep(regs[i].val);
			continue;
		}

		ret = sc202cs_write(dev, regs[i].addr, regs[i].val);
		if (ret < 0) {
			LOG_ERR("write 0x%04x failed: %d", regs[i].addr, ret);
			return ret;
		}
	}

	return 0;
}

static int sc202cs_set_fmt(const struct device *dev, struct video_format *fmt)
{
	struct sc202cs_data *data = dev->data;

	/* One mode is supported, so anything else is refused rather than
	 * silently rounded to it: a caller that asked for 1600x1200 and got
	 * 1280x720 would read the wrong number of bytes per frame.
	 */
	if (fmt->pixelformat != VIDEO_PIX_FMT_SBGGR8 || fmt->width != SC202CS_WIDTH ||
	    fmt->height != SC202CS_HEIGHT) {
		return -ENOTSUP;
	}

	fmt->pitch = fmt->width;
	data->fmt = *fmt;
	return 0;
}

static int sc202cs_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct sc202cs_data *data = dev->data;

	*fmt = data->fmt;
	return 0;
}

static int sc202cs_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);

	caps->format_caps = sc202cs_fmts;
	caps->min_vbuf_count = 0;
	return 0;
}

static int sc202cs_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	struct sc202cs_data *data = dev->data;
	int ret;

	ARG_UNUSED(type);

	if (enable == data->streaming) {
		return 0;
	}

	ret = sc202cs_write(dev, SC202CS_REG_SLEEP_MODE, enable ? 0x01 : 0x00);
	if (ret < 0) {
		return ret;
	}

	data->streaming = enable;
	return 0;
}

/* Exposure is three registers holding one number; the sensor latches them
 * together, so the order below is the order the datasheet asks for.
 */
static int sc202cs_set_exposure(const struct device *dev, int32_t value)
{
	int ret;

	ret = sc202cs_write(dev, 0x3e00, (value >> 12) & 0x0f);
	ret |= sc202cs_write(dev, 0x3e01, (value >> 4) & 0xff);
	ret |= sc202cs_write(dev, 0x3e02, (value & 0x0f) << 4);

	return ret;
}

static int sc202cs_set_gain(const struct device *dev, int32_t value)
{
	return sc202cs_write(dev, 0x3e09, (uint8_t)value);
}

/* The sensor's own pattern generator, which replaces the pixel array with a
 * grey ramp. Useful precisely because it is not a picture: a ramp that arrives
 * tinted puts the fault in the receive path rather than in the light.
 */
static int sc202cs_set_test_pattern(const struct device *dev, int32_t value)
{
	uint8_t val;
	int ret;

	ret = sc202cs_read(dev, 0x4501, &val);
	if (ret < 0) {
		return ret;
	}

	val = value ? (val | BIT(3)) : (val & ~BIT(3));

	return sc202cs_write(dev, 0x4501, val);
}

/* Both flips live in one register, two bits each. The module on this board is
 * mounted rotated, so the defaults turn both on: without them the picture
 * arrives upside down, on the panel and over the network alike.
 */
static int sc202cs_set_flip(const struct device *dev, uint8_t shift, int32_t value)
{
	uint8_t val;
	int ret;

	ret = sc202cs_read(dev, 0x3221, &val);
	if (ret < 0) {
		return ret;
	}

	val &= ~(0x03 << shift);
	if (value) {
		val |= 0x03 << shift;
	}

	return sc202cs_write(dev, 0x3221, val);
}

static int sc202cs_set_ctrl(const struct device *dev, uint32_t id)
{
	struct sc202cs_data *data = dev->data;

	switch (id) {
	case VIDEO_CID_EXPOSURE:
		return sc202cs_set_exposure(dev, data->ctrls.exposure.val);
	case VIDEO_CID_ANALOGUE_GAIN:
		return sc202cs_set_gain(dev, data->ctrls.gain.val);
	case VIDEO_CID_TEST_PATTERN:
		return sc202cs_set_test_pattern(dev, data->ctrls.test_pattern.val);
	case VIDEO_CID_HFLIP:
		return sc202cs_set_flip(dev, 1, data->ctrls.hflip.val);
	case VIDEO_CID_VFLIP:
		return sc202cs_set_flip(dev, 5, data->ctrls.vflip.val);
	default:
		return -ENOTSUP;
	}
}

static int sc202cs_init_controls(const struct device *dev)
{
	struct sc202cs_data *data = dev->data;
	struct sc202cs_ctrls *ctrls = &data->ctrls;
	int ret;

	/* Defaults match the register table, so the controls agree with the
	 * silicon before anything is written to them.
	 */
	ret = video_init_ctrl(&ctrls->exposure, dev, VIDEO_CID_EXPOSURE,
			      (struct video_ctrl_range){
				      .min = 1, .max = 0xffff, .step = 1, .def = 0x3dc});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->gain, dev, VIDEO_CID_ANALOGUE_GAIN,
			      (struct video_ctrl_range){
				      .min = 0, .max = 0x1f, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->test_pattern, dev, VIDEO_CID_TEST_PATTERN,
			      (struct video_ctrl_range){
				      .min = 0, .max = 1, .step = 1, .def = 0});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->hflip, dev, VIDEO_CID_HFLIP,
			      (struct video_ctrl_range){
				      .min = 0, .max = 1, .step = 1, .def = 1});
	if (ret < 0) {
		return ret;
	}

	return video_init_ctrl(&ctrls->vflip, dev, VIDEO_CID_VFLIP,
			       (struct video_ctrl_range){
				       .min = 0, .max = 1, .step = 1, .def = 1});
}

static DEVICE_API(video, sc202cs_driver_api) = {
	.set_ctrl = sc202cs_set_ctrl,
	.set_format = sc202cs_set_fmt,
	.get_format = sc202cs_get_fmt,
	.get_caps = sc202cs_get_caps,
	.set_stream = sc202cs_set_stream,
};

static int sc202cs_init(const struct device *dev)
{
	const struct sc202cs_config *cfg = dev->config;
	struct sc202cs_data *data = dev->data;
	uint8_t id_h, id_l;
	uint16_t pid;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	/* The enable line is usually on an IO expander, itself on I2C, so this
	 * driver has to initialise after that expander.
	 */
	if (cfg->enable_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->enable_gpio)) {
			LOG_ERR("Enable GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->enable_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Could not drive the enable line: %d", ret);
			return ret;
		}
		/* The sensor's own oscillator has to settle and its internal
		 * reset has to run before it answers on SCCB at all.
		 */
		k_msleep(50);
	}

	ret = sc202cs_write(dev, SC202CS_REG_SW_RESET, 0x01);
	if (ret < 0) {
		LOG_ERR("Sensor did not answer: %d", ret);
		return ret;
	}
	k_msleep(10);

	ret = sc202cs_read(dev, SC202CS_REG_ID_H, &id_h);
	if (ret == 0) {
		ret = sc202cs_read(dev, SC202CS_REG_ID_L, &id_l);
	}
	if (ret < 0) {
		LOG_ERR("Could not read the product ID: %d", ret);
		return ret;
	}

	pid = ((uint16_t)id_h << 8) | id_l;
	if (pid != SC202CS_PID) {
		LOG_ERR("Not an SC202CS: product ID 0x%04x", pid);
		return -ENODEV;
	}

	ret = sc202cs_write_table(dev, sc202cs_720p_raw8, ARRAY_SIZE(sc202cs_720p_raw8));
	if (ret < 0) {
		return ret;
	}

	/* The sequence leaves the sensor asleep; streaming starts on request. */
	data->fmt.pixelformat = VIDEO_PIX_FMT_SBGGR8;
	data->fmt.width = SC202CS_WIDTH;
	data->fmt.height = SC202CS_HEIGHT;
	data->fmt.pitch = SC202CS_WIDTH;
	data->streaming = false;

	ret = sc202cs_init_controls(dev);
	if (ret < 0) {
		LOG_ERR("Could not register the controls: %d", ret);
		return ret;
	}

	/* Registering a control only records its default; the register still
	 * has to be written for the silicon to agree with it.
	 */
	ret = sc202cs_set_flip(dev, 1, data->ctrls.hflip.val);
	ret |= sc202cs_set_flip(dev, 5, data->ctrls.vflip.val);
	if (ret < 0) {
		LOG_ERR("Could not set the image orientation: %d", ret);
		return ret;
	}

	LOG_INF("SC202CS at 0x%02x, %ux%u RAW8", cfg->i2c.addr, SC202CS_WIDTH, SC202CS_HEIGHT);
	return 0;
}

#define SC202CS_INIT(n)                                                                            \
	static struct sc202cs_data sc202cs_data_##n;                                               \
	static const struct sc202cs_config sc202cs_config_##n = {                                  \
		.i2c = I2C_DT_SPEC_INST_GET(n),                                                    \
		.enable_gpio = GPIO_DT_SPEC_INST_GET_OR(n, enable_gpios, {0}),                     \
	};                                                                                         \
	VIDEO_DEVICE_DEFINE(sc202cs_##n, DEVICE_DT_INST_GET(n), NULL);                              \
	DEVICE_DT_INST_DEFINE(n, sc202cs_init, NULL, &sc202cs_data_##n, &sc202cs_config_##n,        \
			      POST_KERNEL, CONFIG_VIDEO_SC202CS_INIT_PRIORITY, &sc202cs_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SC202CS_INIT)

#if defined(CONFIG_SHELL)

#include <zephyr/shell/shell.h>
#include <stdlib.h>

/* Read the identification registers and print what the silicon actually
 * answers, rather than reporting that a comparison passed. A board revision
 * carrying a different sensor would take this register map without complaint
 * up to the point where the pixels do not arrive.
 */
static int cmd_cam_id(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	const struct sc202cs_config *cfg = dev->config;
	uint8_t id_h = 0, id_l = 0;
	uint16_t pid;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = sc202cs_read(dev, SC202CS_REG_ID_H, &id_h);
	if (ret == 0) {
		ret = sc202cs_read(dev, SC202CS_REG_ID_L, &id_l);
	}
	if (ret < 0) {
		shell_error(sh, "SCCB read failed at 0x%02x: %d", cfg->i2c.addr, ret);
		return ret;
	}

	pid = ((uint16_t)id_h << 8) | id_l;

	shell_print(sh, "SCCB address     0x%02x", cfg->i2c.addr);
	shell_print(sh, "reg 0x3107       0x%02x", id_h);
	shell_print(sh, "reg 0x3108       0x%02x", id_l);
	shell_print(sh, "product ID       0x%04x  (SC202CS is 0x%04x)", pid, SC202CS_PID);
	shell_print(sh, "match            %s", pid == SC202CS_PID ? "yes" : "NO - wrong sensor");

	/* Registers the mode sequence writes, read back. A sensor that ignored
	 * the sequence returns its own defaults here.
	 */
	{
		uint8_t w_h = 0, w_l = 0, h_h = 0, h_l = 0, sleep = 0;

		(void)sc202cs_read(dev, 0x3208, &w_h);
		(void)sc202cs_read(dev, 0x3209, &w_l);
		(void)sc202cs_read(dev, 0x320a, &h_h);
		(void)sc202cs_read(dev, 0x320b, &h_l);
		(void)sc202cs_read(dev, SC202CS_REG_SLEEP_MODE, &sleep);

		shell_print(sh, "output size      %ux%u  (expected %ux%u)",
			    ((unsigned int)w_h << 8) | w_l, ((unsigned int)h_h << 8) | h_l,
			    SC202CS_WIDTH, SC202CS_HEIGHT);
		shell_print(sh, "sleep mode       0x%02x  (1 = streaming)", sleep);
	}

	return 0;
}

/* These go through the control interface rather than writing the registers
 * again, so the shell and the settings screen cannot disagree about what the
 * sensor is set to.
 */
static int cmd_cam_ctrl(const struct shell *sh, size_t argc, char **argv)
{
	static const struct {
		const char *name;
		uint32_t cid;
	} table[] = {
		{"exp", VIDEO_CID_EXPOSURE},
		{"gain", VIDEO_CID_ANALOGUE_GAIN},
		{"pattern", VIDEO_CID_TEST_PATTERN},
	};
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct video_control ctrl;
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (strcmp(argv[0], table[i].name) != 0) {
			continue;
		}

		ctrl.id = table[i].cid;
		ctrl.val = (int32_t)strtoul(argv[1], NULL, 0);

		ret = video_set_ctrl(dev, &ctrl);
		if (ret < 0) {
			shell_error(sh, "%s: %d", table[i].name, ret);
			return ret;
		}

		shell_print(sh, "%s = %d", table[i].name, (int)ctrl.val);
		return 0;
	}

	return -EINVAL;
}

/* Raw register access. The datasheet is not public and the vendor driver
 * exposes one bit of one register, so the only way to find out what else this
 * part can be asked to do is to ask it.
 */
static int cmd_cam_reg(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	uint16_t addr = (uint16_t)strtoul(argv[1], NULL, 0);
	uint8_t val;
	int ret;

	if (argc > 2) {
		val = (uint8_t)strtoul(argv[2], NULL, 0);
		ret = sc202cs_write(dev, addr, val);
		if (ret < 0) {
			shell_error(sh, "write failed: %d", ret);
			return ret;
		}
	}

	ret = sc202cs_read(dev, addr, &val);
	if (ret < 0) {
		shell_error(sh, "read failed: %d", ret);
		return ret;
	}

	shell_print(sh, "0x%04x = 0x%02x", addr, val);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(cam_cmds,
	SHELL_CMD_ARG(reg, NULL, "Sensor register: reg <addr> [value]", cmd_cam_reg, 2, 1),
	SHELL_CMD(id, NULL, "Identification registers, read from the silicon.", cmd_cam_id),
	SHELL_CMD_ARG(exp, NULL, "Exposure in lines: exp <value>", cmd_cam_ctrl, 2, 0),
	SHELL_CMD_ARG(gain, NULL, "Analogue gain: gain <value>", cmd_cam_ctrl, 2, 0),
	SHELL_CMD_ARG(pattern, NULL, "Sensor grey ramp: pattern <0|1>", cmd_cam_ctrl, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(cam, &cam_cmds, "SC202CS camera sensor", NULL);

#endif /* CONFIG_SHELL */
