# Copyright (c) 2026 Jorge Guzman
# SPDX-License-Identifier: Apache-2.0

# The on-board GD-Link needs the GigaDevice OpenOCD, which has the gd32vw55x
# flash driver:
#   west flash --openocd /path/to/gigadevice/openocd
#   west debug --openocd /path/to/gigadevice/openocd
board_runner_args(jlink "--device=GD32VW553KMQ7" "--iface=jtag" "--speed=4000"
                  "--tool-opt=-jtagconf -1,-1")

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
