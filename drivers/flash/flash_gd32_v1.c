/*
 * Copyright (c) 2022 BrainCo Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "flash_gd32.h"

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <gd32_fmc.h>

LOG_MODULE_DECLARE(flash_gd32);

#define GD32_NV_FLASH_V1_NODE		DT_INST(0, gd_gd32_nv_flash_v1)
#define GD32_NV_FLASH_V1_TIMEOUT	DT_PROP(GD32_NV_FLASH_V1_NODE, max_erase_time_ms)
#define GD32_NV_FLASH_V1_PAGE_SIZE	DT_PROP(GD32_NV_FLASH_V1_NODE, page_size)

#if defined(CONFIG_SOC_SERIES_GD32E10X) || \
	defined(CONFIG_SOC_SERIES_GD32E50X)
/* Some GD32 FMC v1 series require offset and len to word aligned. */
#define GD32_FMC_V1_WORK_ALIGNED
#endif

/* The GD32VW55x FMC reports no programming error flag, only the protection one. */
#ifdef FMC_STAT_PGERR
#define GD32_FMC_V1_PGERR	FMC_STAT_PGERR
#else
#define GD32_FMC_V1_PGERR	0U
#endif

#ifdef FLASH_GD32_FMC_WORK_ALIGNED
#define GD32_FMC_V1_WRITE_ERR	(GD32_FMC_V1_PGERR | FMC_STAT_WPERR | FMC_STAT_PGAERR)
#else
#define GD32_FMC_V1_WRITE_ERR	(GD32_FMC_V1_PGERR | FMC_STAT_WPERR)
#endif
#define GD32_FMC_V1_ERASE_ERR	FMC_STAT_WPERR

#ifdef CONFIG_SOC_SERIES_GD32W51X
#include <gd32_icache.h>

/*
 * Zephyr runs Secure-only on the GD32W51x and the image pages are marked
 * Secure (FMC_SECMCFGx): the flash is erased and programmed through the
 * Secure register set of the FMC, reached through its Secure alias, and
 * written through the Secure alias of the flash. The Secure registers have
 * the bit layout of the Non-secure ones.
 */
#define GD32_SEC_ALIAS_OFFSET	0x10000000U
#define GD32_FMC_V1_KEY		REG32(FMC + GD32_SEC_ALIAS_OFFSET + 0x24U)
#define GD32_FMC_V1_STAT	REG32(FMC + GD32_SEC_ALIAS_OFFSET + 0x2CU)
#define GD32_FMC_V1_CTL		REG32(FMC + GD32_SEC_ALIAS_OFFSET + 0x30U)
#define GD32_FMC_V1_ADDR	REG32(FMC + GD32_SEC_ALIAS_OFFSET + 0x34U)
#define GD32_FMC_V1_FLASH_ADDR	(SOC_NV_FLASH_ADDR + 0x04000000U)
#undef GD32_FMC_V1_WRITE_ERR
#undef GD32_FMC_V1_ERASE_ERR
#define GD32_FMC_V1_WRITE_ERR	(FMC_SECSTAT_SECERR | FMC_SECSTAT_SECWPERR)
#define GD32_FMC_V1_ERASE_ERR	(FMC_SECSTAT_SECERR | FMC_SECSTAT_SECWPERR)
#else
#define GD32_FMC_V1_KEY		FMC_KEY
#define GD32_FMC_V1_STAT	FMC_STAT
#define GD32_FMC_V1_CTL		FMC_CTL
#define GD32_FMC_V1_ADDR	FMC_ADDR
#define GD32_FMC_V1_FLASH_ADDR	SOC_NV_FLASH_ADDR
#endif /* CONFIG_SOC_SERIES_GD32W51X */

#ifdef CONFIG_FLASH_PAGE_LAYOUT
static const struct flash_pages_layout gd32_fmc_v1_layout[] = {
	{
	.pages_size = GD32_NV_FLASH_V1_PAGE_SIZE,
	.pages_count = SOC_NV_FLASH_SIZE / GD32_NV_FLASH_V1_PAGE_SIZE
	}
};
#endif

static inline void gd32_fmc_v1_unlock(void)
{
	GD32_FMC_V1_KEY = UNLOCK_KEY0;
	GD32_FMC_V1_KEY = UNLOCK_KEY1;
}

static inline void gd32_fmc_v1_lock(void)
{
	GD32_FMC_V1_CTL |= FMC_CTL_LK;

#ifdef CONFIG_SOC_SERIES_GD32W51X
	/* Drop the instruction cache lines of the changed flash */
	ICACHE_CTL |= ICACHE_CTL_INVAL;
	while ((ICACHE_STAT & ICACHE_STAT_END) == 0U) {
	}
	ICACHE_FC = ICACHE_STAT_END;
#endif
}

static int gd32_fmc_v1_wait_idle(void)
{
	const int64_t expired_time = k_uptime_get() + GD32_NV_FLASH_V1_TIMEOUT;

	while (GD32_FMC_V1_STAT & FMC_STAT_BUSY) {
		if (k_uptime_get() > expired_time) {
			return -ETIMEDOUT;
		}
	}

	return 0;
}

bool flash_gd32_valid_range(off_t offset, uint32_t len, bool write)
{
	if ((offset > SOC_NV_FLASH_SIZE) ||
	    ((offset + len) > SOC_NV_FLASH_SIZE)) {
		return false;
	}

	if (write) {
		/* Check offset and len is flash_prg_t aligned. */
		if ((offset % sizeof(flash_prg_t)) ||
		    (len % sizeof(flash_prg_t))) {
			return false;
		}

#ifdef FLASH_GD32_FMC_WORK_ALIGNED
		/* Check offset and len is word aligned. */
		if ((offset % sizeof(uint32_t)) ||
		    (len % sizeof(uint32_t))) {
			return false;
		}
#endif

	} else {
		if ((offset % GD32_NV_FLASH_V1_PAGE_SIZE) ||
		    (len % GD32_NV_FLASH_V1_PAGE_SIZE)) {
			return false;
		}
	}

	return true;
}

int flash_gd32_write_range(off_t offset, const void *data, size_t len)
{
	flash_prg_t *prg_flash = (flash_prg_t *)((uint8_t *)GD32_FMC_V1_FLASH_ADDR + offset);
	flash_prg_t *prg_data = (flash_prg_t *)data;
	int ret = 0;

	gd32_fmc_v1_unlock();

	if (GD32_FMC_V1_STAT & FMC_STAT_BUSY) {
		return -EBUSY;
	}

	GD32_FMC_V1_CTL |= FMC_CTL_PG;

	for (size_t i = 0U; i < (len / sizeof(flash_prg_t)); i++) {
		*prg_flash++ = *prg_data++;
	}

	ret = gd32_fmc_v1_wait_idle();
	if (ret < 0) {
		goto expired_out;
	}

	if (GD32_FMC_V1_STAT & GD32_FMC_V1_WRITE_ERR) {
		ret = -EIO;
		GD32_FMC_V1_STAT |= GD32_FMC_V1_WRITE_ERR;
		LOG_ERR("FMC programming failed");
	}

expired_out:
	GD32_FMC_V1_CTL &= ~FMC_CTL_PG;

	gd32_fmc_v1_lock();

	return ret;
}

static int gd32_fmc_v1_page_erase(uint32_t page_addr)
{
	int ret = 0;

	gd32_fmc_v1_unlock();

	if (GD32_FMC_V1_STAT & FMC_STAT_BUSY) {
		return -EBUSY;
	}

	GD32_FMC_V1_CTL |= FMC_CTL_PER;

	GD32_FMC_V1_ADDR = page_addr;

	GD32_FMC_V1_CTL |= FMC_CTL_START;

	ret = gd32_fmc_v1_wait_idle();
	if (ret < 0) {
		goto expired_out;
	}

	if (GD32_FMC_V1_STAT & GD32_FMC_V1_ERASE_ERR) {
		ret = -EIO;
		GD32_FMC_V1_STAT |= GD32_FMC_V1_ERASE_ERR;
		LOG_ERR("FMC page %u erase failed", page_addr);
	}

expired_out:
	GD32_FMC_V1_CTL &= ~FMC_CTL_PER;

	gd32_fmc_v1_lock();

	return ret;
}

int flash_gd32_erase_block(off_t offset, size_t size)
{
	uint32_t page_addr = GD32_FMC_V1_FLASH_ADDR + offset;
	int ret = 0;

	while (size > 0U) {
		ret = gd32_fmc_v1_page_erase(page_addr);
		if (ret < 0) {
			return ret;
		}

		size -= GD32_NV_FLASH_V1_PAGE_SIZE;
		page_addr += GD32_NV_FLASH_V1_PAGE_SIZE;
	}

	return 0;
}

#ifdef CONFIG_FLASH_PAGE_LAYOUT
void flash_gd32_pages_layout(const struct device *dev,
			     const struct flash_pages_layout **layout,
			     size_t *layout_size)
{
	ARG_UNUSED(dev);

	*layout = gd32_fmc_v1_layout;
	*layout_size = ARRAY_SIZE(gd32_fmc_v1_layout);
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */
