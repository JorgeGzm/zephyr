/*
 * Copyright (c) 2026 GZM
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file video_esp32_csi.c
 * @brief ESP32-P4 MIPI-CSI-2 receiver.
 *
 * Three blocks sit between the sensor and memory, and all three have to be
 * brought up in order:
 *
 *   D-PHY  ->  CSI host  ->  CSI bridge  ->  DW-GDMA  ->  buffer
 *
 * The PHY and host are configured wholesale by mipi_csi_hal_init(), which
 * programs the D-PHY PLL for the lane bit rate, releases both from reset and
 * sets the active lane count. What is left here is the part the HAL does not
 * own: the clocks that have to be running before any of it responds, the
 * bridge's view of the frame, and moving the pixels out.
 *
 * The bridge does not write to memory. It exposes a FIFO at a fixed address
 * and the DW-GDMA drains it - the same DMA engine the DSI display driver uses
 * to feed the panel, read here instead of written. The source address is
 * therefore fixed and the destination increments, which is the reverse of the
 * display's configuration.
 *
 * Power: the D-PHY rail is the internal LDO unit 3 at 2.5 V, shared with the
 * DSI transmitter. On boards that drive a MIPI panel it is already on before
 * this driver runs, which is why nothing here switches it.
 *
 * This receiver hands out what the sensor sends. A sensor emitting RAW Bayer
 * produces RAW Bayer buffers; demosaicing is the ISP's job and is not done
 * here.
 */

#define DT_DRV_COMPAT espressif_esp32_csi

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <hal/mipi_csi_hal.h>
#include <hal/mipi_csi_ll.h>
#include <hal/mipi_csi_brg_ll.h>
#include "video_common.h"

#include <hal/isp_hal.h>
#include <hal/isp_ll.h>
#include <hal/dw_gdma_ll.h>
#include <soc/reg_base.h>
#include <esp_intr_alloc.h>
#include <soc/interrupts.h>
#include <zephyr/cache.h>
#include <string.h>
#include <soc/mipi_csi_host_struct.h>

LOG_MODULE_REGISTER(video_esp32_csi, CONFIG_VIDEO_LOG_LEVEL);

/* The bridge's FIFO, read by the DMA. Same address on hw_ver1 and hw_ver3. */
#define CSI_BRG_FIFO_ADDR   MIPI_CSI_BRG_MEM_BASE

/* Espressif's own driver uses 512 for the bridge burst and 16-item bursts of
 * 64-bit words on both sides of the DMA. Reproduced rather than derived: these
 * are the values the block is known to run at.
 */
#define CSI_BRG_BURST_LEN   512

/* The display driver owns DW-GDMA channel 0. Capture takes another one: the
 * two share the controller, its clock and its reset, but not a channel.
 */
#define CSI_DMA_CHANNEL     1

/* The ISP runs at 80 MHz off the 240 MHz PLL, the rate Espressif's own driver
 * picks. Nothing here depends on the exact figure - the block only has to be
 * clocked - but an unclocked or wildly mismatched ISP stalls the pipeline.
 */
#define CSI_ISP_CLK_DIV     3

/* Turning the colour matrix off leaves the sensor's own response untouched,
 * which is what white balance has to be measured against - and what makes the
 * difference visible side by side. There is no standard control for it, so it
 * lives in the private range.
 */
#define VIDEO_CID_ESP32_CSI_COLOUR (VIDEO_CID_PRIVATE_BASE + 0)

/* Colour correction for the SC202CS, from Espressif's calibration of this
 * sensor at roughly 4200 K, the usual indoor figure, in ten-thousandths. Each
 * row sums to one, so it changes hue without changing brightness.
 *
 * Integers rather than floats on purpose: this build has CONFIG_FPU without
 * FPU_SHARING, so floating point is only safe on one thread, and this runs on
 * whichever thread asked for a capture.
 *
 * The matrix expects white-balanced input. This SoC revision has no white
 * balance gain block - it appeared in rev 3 - so the gains are folded into the
 * matrix instead, by scaling its columns: applying gains before a matrix is
 * the same as scaling that matrix's columns by them.
 */
static const int32_t csi_ccm_4200k[3][3] = {
	{ 20841, -7620, -3221 },
	{ -4528, 16710, -2181 },
	{ -3583, -5935, 19518 },
};

struct esp32_csi_config {
	const struct device *sensor;
	uint8_t lanes;
	uint32_t lane_rate_mbps;
	int csi_id;
};

struct esp32_csi_ctrls {
	struct video_ctrl red_balance;
	struct video_ctrl blue_balance;
	struct video_ctrl saturation;
	struct video_ctrl colour;
};

struct esp32_csi_data {
	struct esp32_csi_ctrls ctrls;
	mipi_csi_hal_context_t hal;
	isp_hal_context_t isp;
	uint16_t wb_red;	/* white balance gains, in hundredths */
	uint16_t wb_blue;
	uint8_t bayer;		/* color_raw_element_order_t */
	bool isp_ready;
	bool isp_clocked;
	bool colour;		/* colour correction, off until asked for */
	struct video_format fmt;
	struct k_fifo fifo_in;
	struct k_fifo fifo_out;
	bool streaming;

	struct video_buffer *active;
	intr_handle_t dma_intr;
	uint32_t frames;
	uint32_t overruns;
};

static int esp32_csi_bpp(uint32_t pixelformat);
static void esp32_csi_dma_start(const struct device *dev, struct video_buffer *vbuf);
static void esp32_csi_dma_isr(void *arg);
static int esp32_csi_dma_setup(const struct device *dev);

/* What this receiver hands out, which is not the same as what the sensor
 * sends. The sensor only speaks RAW8; RGB565 is the ISP demosaicing it on the
 * way past, and is listed here because from an application's side it is simply
 * another format the capture device can produce.
 */
static const struct video_format_cap esp32_csi_caps[] = {
	{
		.pixelformat = VIDEO_PIX_FMT_SBGGR8,
		.width_min = 1280, .width_max = 1280, .width_step = 0,
		.height_min = 720, .height_max = 720, .height_step = 0,
	},
	{
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width_min = 1280, .width_max = 1280, .width_step = 0,
		.height_min = 720, .height_max = 720, .height_step = 0,
	},
	{0},
};

static int esp32_csi_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);

	caps->format_caps = esp32_csi_caps;
	caps->min_vbuf_count = 1;
	caps->buf_align = 64;
	return 0;
}

static int esp32_csi_set_fmt(const struct device *dev, struct video_format *fmt)
{
	const struct esp32_csi_config *cfg = dev->config;
	struct esp32_csi_data *data = dev->data;
	struct video_format sensor_fmt = *fmt;
	int bpp;
	int ret;

	bpp = esp32_csi_bpp(fmt->pixelformat);
	if (bpp < 0) {
		LOG_ERR("Unsupported pixel format 0x%08x", fmt->pixelformat);
		return bpp;
	}

	/* The sensor is asked for RAW Bayer whatever the caller wants, because
	 * that is all it can send. Anything else is the ISP's doing.
	 */
	sensor_fmt.pixelformat = VIDEO_PIX_FMT_SBGGR8;
	sensor_fmt.pitch = sensor_fmt.width;

	ret = video_set_format(cfg->sensor, &sensor_fmt);
	if (ret < 0) {
		return ret;
	}

	fmt->pitch = fmt->width * bpp / 8;
	data->fmt = *fmt;
	return 0;
}

static int esp32_csi_get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct esp32_csi_data *data = dev->data;

	*fmt = data->fmt;
	return 0;
}

/* Bits on the wire for one pixel of a format, which is what the bridge counts.
 * Only the RAW formats this receiver has been used with are listed; anything
 * else is refused rather than guessed at, because a wrong value here shows up
 * as a picture that is sheared instead of an error.
 */
static int esp32_csi_bpp(uint32_t pixelformat)
{
	switch (pixelformat) {
	case VIDEO_PIX_FMT_SBGGR8:
	case VIDEO_PIX_FMT_SGBRG8:
	case VIDEO_PIX_FMT_SGRBG8:
	case VIDEO_PIX_FMT_SRGGB8:
		return 8;
	case VIDEO_PIX_FMT_RGB565:
		return 16;
	default:
		return -ENOTSUP;
	}
}

/* The ISP sits between the CSI host and the bridge, and its input stage runs
 * whether or not any processing is asked for. Espressif's driver creates the
 * processor unconditionally and merely clears the enable bit when it wants a
 * pass-through - the comment in their code is explicit that "input module is
 * still working" in that case.
 *
 * This is why capture produced nothing before: the DMA channel was armed, the
 * bridge was enabled and the sensor was streaming, but the ISP was unclocked
 * and knew no frame geometry, so no byte ever reached the bridge FIFO and the
 * channel waited on a request that never came.
 */
/* Without this the picture comes out sharp but washed towards green: the
 * sensor's raw response is nothing like sRGB, and an ISP left at its defaults
 * applies an identity matrix, which corrects none of it.
 */
/* Which corner of the Bayer tile arrives first, given how the sensor is
 * flipped. Mirroring shifts the tile by a pixel, so a flip that is not
 * accounted for here does not turn the picture over - it swaps red and blue.
 *
 * The array reads BGGR when nothing is flipped:
 *
 *      B G        G B        G R        R G
 *      G R        R G        B G        G B
 *     none       hflip      vflip      both
 *
 * which is exactly the enum's own order, so the two flags index it directly.
 */
static color_raw_element_order_t esp32_csi_bayer_order(const struct device *dev)
{
	const struct esp32_csi_config *cfg = dev->config;
	struct esp32_csi_data *data = dev->data;
	struct video_control hflip = {.id = VIDEO_CID_HFLIP};
	struct video_control vflip = {.id = VIDEO_CID_VFLIP};
	unsigned int order;

	if (video_get_ctrl(cfg->sensor, &hflip) < 0 ||
	    video_get_ctrl(cfg->sensor, &vflip) < 0) {
		/* A sensor with no flips to report is not mirrored. */
		return (color_raw_element_order_t)data->bayer;
	}

	order = (unsigned int)data->bayer;
	order ^= vflip.val ? 2U : 0U;
	order ^= hflip.val ? 1U : 0U;

	return (color_raw_element_order_t)order;
}

static void esp32_csi_isp_colour(const struct device *dev)
{
	struct esp32_csi_data *data = dev->data;
	const int32_t gain[3] = {data->wb_red, 100, data->wb_blue};
	isp_ll_ccm_gain_t matrix[3][3];

	isp_ll_ccm_set_clk_ctrl_mode(data->isp.hw, ISP_LL_PIPELINE_CLK_CTRL_AUTO);

	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			/* Fold the white balance in by scaling the columns:
			 * gains applied before a matrix are the same as that
			 * matrix with its columns scaled by them. This part has
			 * no hardware of its own before silicon revision 3.
			 */
			int32_t v = csi_ccm_4200k[row][col] * gain[col] / 100;
			uint32_t sign = (v < 0) ? 1u : 0u;
			uint32_t mag;

			if (v < 0) {
				v = -v;
			}

			/* Sign and magnitude, the magnitude in Q10, which is
			 * how the block reads this field.
			 */
			mag = ((uint32_t)v * (1u << ISP_LL_CCM_MATRIX_FRAC_BITS) + 5000u) / 10000u;
			mag = MIN(mag, BIT(ISP_LL_CCM_MATRIX_INT_BITS +
					   ISP_LL_CCM_MATRIX_FRAC_BITS) - 1u);

			matrix[row][col].val =
				(sign << (ISP_LL_CCM_MATRIX_INT_BITS +
					  ISP_LL_CCM_MATRIX_FRAC_BITS)) | mag;
		}
	}

	isp_ll_ccm_set_matrix(data->isp.hw, matrix);
	isp_ll_ccm_enable(data->isp.hw, true);

	/* 128 is unity in this field, so the control's range is centred there. */
	isp_hal_color_config(&data->isp, &(isp_hal_color_cfg_t){
		.color_contrast = {.val = 128},
		.color_saturation = {.val = (uint32_t)data->ctrls.saturation.val},
		.color_hue = 0,
		.color_brightness = 0,
	});
	isp_ll_color_enable(data->isp.hw, true);
}

static void esp32_csi_isp_setup(const struct device *dev, bool demosaic)
{
	struct esp32_csi_data *data = dev->data;
	hal_utils_clk_div_t clk_div = {.integer = CSI_ISP_CLK_DIV};

	/* HP_SYS_CLKRST is one register shared by every peripheral, and these
	 * four are read-modify-write on it. Espressif's headers require a
	 * critical section for exactly this reason: a lost update here gates
	 * some other peripheral's clock, and the failure surfaces far away -
	 * a bootloader that cannot use the crypto block after the next reset.
	 *
	 * The reset is done once. Repeating it on every capture buys nothing
	 * and puts a live block through reset while the display is streaming.
	 */
	if (!data->isp_clocked) {
		unsigned int key = irq_lock();

		isp_ll_enable_module_clock(true);
		isp_ll_reset_module_clock();
		isp_ll_select_clk_source(ISP_CLK_SRC_DEFAULT);
		isp_ll_set_clock_div(&clk_div);
		irq_unlock(key);

		data->isp_clocked = true;
		LOG_DBG("ISP clocked");
	}

	isp_hal_init(&data->isp, 0);
	LOG_DBG("ISP hal init done");

	isp_ll_clk_enable(data->isp.hw, true);
	isp_ll_set_input_data_source(data->isp.hw, ISP_INPUT_DATA_SOURCE_CSI);

	/* This sensor sends neither line-start nor line-end short packets. */
	isp_ll_enable_line_start_packet_exist(data->isp.hw, false);
	isp_ll_enable_line_end_packet_exist(data->isp.hw, false);

	/* The sensor always sends RAW8. Whether that reaches the buffer as
	 * Bayer or as colour is decided by the output format and the enable
	 * bit below.
	 */
	(void)isp_ll_set_input_data_color_format(data->isp.hw, ISP_COLOR_RAW8);
	(void)isp_ll_set_output_data_color_format(data->isp.hw, ISP_COLOR_RGB565);

	if (demosaic) {
		isp_ll_set_intput_data_h_pixel_num(data->isp.hw, data->fmt.width);
	} else {
		/* With the processing disabled the input stage counts 32-bit
		 * words per line rather than pixels, so the width has to be
		 * restated in those terms or the block never sees a line end.
		 */
		isp_ll_set_intput_data_h_pixel_num(data->isp.hw,
						   DIV_ROUND_UP(data->fmt.width * 8, 32));
	}
	isp_ll_set_intput_data_v_row_num(data->isp.hw, data->fmt.height);
	isp_ll_set_bayer_mode(data->isp.hw, esp32_csi_bayer_order(dev));

	isp_ll_ccm_enable(data->isp.hw, false);
	if (demosaic && data->colour) {
		esp32_csi_isp_colour(dev);
	}

	isp_ll_enable(data->isp.hw, demosaic);
	data->isp_ready = true;
}

static int esp32_csi_configure(const struct device *dev)
{
	const struct esp32_csi_config *cfg = dev->config;
	struct esp32_csi_data *data = dev->data;
	mipi_csi_hal_config_t hal_cfg;
	int bpp;

	bpp = esp32_csi_bpp(data->fmt.pixelformat);
	if (bpp < 0) {
		LOG_ERR("Unsupported pixel format 0x%08x", data->fmt.pixelformat);
		return bpp;
	}

	hal_cfg.lanes_num = cfg->lanes;

	/* Crossed on purpose. The two fields are misnamed: inside the HAL,
	 * frame_height drives the bridge's horizontal pixel count and
	 * frame_width its row count. Assigning them by name leaves the bridge
	 * expecting a 720x1280 frame from a 1280x720 sensor. Espressif's own
	 * driver crosses them here for the same reason.
	 */
	hal_cfg.frame_height = data->fmt.width;
	hal_cfg.frame_width = data->fmt.height;
	hal_cfg.in_bpp = bpp;
	hal_cfg.out_bpp = bpp;
	hal_cfg.byte_swap_en = false;
	hal_cfg.lane_bit_rate_mbps = cfg->lane_rate_mbps;

	/* Programs the D-PHY PLL, releases PHY and host from reset, sets the
	 * lane count and tells the bridge the frame geometry.
	 */
	mipi_csi_hal_init(&data->hal, &hal_cfg);
	mipi_csi_brg_ll_set_burst_len(data->hal.bridge_dev, CSI_BRG_BURST_LEN);

	/* How often the bridge raises a DMA request. Left at its reset value
	 * the bridge may never ask, and a channel that is armed but never
	 * requested moves nothing.
	 */
	mipi_csi_brg_ll_set_dma_req_interval(data->hal.bridge_dev, 1);

	esp32_csi_isp_setup(dev, data->fmt.pixelformat == VIDEO_PIX_FMT_RGB565);

	return 0;
}

static int esp32_csi_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	const struct esp32_csi_config *cfg = dev->config;
	struct esp32_csi_data *data = dev->data;
	int ret;

	ARG_UNUSED(type);

	if (enable == data->streaming) {
		return 0;
	}

	if (enable) {
		LOG_DBG("stream start: configuring");
		ret = esp32_csi_configure(dev);
		if (ret < 0) {
			return ret;
		}

		LOG_DBG("stream start: dma setup");
		ret = esp32_csi_dma_setup(dev);
		if (ret < 0) {
			return ret;
		}

		/* Arm the DMA before the bridge: with no destination ready the
		 * FIFO fills and the bridge latches an overflow.
		 */
		if (data->active == NULL) {
			struct video_buffer *first = k_fifo_get(&data->fifo_in, K_NO_WAIT);

			if (first != NULL) {
				esp32_csi_dma_start(dev, first);
			}
		}

		LOG_DBG("stream start: bridge on");
		mipi_csi_brg_ll_enable(data->hal.bridge_dev, true);

		/* Sensor last, once the whole receive path is standing. Letting
		 * it stream into a pipeline that is still being assembled loses
		 * the first frames at best.
		 */
		LOG_DBG("stream start: starting sensor");
		ret = video_stream_start(cfg->sensor, VIDEO_BUF_TYPE_OUTPUT);
		if (ret < 0) {
			LOG_ERR("Sensor would not start: %d", ret);
			mipi_csi_brg_ll_enable(data->hal.bridge_dev, false);
			return ret;
		}
	} else {
		mipi_csi_brg_ll_enable(data->hal.bridge_dev, false);
		(void)video_stream_stop(cfg->sensor, VIDEO_BUF_TYPE_OUTPUT);
	}

	data->streaming = enable;
	return 0;
}

/* One-time configuration of our DW-GDMA channel.
 *
 * Deliberately does NOT reset the controller. The display driver brings the
 * DW-GDMA up - bus clock, module reset, controller enable - and is streaming
 * a panel by the time capture starts. Resetting the module here would stop
 * that scanout mid-frame. Only the channel this driver owns is touched.
 */
static int esp32_csi_dma_setup(const struct device *dev)
{
	struct esp32_csi_data *data = dev->data;
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);
	int err;

	/* Idempotent, and needed when no display driver ran first. */
	dw_gdma_ll_enable_bus_clock(0, true);
	dw_gdma_ll_enable_controller(dma, true);
	dw_gdma_ll_enable_intr_global(dma, true);

	dw_gdma_ll_channel_enable(dma, CSI_DMA_CHANNEL, false);

	/* Peripheral to memory: the reverse of the display's channel. The CSI
	 * bridge decides when data moves, so it owns the flow control.
	 */
	dw_gdma_ll_channel_set_trans_flow(dma, CSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_CSI,
					  DW_GDMA_ROLE_MEM, DW_GDMA_FLOW_CTRL_SRC);

	/* Contiguous, not a linked list. A capture is one block into one
	 * buffer; the display driver chains items because it cycles
	 * framebuffers, and copying that choice left the channel waiting for a
	 * descriptor it never fetched - current_lli stayed 0 and no byte moved.
	 */
	dw_gdma_ll_channel_set_src_multi_block_type(dma, CSI_DMA_CHANNEL,
						    DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS);
	dw_gdma_ll_channel_set_dst_multi_block_type(dma, CSI_DMA_CHANNEL,
						    DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS);

	/* Where the source reports its status. Espressif's driver points this
	 * at the bridge FIFO itself.
	 */
	dw_gdma_ll_channel_set_src_periph_status_addr(dma, CSI_DMA_CHANNEL, CSI_BRG_FIFO_ADDR);

	dw_gdma_ll_channel_set_src_handshake_interface(dma, CSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
	dw_gdma_ll_channel_set_dst_handshake_interface(dma, CSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
	dw_gdma_ll_channel_set_src_handshake_periph(dma, CSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_CSI);

	dw_gdma_ll_channel_set_src_outstanding_limit(dma, CSI_DMA_CHANNEL, 5);
	dw_gdma_ll_channel_set_dst_outstanding_limit(dma, CSI_DMA_CHANNEL, 5);

	/* Below the display's channel: a late frame is a dropped frame, a late
	 * scanout is a visible tear.
	 */
	dw_gdma_ll_channel_set_priority(dma, CSI_DMA_CHANNEL, 2);

	dw_gdma_ll_channel_enable_intr_generation(dma, CSI_DMA_CHANNEL, UINT32_MAX, true);
	dw_gdma_ll_channel_enable_intr_propagation(
		dma, CSI_DMA_CHANNEL,
		DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE |
			DW_GDMA_LL_CHANNEL_EVENT_SHADOWREG_OR_LLI_INVALID_ERR,
		true);

	if (data->dma_intr != NULL) {
		return 0;
	}

	/* Shared, and filtered by this channel's status bits: the display owns
	 * another channel behind the same interrupt line.
	 */
	err = esp_intr_alloc_intrstatus(ETS_DW_GDMA_INTR_SOURCE, ESP_INTR_FLAG_SHARED,
					(uint32_t)dw_gdma_ll_get_intr_status_reg(dma),
					DW_GDMA_LL_CHANNEL_EVENT_MASK(CSI_DMA_CHANNEL),
					esp32_csi_dma_isr, (void *)dev, &data->dma_intr);
	if (err != 0) {
		LOG_ERR("Could not attach the DMA interrupt: %d", err);
		return -EIO;
	}

	return 0;
}

/* Point the DMA at one buffer and let it run. A frame is a single block, so
 * the item never chains: the transfer ends and the interrupt says so.
 */
static void esp32_csi_dma_start(const struct device *dev, struct video_buffer *vbuf)
{
	struct esp32_csi_data *data = dev->data;
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);
	size_t frame_size = (size_t)data->fmt.pitch * data->fmt.height;

	/* Source is the bridge FIFO: one address, never advancing. */
	dw_gdma_ll_channel_set_src_addr(dma, CSI_DMA_CHANNEL, CSI_BRG_FIFO_ADDR);
	/* Master port and transfer width are not optional extras: the channel
	 * will not start without them, and leaving them out is what kept
	 * trans_amount at zero through three rounds of guessing at the
	 * handshake instead.
	 */
	dw_gdma_ll_channel_set_src_master_port(dma, CSI_DMA_CHANNEL, CSI_BRG_FIFO_ADDR);
	dw_gdma_ll_channel_set_src_trans_width(dma, CSI_DMA_CHANNEL, DW_GDMA_TRANS_WIDTH_64);
	dw_gdma_ll_channel_set_src_burst_mode(dma, CSI_DMA_CHANNEL, DW_GDMA_BURST_MODE_FIXED);
	dw_gdma_ll_channel_set_src_burst_items(dma, CSI_DMA_CHANNEL, DW_GDMA_BURST_ITEMS_512);
	dw_gdma_ll_channel_set_src_burst_len(dma, CSI_DMA_CHANNEL, 16);

	/* Destination is the caller's buffer, filled front to back. */
	dw_gdma_ll_channel_set_dst_addr(dma, CSI_DMA_CHANNEL, (uint32_t)vbuf->buffer);
	dw_gdma_ll_channel_set_dst_master_port(dma, CSI_DMA_CHANNEL, (intptr_t)vbuf->buffer);
	dw_gdma_ll_channel_set_dst_trans_width(dma, CSI_DMA_CHANNEL, DW_GDMA_TRANS_WIDTH_64);
	dw_gdma_ll_channel_set_dst_burst_mode(dma, CSI_DMA_CHANNEL, DW_GDMA_BURST_MODE_INCREMENT);
	dw_gdma_ll_channel_set_dst_burst_items(dma, CSI_DMA_CHANNEL, DW_GDMA_BURST_ITEMS_512);
	dw_gdma_ll_channel_set_dst_burst_len(dma, CSI_DMA_CHANNEL, 16);

	/* Counted in 64-bit words, matching the transfer width. */
	dw_gdma_ll_channel_set_trans_block_size(dma, CSI_DMA_CHANNEL, frame_size / 8);

	/* The DMA writes straight to memory, so anything the CPU still holds
	 * for this buffer has to go, or a later read returns the stale line
	 * instead of the captured pixels.
	 */
	sys_cache_data_invd_range(vbuf->buffer, frame_size);

	data->active = vbuf;

	dw_gdma_ll_channel_enable(dma, CSI_DMA_CHANNEL, true);
}

static void esp32_csi_dma_isr(void *arg)
{
	const struct device *dev = arg;
	struct esp32_csi_data *data = dev->data;
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);
	uint32_t status = dw_gdma_ll_channel_get_intr_status(dma, CSI_DMA_CHANNEL);

	if (status == 0U) {
		return;   /* shared vector: this one was not ours */
	}

	dw_gdma_ll_channel_clear_intr(dma, CSI_DMA_CHANNEL, status);

	if ((status & DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE) == 0U) {
		return;
	}

	if (data->active != NULL) {
		struct video_buffer *done = data->active;
		struct video_buffer *next;

		done->bytesused = data->fmt.pitch * data->fmt.height;
		data->active = NULL;
		data->frames++;
		k_fifo_put(&data->fifo_out, done);

		/* Chain straight into the next buffer if one is waiting. With
		 * none, the frame arriving now is dropped rather than written
		 * over a buffer the caller still holds.
		 */
		next = k_fifo_get(&data->fifo_in, K_NO_WAIT);
		if (next != NULL) {
			esp32_csi_dma_start(dev, next);
		} else {
			data->overruns++;
		}
	}
}

static int esp32_csi_enqueue(const struct device *dev, struct video_buffer *vbuf)
{
	struct esp32_csi_data *data = dev->data;
	size_t needed = data->fmt.pitch * data->fmt.height;

	if (vbuf->size < needed) {
		LOG_ERR("Buffer of %u bytes is too small for a %ux%u frame", vbuf->size,
			data->fmt.width, data->fmt.height);
		return -EINVAL;
	}

	vbuf->bytesused = 0;

	/* Straight to the DMA when it is idle and streaming, queued otherwise. */
	if (data->streaming && data->active == NULL) {
		esp32_csi_dma_start(dev, vbuf);
	} else {
		k_fifo_put(&data->fifo_in, vbuf);
	}

	return 0;
}

static int esp32_csi_dequeue(const struct device *dev, struct video_buffer **vbuf,
			     k_timeout_t timeout)
{
	struct esp32_csi_data *data = dev->data;

	*vbuf = k_fifo_get(&data->fifo_out, timeout);
	if (*vbuf == NULL) {
		return -EAGAIN;
	}

	/* The DMA wrote this behind the cache, so drop anything stale the CPU
	 * may hold for it before the caller reads a single pixel.
	 */
	sys_cache_data_invd_range((*vbuf)->buffer, (*vbuf)->bytesused);
	return 0;
}

/* White balance has no hardware of its own before silicon revision 3, so these
 * two reach the colour matrix instead, and the matrix is rebuilt whenever they
 * move. Expressed in hundredths, so 100 is unity.
 */
static int esp32_csi_set_ctrl(const struct device *dev, uint32_t id)
{
	struct esp32_csi_data *data = dev->data;

	switch (id) {
	case VIDEO_CID_RED_BALANCE:
		data->wb_red = (uint16_t)data->ctrls.red_balance.val;
		break;
	case VIDEO_CID_BLUE_BALANCE:
		data->wb_blue = (uint16_t)data->ctrls.blue_balance.val;
		break;
	case VIDEO_CID_SATURATION:
		break;
	case VIDEO_CID_ESP32_CSI_COLOUR:
		data->colour = data->ctrls.colour.val != 0;
		if (data->isp_ready && !data->colour) {
			isp_ll_ccm_enable(data->isp.hw, false);
			isp_ll_color_enable(data->isp.hw, false);
		}
		break;
	default:
		return -ENOTSUP;
	}

	if (data->isp_ready && data->colour) {
		esp32_csi_isp_colour(dev);
	}

	return 0;
}

static int esp32_csi_init_controls(const struct device *dev)
{
	struct esp32_csi_data *data = dev->data;
	struct esp32_csi_ctrls *ctrls = &data->ctrls;
	int ret;

	/* The defaults are what a grey scene measured at: with the matrix
	 * bypassed the sensor reads about 102/118/105, and these bring the
	 * three channels level before the matrix sees them.
	 */
	ret = video_init_ctrl(&ctrls->red_balance, dev, VIDEO_CID_RED_BALANCE,
			      (struct video_ctrl_range){
				      .min = 50, .max = 390, .step = 1, .def = 116});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->blue_balance, dev, VIDEO_CID_BLUE_BALANCE,
			      (struct video_ctrl_range){
				      .min = 50, .max = 390, .step = 1, .def = 112});
	if (ret < 0) {
		return ret;
	}

	ret = video_init_ctrl(&ctrls->saturation, dev, VIDEO_CID_SATURATION,
			      (struct video_ctrl_range){
				      .min = 0, .max = 255, .step = 1, .def = 160});
	if (ret < 0) {
		return ret;
	}

	return video_init_ctrl(&ctrls->colour, dev, VIDEO_CID_ESP32_CSI_COLOUR,
			       (struct video_ctrl_range){
				       .min = 0, .max = 1, .step = 1, .def = 1});
}

static DEVICE_API(video, esp32_csi_driver_api) = {
	.set_ctrl = esp32_csi_set_ctrl,
	.set_format = esp32_csi_set_fmt,
	.get_format = esp32_csi_get_fmt,
	.get_caps = esp32_csi_get_caps,
	.set_stream = esp32_csi_set_stream,
	.enqueue = esp32_csi_enqueue,
	.dequeue = esp32_csi_dequeue,
};

static int esp32_csi_init(const struct device *dev)
{
	const struct esp32_csi_config *cfg = dev->config;
	struct esp32_csi_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->sensor)) {
		LOG_ERR("Sensor not ready");
		return -ENODEV;
	}

	k_fifo_init(&data->fifo_in);
	k_fifo_init(&data->fifo_out);

	/* Clocks before anything else: the host and bridge registers read back
	 * as zero, and writes are dropped, until their bus clocks run.
	 */
	mipi_csi_ll_enable_host_bus_clock(cfg->csi_id, true);
	mipi_csi_ll_reset_host_clock(cfg->csi_id);
	mipi_csi_ll_enable_brg_module_clock(cfg->csi_id, true);
	mipi_csi_ll_reset_brg_module_clock(cfg->csi_id);

	/* The PHY configuration interface is clocked separately from the host,
	 * and is toggled off and on to put its state machine at a known point.
	 */
	mipi_csi_ll_set_phy_clock_source(cfg->csi_id, MIPI_CSI_PHY_CLK_SRC_DEFAULT);
	mipi_csi_ll_enable_phy_config_clock(cfg->csi_id, false);
	mipi_csi_ll_enable_phy_config_clock(cfg->csi_id, true);

	/* Resolve the ISP register base and nothing more, so that data->isp.hw
	 * is never NULL. isp_hal_init() is deliberately not used here: it
	 * writes ISP registers, and doing that before the module is clocked
	 * hangs the bus - hard enough that the console stops answering and the
	 * watchdog reset that follows leaves the part unable to boot.
	 */
	data->isp.hw = ISP_LL_GET_HW(0);

	/* Adopt whatever the sensor is set to, so a caller that never calls
	 * set_format still gets a coherent geometry.
	 */
	ret = video_get_format(cfg->sensor, &data->fmt);
	if (ret < 0) {
		LOG_ERR("Could not read the sensor format: %d", ret);
		return ret;
	}

	ret = esp32_csi_init_controls(dev);
	if (ret < 0) {
		LOG_ERR("Could not register the controls: %d", ret);
		return ret;
	}

	LOG_INF("CSI%d ready: %u lanes at %u Mbps, sensor %ux%u", cfg->csi_id, cfg->lanes,
		cfg->lane_rate_mbps, data->fmt.width, data->fmt.height);
	return 0;
}

#define ESP32_CSI_INIT(n)                                                                          \
	static struct esp32_csi_data esp32_csi_data_##n = {                                 \
		.wb_red = 116,                                                             \
		.wb_blue = 112,                                                            \
		.colour = true,                                                            \
		.bayer = COLOR_RAW_ELEMENT_ORDER_BGGR,                                     \
	};                                           \
	static const struct esp32_csi_config esp32_csi_config_##n = {                              \
		.sensor = DEVICE_DT_GET(DT_INST_PHANDLE(n, sensor)),                               \
		.lanes = DT_INST_PROP(n, data_lanes),                                              \
		.lane_rate_mbps = DT_INST_PROP(n, lane_rate_mbps),                                 \
		.csi_id = 0,                                                                       \
	};                                                                                         \
	VIDEO_DEVICE_DEFINE(esp32_csi_##n, DEVICE_DT_INST_GET(n),                                  \
			    DEVICE_DT_GET(DT_INST_PHANDLE(n, sensor)));                            \
	DEVICE_DT_INST_DEFINE(n, esp32_csi_init, NULL, &esp32_csi_data_##n,                        \
			      &esp32_csi_config_##n, POST_KERNEL,                                  \
			      CONFIG_VIDEO_ESP32_CSI_INIT_PRIORITY, &esp32_csi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ESP32_CSI_INIT)

#if defined(CONFIG_SHELL)

#include <zephyr/shell/shell.h>
#include <stdlib.h>

/* What the D-PHY thinks it is receiving. There is no way to infer this from a
 * captured buffer - with no DMA yet there is no buffer - and the difference
 * between "the sensor is silent" and "the receiver is misconfigured" lives
 * entirely in these bits.
 */
static int cmd_csi_phy(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct esp32_csi_data *data = dev->data;
	csi_host_dev_t *host;
	csi_host_phy_rx_reg_t rx;
	uint32_t stopstate, phy_fatal, pkt_fatal;
	unsigned int hs_seen = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!device_is_ready(dev)) {
		shell_error(sh, "CSI not ready");
		return -ENODEV;
	}

	host = (csi_host_dev_t *)data->hal.host_dev;
	if (host == NULL) {
		shell_warn(sh, "PHY not configured yet - start the stream first");
		return -EAGAIN;
	}

	/* One read, then report from that snapshot. Reading the register once
	 * per printed field gives answers from different instants, and the
	 * clock lane genuinely does drop between frames, so two reads a
	 * microsecond apart can and do disagree.
	 */
	rx.val = host->phy_rx.val;
	stopstate = host->phy_stopstate.val;
	phy_fatal = host->int_st_phy_fatal.val;
	pkt_fatal = host->int_st_pkt_fatal.val;

	shell_print(sh, "streaming        %s", data->streaming ? "yes" : "no");
	shell_print(sh, "phy_rx           0x%08x  (clkactivehs=%u ulpsclknot=%u)", rx.val,
		    rx.phy_rxclkactivehs, rx.phy_rxulpsclknot);
	shell_print(sh, "phy_stopstate    0x%08x", stopstate);
	shell_print(sh, "phy fatal        0x%08x%s", phy_fatal, phy_fatal ? "  <- ERRORS" : "");
	shell_print(sh, "pkt fatal        0x%08x%s", pkt_fatal, pkt_fatal ? "  <- ERRORS" : "");

	/* A sensor sending 30 frames a second leaves the clock lane in high
	 * speed only while a frame is on the wire, so a single sample says
	 * little. Counting over a couple of frame periods separates "silent"
	 * from "transmitting in bursts".
	 */
	for (unsigned int i = 0; i < 200U; i++) {
		if (host->phy_rx.phy_rxclkactivehs) {
			hs_seen++;
		}
		k_busy_wait(500);
	}
	shell_print(sh, "clock lane in HS %u of 200 samples over 100 ms", hs_seen);

	return 0;
}

static int cmd_csi_stream(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	bool on = (argc > 1) && (strcmp(argv[1], "on") == 0);
	int ret;

	ret = video_stream_start(dev, VIDEO_BUF_TYPE_OUTPUT);
	if (!on) {
		ret = video_stream_stop(dev, VIDEO_BUF_TYPE_OUTPUT);
	}
	shell_print(sh, "stream %s: %d", on ? "on" : "off", ret);
	return ret;
}

/* Capture one frame into a buffer from the shared video pool and report what
 * arrived. Pixel statistics rather than the pixels themselves: a DMA that runs
 * but writes nothing leaves the buffer at its fill value, and a mean with no
 * spread is the signature of that.
 */
static int cmd_csi_grab(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct esp32_csi_data *data = dev->data;
	struct video_buffer *vbuf;
	size_t frame_size;
	uint32_t sum = 0;
	uint8_t lo = 0xff, hi = 0;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	frame_size = data->fmt.pitch * data->fmt.height;

	vbuf = video_buffer_alloc(frame_size, K_MSEC(500));
	if (vbuf == NULL) {
		shell_error(sh, "no video buffer of %u bytes", frame_size);
		return -ENOMEM;
	}
	/* video_buffer_alloc() leaves type unset, and video_enqueue() refuses a
	 * buffer without one - which reads as the driver rejecting the buffer,
	 * because the API returns the same -EINVAL the driver would.
	 */
	vbuf->type = VIDEO_BUF_TYPE_OUTPUT;

	/* A fill value that no sensor produces uniformly, so "the DMA wrote
	 * nothing" is distinguishable from "the scene is flat grey".
	 */
	memset(vbuf->buffer, 0xa5, frame_size);
	sys_cache_data_flush_range(vbuf->buffer, frame_size);

	ret = video_enqueue(dev, vbuf);
	if (ret < 0) {
		shell_error(sh, "enqueue failed: %d", ret);
		video_buffer_release(vbuf);
		return ret;
	}

	ret = video_stream_start(dev, VIDEO_BUF_TYPE_OUTPUT);
	if (ret < 0) {
		shell_error(sh, "stream start failed: %d", ret);
		video_buffer_release(vbuf);
		return ret;
	}

	ret = video_dequeue(dev, &vbuf, K_MSEC(1000));
	(void)video_stream_stop(dev, VIDEO_BUF_TYPE_OUTPUT);

	if (ret < 0) {
		shell_error(sh, "no frame in 1 s (frames=%u overruns=%u)", data->frames,
			    data->overruns);
		return ret;
	}

	for (size_t i = 0; i < vbuf->bytesused; i++) {
		uint8_t v = ((uint8_t *)vbuf->buffer)[i];

		sum += v;
		lo = MIN(lo, v);
		hi = MAX(hi, v);
	}

	shell_print(sh, "frame %u bytes, mean %u, min %u, max %u", vbuf->bytesused,
		    (unsigned int)(sum / vbuf->bytesused), lo, hi);
	shell_print(sh, "%s", (lo == 0xa5 && hi == 0xa5)
				      ? "buffer untouched - the DMA did not write"
				      : "pixels present");
	shell_print(sh, "frames=%u overruns=%u", data->frames, data->overruns);

	video_buffer_release(vbuf);
	return 0;
}

/* Where the transfer actually stopped. "No frame" has three very different
 * causes - the channel never armed, it armed and moved nothing, or it moved
 * data and the completion interrupt never arrived - and only these registers
 * tell them apart.
 */
static int cmd_csi_dma(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct esp32_csi_data *data = dev->data;
	dw_gdma_dev_t *dma = DW_GDMA_LL_GET_HW(0);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "interrupt        %s", data->dma_intr ? "attached" : "NOT ATTACHED");
	shell_print(sh, "active buffer    %s", data->active ? "yes" : "no");
	shell_print(sh, "frames           %u", data->frames);
	shell_print(sh, "overruns         %u", data->overruns);
	shell_print(sh, "block size asked %u words of 64 bit",
		    data->fmt.pitch * data->fmt.height / 8);
	shell_print(sh, "trans amount     %u   <- 0 means nothing moved",
		    (unsigned int)dw_gdma_ll_channel_get_trans_amount(dma, CSI_DMA_CHANNEL));
	shell_print(sh, "fifo remain      %u",
		    (unsigned int)dw_gdma_ll_channel_get_fifo_remain(dma, CSI_DMA_CHANNEL));
	shell_print(sh, "intr status      0x%08x",
		    (unsigned int)dw_gdma_ll_channel_get_intr_status(dma, CSI_DMA_CHANNEL));
	shell_print(sh, "common intr      0x%08x",
		    (unsigned int)dw_gdma_ll_get_common_intr_status(dma));
	shell_print(sh, "current lli      0x%08x",
		    (unsigned int)dw_gdma_ll_channel_get_current_link_list_item_addr(dma,
										    CSI_DMA_CHANNEL));
	return 0;
}

/* Switching the capture format from the shell, so the ISP pass-through and the
 * demosaic path can be compared on the same board without a rebuild.
 */
static int cmd_csi_fmt(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct esp32_csi_data *data = dev->data;
	struct video_format fmt = data->fmt;
	int ret;

	if (strcmp(argv[1], "rgb565") == 0) {
		fmt.pixelformat = VIDEO_PIX_FMT_RGB565;
	} else if (strcmp(argv[1], "raw8") == 0) {
		fmt.pixelformat = VIDEO_PIX_FMT_SBGGR8;
	} else {
		shell_error(sh, "expected rgb565 or raw8");
		return -EINVAL;
	}

	ret = esp32_csi_set_fmt(dev, &fmt);
	if (ret < 0) {
		shell_error(sh, "set_fmt failed: %d", ret);
		return ret;
	}

	shell_print(sh, "format now %ux%u, %u bytes per line", fmt.width, fmt.height, fmt.pitch);
	return 0;
}

/* The sensor and this driver agree on BGGR, and the sensor's own grey ramp
 * confirms the receive path is colour-neutral, so this exists for the case
 * where a different board revision carries a sensor with another phase.
 */
static int cmd_csi_bayer(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[] = {"BGGR", "GBRG", "GRBG", "RGGB"};
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct esp32_csi_data *data = dev->data;
	unsigned long v = strtoul(argv[1], NULL, 0);

	if (v > 3) {
		shell_error(sh, "expected 0..3");
		return -EINVAL;
	}

	if (!data->isp_ready) {
		shell_error(sh, "capture once first: the ISP is set up at stream start");
		return -EAGAIN;
	}

	data->bayer = (uint8_t)v;
	isp_ll_set_bayer_mode(data->isp.hw, esp32_csi_bayer_order(dev));
	shell_print(sh, "bayer base %lu (%s), effective %s", v, names[v],
		    names[esp32_csi_bayer_order(dev)]);
	return 0;
}

/* Through the control interface, like the rest, so the shell and the settings
 * screen cannot disagree about whether the matrix is in force.
 */
static int cmd_csi_colour(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct video_control ctrl = {.id = VIDEO_CID_ESP32_CSI_COLOUR};
	int ret;

	ctrl.val = (strtoul(argv[1], NULL, 0) != 0) ? 1 : 0;

	ret = video_set_ctrl(dev, &ctrl);
	if (ret < 0) {
		shell_error(sh, "colour correction: %d", ret);
		return ret;
	}

	shell_print(sh, "colour correction %s", ctrl.val ? "on" : "off");
	return 0;
}

/* Through the control interface, so this and the settings screen cannot
 * disagree about the gains in force.
 */
static int cmd_csi_wb(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct video_control red = {.id = VIDEO_CID_RED_BALANCE};
	struct video_control blue = {.id = VIDEO_CID_BLUE_BALANCE};
	int ret;

	red.val = (int32_t)strtoul(argv[1], NULL, 0);
	blue.val = (int32_t)strtoul(argv[2], NULL, 0);

	ret = video_set_ctrl(dev, &red);
	if (ret == 0) {
		ret = video_set_ctrl(dev, &blue);
	}
	if (ret < 0) {
		shell_error(sh, "white balance: %d", ret);
		return ret;
	}

	shell_print(sh, "white balance: red x%d.%02d blue x%d.%02d", (int)red.val / 100,
		    (int)red.val % 100, (int)blue.val / 100, (int)blue.val % 100);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(csi_cmds,
	SHELL_CMD_ARG(colour, NULL, "Colour correction matrix: colour <0|1>", cmd_csi_colour,
		      2, 0),
	SHELL_CMD_ARG(bayer, NULL, "Bayer order: bayer <0=BGGR 1=GBRG 2=GRBG 3=RGGB>",
		      cmd_csi_bayer, 2, 0),
	SHELL_CMD_ARG(wb, NULL, "White balance gains in hundredths: wb <red> <blue>",
		      cmd_csi_wb, 3, 0),
	SHELL_CMD_ARG(fmt, NULL, "Capture format: fmt <raw8|rgb565>", cmd_csi_fmt, 2, 0),
	SHELL_CMD(dma, NULL, "Where the DMA transfer stopped.", cmd_csi_dma),
	SHELL_CMD(grab, NULL, "Capture one frame and report what arrived.", cmd_csi_grab),
	SHELL_CMD(phy, NULL, "D-PHY receive state and error counters.", cmd_csi_phy),
	SHELL_CMD_ARG(stream, NULL, "Start or stop capture: stream <on|off>", cmd_csi_stream, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(csi, &csi_cmds, "ESP32-P4 MIPI-CSI receiver", NULL);

#endif /* CONFIG_SHELL */
