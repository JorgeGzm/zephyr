# Copyright (c) 2026 Jorge Guzman
# SPDX-License-Identifier: Apache-2.0

# The on-board GD-Link (CMSIS-DAP, SWD) needs the GigaDevice OpenOCD, which
# has the gd32w51x flash driver:
#   west flash --openocd /path/to/gigadevice/openocd
#   west debug --openocd /path/to/gigadevice/openocd
# With TrustZone enabled the flash driver registers the flash at its Secure
# alias (0x0c000000), so the raw binary is programmed there, at the offset
# of the image (MCUboot and the application in slot0 with sysbuild). gdb
# would load the ELF at the Non-secure alias, where there is no flash bank:
# west debug attaches to the image west flash programmed.
if(CONFIG_FLASH_USES_MAPPED_PARTITION)
  dt_chosen(gd32w51x_code_partition PROPERTY "zephyr,code-partition")
  dt_reg_addr(gd32w51x_code_addr PATH ${gd32w51x_code_partition})
  math(EXPR gd32w51x_flash_s_addr "${gd32w51x_code_addr} + 0x04000000"
       OUTPUT_FORMAT HEXADECIMAL)
elseif(DEFINED CONFIG_FLASH_LOAD_OFFSET)
  math(EXPR gd32w51x_flash_s_addr "0x0c000000 + ${CONFIG_FLASH_LOAD_OFFSET}"
       OUTPUT_FORMAT HEXADECIMAL)
else()
  set(gd32w51x_flash_s_addr 0x0c000000)
endif()
board_runner_args(openocd "--file-type=bin" "--flash-address=${gd32w51x_flash_s_addr}"
                  "--cmd-load=flash write_image erase" "--no-load")
board_runner_args(jlink "--device=GD32W515PI" "--iface=swd" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
