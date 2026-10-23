# Copyright (c) 2026 Jorge Guzman
# SPDX-License-Identifier: Apache-2.0

# The on-board GD-Link (CMSIS-DAP, SWD) needs the GigaDevice OpenOCD, which
# has the gd32w51x flash driver:
#   west flash --openocd /path/to/gigadevice/openocd
#   west debug --openocd /path/to/gigadevice/openocd
# With TrustZone enabled the flash driver registers the flash at its Secure
# alias (0x0c000000), so the raw binary is programmed there. gdb would load
# the ELF at the Non-secure alias, where there is no flash bank: west debug
# attaches to the image west flash programmed.
board_runner_args(openocd "--file-type=bin" "--flash-address=0x0c000000"
                  "--cmd-load=flash write_image erase" "--no-load")
board_runner_args(jlink "--device=GD32W515PI" "--iface=swd" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
