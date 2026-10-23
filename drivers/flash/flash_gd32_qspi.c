/*
 * Copyright (c) 2026 Jorge Guzman
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * NOR flash memory on the GD32 QSPI controller, driven in the indirect mode
 * with polled transfers. Commands and addresses use one line; with the
 * quad-enable-requirements property, reads (0x6b) and page programs (0x32)
 * move the data on four lines (1-1-4). Memories up to 16 MiB (3-byte
 * addresses) are supported.
 */

#define DT_DRV_COMPAT gd_gd32_qspi_nor

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/gd32.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "jesd216.h"
#include "spi_nor.h"

LOG_MODULE_REGISTER(flash_gd32_qspi, CONFIG_FLASH_LOG_LEVEL);

#define GD32_QSPI_NODE DT_INST_PARENT(0)

/* Registers */
#define GD32_QSPI_CTL   0x00U
#define GD32_QSPI_DCFG  0x04U
#define GD32_QSPI_STAT  0x08U
#define GD32_QSPI_STATC 0x0CU
#define GD32_QSPI_DTLEN 0x10U
#define GD32_QSPI_TCFG  0x14U
#define GD32_QSPI_ADDR  0x18U
#define GD32_QSPI_DATA  0x20U

#define GD32_QSPI_CTL_QSPIEN  BIT(0)
#define GD32_QSPI_CTL_ABORT   BIT(1)
#define GD32_QSPI_CTL_BUSY    BIT(14)
#define GD32_QSPI_CTL_FTL_POS 8U
#define GD32_QSPI_CTL_PSC_POS 24U

#define GD32_QSPI_DCFG_CSHC_POS 8U
#define GD32_QSPI_DCFG_FMSZ_POS 16U

#define GD32_QSPI_STAT_TERR BIT(0)
#define GD32_QSPI_STAT_TC   BIT(1)
#define GD32_QSPI_STAT_FT   BIT(2)
#define GD32_QSPI_STATC_ALL (BIT(0) | BIT(1) | BIT(3) | BIT(4) | BIT(5))

#define GD32_QSPI_TCFG_IMOD_POS    8U
#define GD32_QSPI_TCFG_ADDRMOD_POS 10U
#define GD32_QSPI_TCFG_ADDRSZ_POS  12U
#define GD32_QSPI_TCFG_DUMYC_POS   18U
#define GD32_QSPI_TCFG_DATAMOD_POS 24U
#define GD32_QSPI_TCFG_FMOD_POS    26U

#define GD32_QSPI_LINES_1 1U
#define GD32_QSPI_LINES_4 3U

#define GD32_QSPI_FMOD_INDIRECT_WRITE 0U
#define GD32_QSPI_FMOD_INDIRECT_READ  1U

/* 24-bit addresses, 8 dummy cycles for the fast read and SFDP commands */
#define GD32_QSPI_ADDR_SIZE  3U
#define GD32_QSPI_DUMMY_READ 8U

#define GD32_QSPI_PAGE_SIZE   256U
#define GD32_QSPI_SECTOR_SIZE KB(4)
#define GD32_QSPI_BLOCK_SIZE  KB(64)

/* Polling bounds: one transfer step, a page program or erase, a chip erase */
#define GD32_QSPI_STEP_TIMEOUT_US 10000U
#define GD32_QSPI_WIP_TIMEOUT_MS  3000U
#define GD32_QSPI_CE_TIMEOUT_MS   320000U

/*
 * The QSPI is a Secure peripheral of the GD32W51x: its alternate functions
 * only reach pins marked Secure (GPIOx_SCFG), while the GPIO driver releases
 * every pin to the Non-secure peripherals. The pins are handed back here,
 * through the Secure alias of the GPIO registers.
 */
#define GD32_GPIO_BASE        DT_REG_ADDR(DT_NODELABEL(gpioa))
#define GD32_GPIO_PORT_STRIDE 0x400U
#define GD32_GPIO_SCFG_S      (0x10000000U + 0x30U)

struct flash_gd32_qspi_config {
	uint32_t reg;
	uint16_t clkid;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
	uint32_t flash_size;
	uint32_t max_frequency;
	uint8_t jedec_id[JESD216_READ_ID_LEN];
	/* JESD216 quad enable requirement (DW15 QER), NONE for single line */
	enum jesd216_dw15_qer_type qer;
};

struct flash_gd32_qspi_data {
	struct k_sem lock;
};

static const struct flash_parameters flash_gd32_qspi_parameters = {
	.write_block_size = 1,
	.erase_value = 0xff,
};

static int qspi_wait(const struct flash_gd32_qspi_config *cfg, uint32_t flags)
{
	uint32_t elapsed = 0U;
	uint32_t stat;

	for (;;) {
		stat = sys_read32(cfg->reg + GD32_QSPI_STAT);
		if ((stat & GD32_QSPI_STAT_TERR) != 0U) {
			return -EIO;
		}

		if ((stat & flags) != 0U) {
			return 0;
		}

		if (elapsed++ >= GD32_QSPI_STEP_TIMEOUT_US) {
			return -ETIMEDOUT;
		}

		k_busy_wait(1);
	}
}

static int qspi_wait_idle(const struct flash_gd32_qspi_config *cfg)
{
	uint32_t elapsed = 0U;

	while ((sys_read32(cfg->reg + GD32_QSPI_CTL) & GD32_QSPI_CTL_BUSY) != 0U) {
		if (elapsed++ >= GD32_QSPI_STEP_TIMEOUT_US) {
			return -ETIMEDOUT;
		}

		k_busy_wait(1);
	}

	return 0;
}

/*
 * Run one indirect mode transfer: the opcode, an optional 24-bit address,
 * optional dummy cycles and an optional data phase, all on one line. The
 * transfer starts when TCFG, or ADDR when there is an address phase, is
 * written.
 */
static int qspi_xfer(const struct flash_gd32_qspi_config *cfg, uint8_t opcode, bool has_addr,
		     uint32_t addr, uint8_t dummy, uint32_t data_lines, uint8_t *buf, size_t len,
		     bool read)
{
	uint32_t tcfg;
	int ret;

	ret = qspi_wait_idle(cfg);
	if (ret < 0) {
		return ret;
	}

	sys_write32(GD32_QSPI_STATC_ALL, cfg->reg + GD32_QSPI_STATC);

	tcfg = opcode | (GD32_QSPI_LINES_1 << GD32_QSPI_TCFG_IMOD_POS) |
	       ((read ? GD32_QSPI_FMOD_INDIRECT_READ : GD32_QSPI_FMOD_INDIRECT_WRITE)
		<< GD32_QSPI_TCFG_FMOD_POS);

	if (has_addr) {
		tcfg |= (GD32_QSPI_LINES_1 << GD32_QSPI_TCFG_ADDRMOD_POS) |
			((GD32_QSPI_ADDR_SIZE - 1U) << GD32_QSPI_TCFG_ADDRSZ_POS);
	}

	tcfg |= (uint32_t)dummy << GD32_QSPI_TCFG_DUMYC_POS;

	if (len > 0U) {
		sys_write32(len - 1U, cfg->reg + GD32_QSPI_DTLEN);
		tcfg |= data_lines << GD32_QSPI_TCFG_DATAMOD_POS;
	}

	sys_write32(tcfg, cfg->reg + GD32_QSPI_TCFG);

	if (has_addr) {
		sys_write32(addr, cfg->reg + GD32_QSPI_ADDR);
	}

	/*
	 * The FIFO threshold is one word: move whole words while at least four
	 * bytes remain, then the tail byte by byte. In reads, FT or the end of
	 * the transfer means that the FIFO holds what is left to read.
	 */
	for (size_t i = 0U; i < len;) {
		size_t step = (len - i >= sizeof(uint32_t)) ? sizeof(uint32_t) : 1U;

		ret = qspi_wait(cfg,
				read ? (GD32_QSPI_STAT_FT | GD32_QSPI_STAT_TC) : GD32_QSPI_STAT_FT);
		if (ret < 0) {
			goto abort;
		}

		if (step == sizeof(uint32_t)) {
			if (read) {
				sys_put_le32(sys_read32(cfg->reg + GD32_QSPI_DATA), &buf[i]);
			} else {
				sys_write32(sys_get_le32(&buf[i]), cfg->reg + GD32_QSPI_DATA);
			}
		} else if (read) {
			buf[i] = sys_read8(cfg->reg + GD32_QSPI_DATA);
		} else {
			sys_write8(buf[i], cfg->reg + GD32_QSPI_DATA);
		}

		i += step;
	}

	ret = qspi_wait(cfg, GD32_QSPI_STAT_TC);
	if (ret < 0) {
		goto abort;
	}

	sys_write32(GD32_QSPI_STATC_ALL, cfg->reg + GD32_QSPI_STATC);

	return 0;

abort:
	sys_write32(sys_read32(cfg->reg + GD32_QSPI_CTL) | GD32_QSPI_CTL_ABORT,
		    cfg->reg + GD32_QSPI_CTL);
	(void)qspi_wait_idle(cfg);
	sys_write32(GD32_QSPI_STATC_ALL, cfg->reg + GD32_QSPI_STATC);
	LOG_ERR("transfer 0x%02x failed (%d)", opcode, ret);

	return ret;
}

static int qspi_transfer(const struct flash_gd32_qspi_config *cfg, uint8_t opcode, bool has_addr,
			 uint32_t addr, uint8_t dummy, uint8_t *buf, size_t len, bool read)
{
	return qspi_xfer(cfg, opcode, has_addr, addr, dummy, GD32_QSPI_LINES_1, buf, len, read);
}

static inline bool qspi_is_quad(const struct flash_gd32_qspi_config *cfg)
{
	return cfg->qer != JESD216_DW15_QER_NONE;
}

static int qspi_cmd(const struct flash_gd32_qspi_config *cfg, uint8_t opcode)
{
	return qspi_transfer(cfg, opcode, false, 0U, 0U, NULL, 0U, false);
}

static int qspi_wait_ready(const struct flash_gd32_qspi_config *cfg, uint32_t timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;
	uint8_t sr;
	int ret;

	do {
		ret = qspi_transfer(cfg, SPI_NOR_CMD_RDSR, false, 0U, 0U, &sr, 1U, true);
		if (ret < 0) {
			return ret;
		}

		if ((sr & SPI_NOR_WIP_BIT) == 0U) {
			return 0;
		}

		if (timeout_ms > GD32_QSPI_WIP_TIMEOUT_MS) {
			k_msleep(1);
		}
	} while (k_uptime_get() < end);

	return -ETIMEDOUT;
}

static int qspi_write_enable(const struct flash_gd32_qspi_config *cfg)
{
	return qspi_cmd(cfg, SPI_NOR_CMD_WREN);
}

static bool flash_gd32_qspi_range_ok(const struct flash_gd32_qspi_config *cfg, off_t offset,
				     size_t len)
{
	return (offset >= 0) && ((size_t)offset <= cfg->flash_size) &&
	       (len <= cfg->flash_size - (size_t)offset);
}

static int flash_gd32_qspi_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;
	struct flash_gd32_qspi_data *dev_data = dev->data;
	int ret;

	if (!flash_gd32_qspi_range_ok(cfg, offset, len)) {
		return -EINVAL;
	}

	if (len == 0U) {
		return 0;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);
	if (qspi_is_quad(cfg)) {
		ret = qspi_xfer(cfg, SPI_NOR_CMD_QREAD, true, offset, GD32_QSPI_DUMMY_READ,
				GD32_QSPI_LINES_4, data, len, true);
	} else {
		ret = qspi_transfer(cfg, SPI_NOR_CMD_READ_FAST, true, offset, GD32_QSPI_DUMMY_READ,
				    data, len, true);
	}
	k_sem_give(&dev_data->lock);

	return ret;
}

static int flash_gd32_qspi_write(const struct device *dev, off_t offset, const void *data,
				 size_t len)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;
	struct flash_gd32_qspi_data *dev_data = dev->data;
	const uint8_t *src = data;
	int ret = 0;

	if (!flash_gd32_qspi_range_ok(cfg, offset, len)) {
		return -EINVAL;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);

	while (len > 0U) {
		/* A page program must not cross a page boundary */
		size_t chunk =
			MIN(len, GD32_QSPI_PAGE_SIZE - ((size_t)offset % GD32_QSPI_PAGE_SIZE));

		ret = qspi_write_enable(cfg);
		if (ret == 0) {
			ret = qspi_xfer(cfg,
					qspi_is_quad(cfg) ? SPI_NOR_CMD_PP_1_1_4 : SPI_NOR_CMD_PP,
					true, offset, 0U,
					qspi_is_quad(cfg) ? GD32_QSPI_LINES_4 : GD32_QSPI_LINES_1,
					(uint8_t *)src, chunk, false);
		}
		if (ret == 0) {
			ret = qspi_wait_ready(cfg, GD32_QSPI_WIP_TIMEOUT_MS);
		}
		if (ret < 0) {
			break;
		}

		offset += chunk;
		src += chunk;
		len -= chunk;
	}

	k_sem_give(&dev_data->lock);

	return ret;
}

static int flash_gd32_qspi_erase(const struct device *dev, off_t offset, size_t size)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;
	struct flash_gd32_qspi_data *dev_data = dev->data;
	int ret = 0;

	if (!flash_gd32_qspi_range_ok(cfg, offset, size) ||
	    ((offset % GD32_QSPI_SECTOR_SIZE) != 0) || ((size % GD32_QSPI_SECTOR_SIZE) != 0U)) {
		return -EINVAL;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);

	if ((offset == 0) && (size == cfg->flash_size)) {
		ret = qspi_write_enable(cfg);
		if (ret == 0) {
			ret = qspi_cmd(cfg, SPI_NOR_CMD_CE);
		}
		if (ret == 0) {
			ret = qspi_wait_ready(cfg, GD32_QSPI_CE_TIMEOUT_MS);
		}
		goto out;
	}

	while (size > 0U) {
		bool block =
			((offset % GD32_QSPI_BLOCK_SIZE) == 0) && (size >= GD32_QSPI_BLOCK_SIZE);
		size_t step = block ? GD32_QSPI_BLOCK_SIZE : GD32_QSPI_SECTOR_SIZE;

		ret = qspi_write_enable(cfg);
		if (ret == 0) {
			ret = qspi_transfer(cfg, block ? SPI_NOR_CMD_BE : SPI_NOR_CMD_SE, true,
					    offset, 0U, NULL, 0U, false);
		}
		if (ret == 0) {
			ret = qspi_wait_ready(cfg, GD32_QSPI_WIP_TIMEOUT_MS);
		}
		if (ret < 0) {
			break;
		}

		offset += step;
		size -= step;
	}

out:
	k_sem_give(&dev_data->lock);

	return ret;
}

static const struct flash_parameters *flash_gd32_qspi_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return &flash_gd32_qspi_parameters;
}

static int flash_gd32_qspi_get_size(const struct device *dev, uint64_t *size)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;

	*size = cfg->flash_size;

	return 0;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void flash_gd32_qspi_pages_layout(const struct device *dev,
					 const struct flash_pages_layout **layout,
					 size_t *layout_size)
{
	static struct flash_pages_layout pages_layout;
	const struct flash_gd32_qspi_config *cfg = dev->config;

	pages_layout.pages_count = cfg->flash_size / GD32_QSPI_SECTOR_SIZE;
	pages_layout.pages_size = GD32_QSPI_SECTOR_SIZE;

	*layout = &pages_layout;
	*layout_size = 1;
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static int qspi_read_jedec_id(const struct flash_gd32_qspi_config *cfg, uint8_t *id)
{
	return qspi_transfer(cfg, SPI_NOR_CMD_RDID, false, 0U, 0U, id, JESD216_READ_ID_LEN, true);
}

#if defined(CONFIG_FLASH_JESD216_API)
static int flash_gd32_qspi_read_jedec_id(const struct device *dev, uint8_t *id)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;
	struct flash_gd32_qspi_data *dev_data = dev->data;
	int ret;

	k_sem_take(&dev_data->lock, K_FOREVER);
	ret = qspi_read_jedec_id(cfg, id);
	k_sem_give(&dev_data->lock);

	return ret;
}

static int flash_gd32_qspi_sfdp_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;
	struct flash_gd32_qspi_data *dev_data = dev->data;
	int ret;

	if (len == 0U) {
		return 0;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);
	ret = qspi_transfer(cfg, JESD216_CMD_READ_SFDP, true, offset, GD32_QSPI_DUMMY_READ, data,
			    len, true);
	k_sem_give(&dev_data->lock);

	return ret;
}
#endif /* CONFIG_FLASH_JESD216_API */

static DEVICE_API(flash, flash_gd32_qspi_api) = {
	.read = flash_gd32_qspi_read,
	.write = flash_gd32_qspi_write,
	.erase = flash_gd32_qspi_erase,
	.get_parameters = flash_gd32_qspi_get_parameters,
	.get_size = flash_gd32_qspi_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = flash_gd32_qspi_pages_layout,
#endif
#if defined(CONFIG_FLASH_JESD216_API)
	.sfdp_read = flash_gd32_qspi_sfdp_read,
	.read_jedec_id = flash_gd32_qspi_read_jedec_id,
#endif
};

/*
 * Set the Quad Enable bit, bit 1 of the second status register (QER S2B1v4,
 * v5 and v6), so that IO2/IO3 carry data instead of WP#/HOLD#. The bit is
 * non-volatile and only written when it is clear.
 */
static int qspi_quad_enable(const struct flash_gd32_qspi_config *cfg)
{
	uint8_t sr[2];
	int ret;

	ret = qspi_transfer(cfg, SPI_NOR_CMD_RDSR2, false, 0U, 0U, &sr[1], 1U, true);
	if (ret < 0 || (sr[1] & BIT(1)) != 0U) {
		return ret;
	}

	ret = qspi_transfer(cfg, SPI_NOR_CMD_RDSR, false, 0U, 0U, &sr[0], 1U, true);
	if (ret < 0) {
		return ret;
	}

	sr[1] |= BIT(1);

	ret = qspi_write_enable(cfg);
	if (ret < 0) {
		return ret;
	}

	if (cfg->qer == JESD216_DW15_QER_S2B1v6) {
		ret = qspi_transfer(cfg, SPI_NOR_CMD_WRSR2, false, 0U, 0U, &sr[1], 1U, false);
	} else {
		ret = qspi_transfer(cfg, SPI_NOR_CMD_WRSR, false, 0U, 0U, sr, 2U, false);
	}
	if (ret == 0) {
		ret = qspi_wait_ready(cfg, GD32_QSPI_WIP_TIMEOUT_MS);
	}
	if (ret < 0) {
		return ret;
	}

	ret = qspi_transfer(cfg, SPI_NOR_CMD_RDSR2, false, 0U, 0U, &sr[1], 1U, true);
	if (ret == 0 && (sr[1] & BIT(1)) == 0U) {
		ret = -EIO;
	}

	return ret;
}

static void qspi_pins_secure(const struct pinctrl_dev_config *pcfg)
{
	const struct pinctrl_state *state;

	if (pinctrl_lookup_state(pcfg, PINCTRL_STATE_DEFAULT, &state) < 0) {
		return;
	}

	for (uint8_t i = 0U; i < state->pin_cnt; i++) {
		uint32_t port = GD32_PORT_GET(state->pins[i]);
		mem_addr_t scfg = GD32_GPIO_BASE + port * GD32_GPIO_PORT_STRIDE + GD32_GPIO_SCFG_S;

		sys_write32(sys_read32(scfg) | BIT(GD32_PIN_GET(state->pins[i])), scfg);
	}
}

static int flash_gd32_qspi_init(const struct device *dev)
{
	const struct flash_gd32_qspi_config *cfg = dev->config;
	struct flash_gd32_qspi_data *dev_data = dev->data;
	uint8_t id[JESD216_READ_ID_LEN];
	uint32_t ahb_rate;
	uint32_t psc;
	int ret;

	k_sem_init(&dev_data->lock, 1, 1);

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	qspi_pins_secure(cfg->pcfg);

	(void)clock_control_on(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid);
	(void)reset_line_toggle_dt(&cfg->reset);

	ret = clock_control_get_rate(GD32_CLOCK_CONTROLLER, (clock_control_subsys_t)&cfg->clkid,
				     &ahb_rate);
	if (ret < 0) {
		return ret;
	}

	/* SCK = AHB / (PSC + 1), at most the memory frequency */
	psc = DIV_ROUND_UP(ahb_rate, cfg->max_frequency);
	psc = CLAMP(psc, 1U, 256U) - 1U;

	/*
	 * FIFO threshold of one word, CS high for two cycles between commands,
	 * memory size 2^(FMSZ + 1) bytes.
	 */
	sys_write32(0U, cfg->reg + GD32_QSPI_CTL);
	sys_write32((1U << GD32_QSPI_DCFG_CSHC_POS) |
			    ((find_lsb_set(cfg->flash_size) - 2U) << GD32_QSPI_DCFG_FMSZ_POS),
		    cfg->reg + GD32_QSPI_DCFG);
	sys_write32((psc << GD32_QSPI_CTL_PSC_POS) |
			    ((sizeof(uint32_t) - 1U) << GD32_QSPI_CTL_FTL_POS) |
			    GD32_QSPI_CTL_QSPIEN,
		    cfg->reg + GD32_QSPI_CTL);

	ret = qspi_read_jedec_id(cfg, id);
	if (ret < 0) {
		LOG_ERR("JEDEC ID read failed (%d)", ret);
		return ret;
	}

	if (memcmp(id, cfg->jedec_id, sizeof(id)) != 0) {
		LOG_ERR("JEDEC ID %02x %02x %02x, expected %02x %02x %02x", id[0], id[1], id[2],
			cfg->jedec_id[0], cfg->jedec_id[1], cfg->jedec_id[2]);
		return -ENODEV;
	}

	if (qspi_is_quad(cfg)) {
		ret = qspi_quad_enable(cfg);
		if (ret < 0) {
			LOG_ERR("quad enable failed (%d)", ret);
			return ret;
		}
	}

	LOG_INF("%u KiB NOR flash, SCK %u Hz, %s", cfg->flash_size / 1024U, ahb_rate / (psc + 1U),
		qspi_is_quad(cfg) ? "1-1-4" : "1-1-1");

	return 0;
}

BUILD_ASSERT(DT_INST_PROP(0, size) / 8 <= MB(16), "only 3-byte addresses are supported");

#define GD32_QSPI_QER(inst)                                                                        \
	_CONCAT(JESD216_DW15_QER_VAL_,                                                             \
		DT_INST_STRING_TOKEN_OR(inst, quad_enable_requirements, NONE))

BUILD_ASSERT((GD32_QSPI_QER(0) == JESD216_DW15_QER_VAL_NONE) ||
		     (GD32_QSPI_QER(0) == JESD216_DW15_QER_VAL_S2B1v4) ||
		     (GD32_QSPI_QER(0) == JESD216_DW15_QER_VAL_S2B1v5) ||
		     (GD32_QSPI_QER(0) == JESD216_DW15_QER_VAL_S2B1v6),
	     "supported quad enable requirements: NONE, S2B1v4, S2B1v5, S2B1v6");

PINCTRL_DT_DEFINE(GD32_QSPI_NODE);

static const struct flash_gd32_qspi_config flash_gd32_qspi_cfg = {
	.reg = DT_REG_ADDR(GD32_QSPI_NODE),
	.clkid = DT_CLOCKS_CELL(GD32_QSPI_NODE, id),
	.reset = RESET_DT_SPEC_GET(GD32_QSPI_NODE),
	.pcfg = PINCTRL_DT_DEV_CONFIG_GET(GD32_QSPI_NODE),
	.flash_size = DT_INST_PROP(0, size) / 8,
	.max_frequency = DT_INST_PROP(0, qspi_max_frequency),
	.jedec_id = DT_INST_PROP(0, jedec_id),
	.qer = (enum jesd216_dw15_qer_type)GD32_QSPI_QER(0),
};

static struct flash_gd32_qspi_data flash_gd32_qspi_dev_data;

DEVICE_DT_INST_DEFINE(0, flash_gd32_qspi_init, NULL, &flash_gd32_qspi_dev_data,
		      &flash_gd32_qspi_cfg, POST_KERNEL, CONFIG_FLASH_GD32_QSPI_INIT_PRIORITY,
		      &flash_gd32_qspi_api);
