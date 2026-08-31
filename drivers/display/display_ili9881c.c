/*
 * Copyright (c) 2026 GZM
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ILI9881C MIPI DSI video-mode panel.
 *
 * The vendor initialization table below is the one Espressif ship for the
 * M5Stack Tab5 in esp-bsp (bsp/m5stack_tab5/priv_include/disp_init_data.h,
 * Apache-2.0). It walks the panel through its command pages - page selection is
 * the 0xff command with the 0x98 0x81 signature and a page number - so the order
 * matters, and it ends back on page 0 with the display turned on.
 */

#define DT_DRV_COMPAT ilitek_ili9881c

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/display/mipi_display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ili9881c, CONFIG_DISPLAY_LOG_LEVEL);

/* Page selection: 0xff with the ILITEK signature and the page to switch to. */
#define ILI9881C_CMD_PAGE   0xffU
#define ILI9881C_PAGE_SIG0  0x98U
#define ILI9881C_PAGE_SIG1  0x81U
#define ILI9881C_PAGE0      0x00U
#define ILI9881C_PAGE1      0x01U

/* Identification lives at 0x00..0x02 of command page 1. The generic DCS
 * GET_DISPLAY_ID (0x04) returns zeroes on this panel, so it is read from there.
 */
#define ILI9881C_REG_ID     0x00U
#define ILI9881C_ID_LEN     3U

/* Time the panel needs after leaving sleep before it accepts the rest. */
#define ILI9881C_SLEEP_OUT_MS 120

/* Vendor table, as a flat stream of (command, payload length, payload...). */
static const uint8_t ili9881c_init_table[] = {
	0xff, 3, 0x98, 0x81, 0x01,
	0xb7, 1, 0x03,
	0xff, 3, 0x98, 0x81, 0x03,
	0x01, 1, 0x00,
	0x02, 1, 0x00,
	0x03, 1, 0x73,
	0x04, 1, 0x00,
	0x05, 1, 0x00,
	0x06, 1, 0x08,
	0x07, 1, 0x00,
	0x08, 1, 0x00,
	0x09, 1, 0x1b,
	0x0a, 1, 0x01,
	0x0b, 1, 0x01,
	0x0c, 1, 0x0d,
	0x0d, 1, 0x01,
	0x0e, 1, 0x01,
	0x0f, 1, 0x26,
	0x10, 1, 0x26,
	0x11, 1, 0x00,
	0x12, 1, 0x00,
	0x13, 1, 0x02,
	0x14, 1, 0x00,
	0x15, 1, 0x00,
	0x16, 1, 0x00,
	0x17, 1, 0x00,
	0x18, 1, 0x00,
	0x19, 1, 0x00,
	0x1a, 1, 0x00,
	0x1b, 1, 0x00,
	0x1c, 1, 0x00,
	0x1d, 1, 0x00,
	0x1e, 1, 0x40,
	0x1f, 1, 0x00,
	0x20, 1, 0x06,
	0x21, 1, 0x01,
	0x22, 1, 0x00,
	0x23, 1, 0x00,
	0x24, 1, 0x00,
	0x25, 1, 0x00,
	0x26, 1, 0x00,
	0x27, 1, 0x00,
	0x28, 1, 0x33,
	0x29, 1, 0x03,
	0x2a, 1, 0x00,
	0x2b, 1, 0x00,
	0x2c, 1, 0x00,
	0x2d, 1, 0x00,
	0x2e, 1, 0x00,
	0x2f, 1, 0x00,
	0x30, 1, 0x00,
	0x31, 1, 0x00,
	0x32, 1, 0x00,
	0x33, 1, 0x00,
	0x34, 1, 0x00,
	0x35, 1, 0x00,
	0x36, 1, 0x00,
	0x37, 1, 0x00,
	0x38, 1, 0x00,
	0x39, 1, 0x00,
	0x3a, 1, 0x00,
	0x3b, 1, 0x00,
	0x3c, 1, 0x00,
	0x3d, 1, 0x00,
	0x3e, 1, 0x00,
	0x3f, 1, 0x00,
	0x40, 1, 0x00,
	0x41, 1, 0x00,
	0x42, 1, 0x00,
	0x43, 1, 0x00,
	0x44, 1, 0x00,
	0x50, 1, 0x01,
	0x51, 1, 0x23,
	0x52, 1, 0x45,
	0x53, 1, 0x67,
	0x54, 1, 0x89,
	0x55, 1, 0xab,
	0x56, 1, 0x01,
	0x57, 1, 0x23,
	0x58, 1, 0x45,
	0x59, 1, 0x67,
	0x5a, 1, 0x89,
	0x5b, 1, 0xab,
	0x5c, 1, 0xcd,
	0x5d, 1, 0xef,
	0x5e, 1, 0x11,
	0x5f, 1, 0x02,
	0x60, 1, 0x00,
	0x61, 1, 0x07,
	0x62, 1, 0x06,
	0x63, 1, 0x0e,
	0x64, 1, 0x0f,
	0x65, 1, 0x0c,
	0x66, 1, 0x0d,
	0x67, 1, 0x02,
	0x68, 1, 0x02,
	0x69, 1, 0x02,
	0x6a, 1, 0x02,
	0x6b, 1, 0x02,
	0x6c, 1, 0x02,
	0x6d, 1, 0x02,
	0x6e, 1, 0x02,
	0x6f, 1, 0x02,
	0x70, 1, 0x02,
	0x71, 1, 0x02,
	0x72, 1, 0x02,
	0x73, 1, 0x05,
	0x74, 1, 0x01,
	0x75, 1, 0x02,
	0x76, 1, 0x00,
	0x77, 1, 0x07,
	0x78, 1, 0x06,
	0x79, 1, 0x0e,
	0x7a, 1, 0x0f,
	0x7b, 1, 0x0c,
	0x7c, 1, 0x0d,
	0x7d, 1, 0x02,
	0x7e, 1, 0x02,
	0x7f, 1, 0x02,
	0x80, 1, 0x02,
	0x81, 1, 0x02,
	0x82, 1, 0x02,
	0x83, 1, 0x02,
	0x84, 1, 0x02,
	0x85, 1, 0x02,
	0x86, 1, 0x02,
	0x87, 1, 0x02,
	0x88, 1, 0x02,
	0x89, 1, 0x05,
	0x8a, 1, 0x01,
	0xff, 3, 0x98, 0x81, 0x04,
	0x38, 1, 0x01,
	0x39, 1, 0x00,
	0x6c, 1, 0x15,
	0x6e, 1, 0x1a,
	0x6f, 1, 0x25,
	0x3a, 1, 0xa4,
	0x8d, 1, 0x20,
	0x87, 1, 0xba,
	0x3b, 1, 0x98,
	0xff, 3, 0x98, 0x81, 0x01,
	0x22, 1, 0x0a,
	0x31, 1, 0x00,
	0x50, 1, 0x6b,
	0x51, 1, 0x66,
	0x53, 1, 0x73,
	0x55, 1, 0x8b,
	0x60, 1, 0x1b,
	0x61, 1, 0x01,
	0x62, 1, 0x0c,
	0x63, 1, 0x00,
	0xa0, 1, 0x00,
	0xa1, 1, 0x15,
	0xa2, 1, 0x1f,
	0xa3, 1, 0x13,
	0xa4, 1, 0x11,
	0xa5, 1, 0x21,
	0xa6, 1, 0x17,
	0xa7, 1, 0x1b,
	0xa8, 1, 0x6b,
	0xa9, 1, 0x1e,
	0xaa, 1, 0x2b,
	0xab, 1, 0x5d,
	0xac, 1, 0x19,
	0xad, 1, 0x14,
	0xae, 1, 0x4b,
	0xaf, 1, 0x1d,
	0xb0, 1, 0x27,
	0xb1, 1, 0x49,
	0xb2, 1, 0x5d,
	0xb3, 1, 0x39,
	0xc0, 1, 0x00,
	0xc1, 1, 0x01,
	0xc2, 1, 0x0c,
	0xc3, 1, 0x11,
	0xc4, 1, 0x15,
	0xc5, 1, 0x28,
	0xc6, 1, 0x1b,
	0xc7, 1, 0x1c,
	0xc8, 1, 0x62,
	0xc9, 1, 0x1c,
	0xca, 1, 0x29,
	0xcb, 1, 0x60,
	0xcc, 1, 0x16,
	0xcd, 1, 0x17,
	0xce, 1, 0x4a,
	0xcf, 1, 0x23,
	0xd0, 1, 0x24,
	0xd1, 1, 0x4f,
	0xd2, 1, 0x5f,
	0xd3, 1, 0x39,
	0xff, 3, 0x98, 0x81, 0x00,
	0x35, 0,
	0xfe, 0,
	0x29, 0,};

struct ili9881c_config {
	const struct device *mipi_dsi;
	const struct gpio_dt_spec reset_gpio;
	const struct gpio_dt_spec bl_gpio;
	uint8_t num_of_lanes;
	uint8_t pixel_format;
	uint16_t panel_width;
	uint16_t panel_height;
	uint8_t channel;
	uint32_t hbp;
	uint32_t hsync;
	uint32_t hfp;
	uint32_t vbp;
	uint32_t vsync;
	uint32_t vfp;
};

static int ili9881c_blanking_off(const struct device *dev)
{
	const struct ili9881c_config *config = dev->config;

	if (config->bl_gpio.port != NULL) {
		return gpio_pin_set_dt(&config->bl_gpio, 1);
	}
	return 0;
}

static int ili9881c_blanking_on(const struct device *dev)
{
	const struct ili9881c_config *config = dev->config;

	if (config->bl_gpio.port != NULL) {
		return gpio_pin_set_dt(&config->bl_gpio, 0);
	}
	return 0;
}

/* The devicetree carries the MIPI DSI pixel format, which the display API
 * reports under its own enumeration.
 */
static enum display_pixel_format ili9881c_display_format(uint8_t pixfmt)
{
	switch (pixfmt) {
	case MIPI_DSI_PIXFMT_RGB565:
		return PIXEL_FORMAT_RGB_565;
	case MIPI_DSI_PIXFMT_RGB888:
	default:
		return PIXEL_FORMAT_RGB_888;
	}
}

static void ili9881c_get_capabilities(const struct device *dev,
				      struct display_capabilities *capabilities)
{
	const struct ili9881c_config *config = dev->config;
	enum display_pixel_format format = ili9881c_display_format(config->pixel_format);

	memset(capabilities, 0, sizeof(struct display_capabilities));
	capabilities->x_resolution = config->panel_width;
	capabilities->y_resolution = config->panel_height;
	capabilities->supported_pixel_formats = format;
	capabilities->current_pixel_format = format;
	capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int ili9881c_set_pixel_format(const struct device *dev,
				     const enum display_pixel_format pixel_format)
{
	const struct ili9881c_config *config = dev->config;

	if (pixel_format == ili9881c_display_format(config->pixel_format)) {
		return 0;
	}

	return -ENOTSUP;
}

static int ili9881c_set_orientation(const struct device *dev,
				    const enum display_orientation orientation)
{
	if (orientation == DISPLAY_ORIENTATION_NORMAL) {
		return 0;
	}
	return -ENOTSUP;
}

static DEVICE_API(display, ili9881c_api) = {
	.blanking_on = ili9881c_blanking_on,
	.blanking_off = ili9881c_blanking_off,
	.get_capabilities = ili9881c_get_capabilities,
	.set_pixel_format = ili9881c_set_pixel_format,
	.set_orientation = ili9881c_set_orientation,
};

static int ili9881c_select_page(const struct device *dev, uint8_t page)
{
	const struct ili9881c_config *config = dev->config;
	const uint8_t sel[] = {ILI9881C_PAGE_SIG0, ILI9881C_PAGE_SIG1, page};

	return mipi_dsi_dcs_write(config->mipi_dsi, config->channel, ILI9881C_CMD_PAGE, sel,
				  sizeof(sel));
}

/* Best-effort: a panel that answers here confirms the DSI read path works, but
 * a silent one is not fatal - the init sequence below is write-only.
 */
static void ili9881c_report_id(const struct device *dev)
{
	const struct ili9881c_config *config = dev->config;
	uint8_t id[ILI9881C_ID_LEN] = {0};

	if (ili9881c_select_page(dev, ILI9881C_PAGE1) < 0) {
		return;
	}

	for (uint8_t i = 0; i < ILI9881C_ID_LEN; i++) {
		if (mipi_dsi_dcs_read(config->mipi_dsi, config->channel, ILI9881C_REG_ID + i,
				      &id[i], 1) < 0) {
			LOG_WRN("Panel ID read failed at register 0x%02x", ILI9881C_REG_ID + i);
			return;
		}
	}

	LOG_INF("Panel ID %02x %02x %02x", id[0], id[1], id[2]);
}

static int ili9881c_send_init_table(const struct device *dev)
{
	const struct ili9881c_config *config = dev->config;
	size_t i = 0;

	while (i + 1 < ARRAY_SIZE(ili9881c_init_table)) {
		uint8_t cmd = ili9881c_init_table[i];
		uint8_t len = ili9881c_init_table[i + 1];
		int ret;

		i += 2;
		ret = mipi_dsi_dcs_write(config->mipi_dsi, config->channel, cmd,
					 len > 0 ? &ili9881c_init_table[i] : NULL, len);
		if (ret < 0) {
			LOG_ERR("Panel init command 0x%02x failed (%d)", cmd, ret);
			return ret;
		}

		i += len;
	}

	return 0;
}

static int ili9881c_init(const struct device *dev)
{
	const struct ili9881c_config *config = dev->config;
	struct mipi_dsi_device mdev;
	uint8_t param;
	int ret;

	if (!device_is_ready(config->mipi_dsi)) {
		LOG_ERR("MIPI DSI host not ready");
		return -ENODEV;
	}

	if (config->num_of_lanes != 2) {
		LOG_ERR("Unsupported lane count %u, panel is configured for 2",
			config->num_of_lanes);
		return -ENOTSUP;
	}

	if (config->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset_gpio)) {
			LOG_ERR("Panel reset GPIO not ready");
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure reset GPIO (%d)", ret);
			return ret;
		}
		k_msleep(10);
		gpio_pin_set_dt(&config->reset_gpio, 0);
		k_msleep(50);
	}

	if (config->bl_gpio.port != NULL) {
		/* Left off until the first frame is on screen, so the panel does
		 * not show whatever the framebuffer happens to contain.
		 */
		ret = gpio_pin_configure_dt(&config->bl_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure backlight GPIO (%d)", ret);
			return ret;
		}
	}

	ili9881c_report_id(dev);

	ret = ili9881c_select_page(dev, ILI9881C_PAGE0);
	if (ret < 0) {
		LOG_ERR("Failed to select command page 0 (%d)", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_write(config->mipi_dsi, config->channel, MIPI_DCS_EXIT_SLEEP_MODE, NULL,
				 0);
	if (ret < 0) {
		LOG_ERR("Failed to leave sleep mode (%d)", ret);
		return ret;
	}
	k_msleep(ILI9881C_SLEEP_OUT_MS);

	param = 0x00;
	ret = mipi_dsi_dcs_write(config->mipi_dsi, config->channel, MIPI_DCS_SET_ADDRESS_MODE,
				 &param, 1);
	if (ret < 0) {
		LOG_ERR("Failed to set address mode (%d)", ret);
		return ret;
	}

	param = (config->pixel_format == MIPI_DSI_PIXFMT_RGB565) ? MIPI_DCS_PIXEL_FORMAT_16BIT
								 : MIPI_DCS_PIXEL_FORMAT_24BIT;
	ret = mipi_dsi_dcs_write(config->mipi_dsi, config->channel, MIPI_DCS_SET_PIXEL_FORMAT,
				 &param, 1);
	if (ret < 0) {
		LOG_ERR("Failed to set pixel format (%d)", ret);
		return ret;
	}

	/* Ends on page 0 with the display on, so no separate SET_DISPLAY_ON. */
	ret = ili9881c_send_init_table(dev);
	if (ret < 0) {
		return ret;
	}
	k_msleep(50);

	mdev.pixfmt = config->pixel_format;
	mdev.data_lanes = config->num_of_lanes;
	mdev.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM;
	mdev.timings.hactive = config->panel_width;
	mdev.timings.hfp = config->hfp;
	mdev.timings.hbp = config->hbp;
	mdev.timings.hsync = config->hsync;
	mdev.timings.vactive = config->panel_height;
	mdev.timings.vfp = config->vfp;
	mdev.timings.vbp = config->vbp;
	mdev.timings.vsync = config->vsync;

	ret = mipi_dsi_attach(config->mipi_dsi, config->channel, &mdev);
	if (ret < 0) {
		LOG_ERR("Failed to attach to MIPI DSI host (%d)", ret);
		return ret;
	}

	LOG_INF("ILI9881C panel initialized (%ux%u)", config->panel_width, config->panel_height);

	return 0;
}

#define ILI9881C_PANEL(id)                                                                         \
	static const struct ili9881c_config ili9881c_config_##id = {                               \
		.mipi_dsi = DEVICE_DT_GET(DT_INST_BUS(id)),                                        \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(id, reset_gpios, {0}),                      \
		.bl_gpio = GPIO_DT_SPEC_INST_GET_OR(id, bl_gpios, {0}),                            \
		.num_of_lanes = DT_INST_PROP_BY_IDX(id, data_lanes, 0),                            \
		.pixel_format = DT_INST_PROP(id, pixel_format),                                    \
		.panel_width = DT_INST_PROP(id, width),                                            \
		.panel_height = DT_INST_PROP(id, height),                                          \
		.channel = DT_INST_REG_ADDR(id),                                                   \
		.hbp = DT_PROP(DT_INST_CHILD(id, display_timings), hback_porch),                   \
		.hsync = DT_PROP(DT_INST_CHILD(id, display_timings), hsync_len),                   \
		.hfp = DT_PROP(DT_INST_CHILD(id, display_timings), hfront_porch),                  \
		.vbp = DT_PROP(DT_INST_CHILD(id, display_timings), vback_porch),                   \
		.vsync = DT_PROP(DT_INST_CHILD(id, display_timings), vsync_len),                   \
		.vfp = DT_PROP(DT_INST_CHILD(id, display_timings), vfront_porch),                  \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(id, ili9881c_init, NULL, NULL, &ili9881c_config_##id, POST_KERNEL,   \
			      CONFIG_DISPLAY_ILI9881C_INIT_PRIORITY, &ili9881c_api);

DT_INST_FOREACH_STATUS_OKAY(ILI9881C_PANEL)
