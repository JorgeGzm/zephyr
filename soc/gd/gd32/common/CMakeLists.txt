# Copyright (c) 2021 Teslabs Engineering S.L.
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_sources_ifdef(CONFIG_SOC_FAMILY_GD_GD32_HAS_BACKUP_DOMAIN_PROTECTION gd32_backup_domain.c)

# Startup, SoC context save/restore and the reset-vector placement shared by
# the RISC-V (Nuclei) series.
if(CONFIG_RISCV)
  zephyr_sources(
    entry.S
    soc_irq.S
  )
  zephyr_linker_sources(ROM_START SORT_KEY 0x0 init.ld)
  set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld CACHE INTERNAL "")
endif()
