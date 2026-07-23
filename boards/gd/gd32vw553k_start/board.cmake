# Copyright (c) 2026 Jorge Guzman
# SPDX-License-Identifier: Apache-2.0

# Flashing and debugging require the GigaDevice OpenOCD fork, which carries the
# gd32vw55x flash driver (mainline / Zephyr-SDK OpenOCD cannot program this part
# yet). Provide it either by placing that OpenOCD first on your PATH, or per
# invocation:
#
#   west flash  --openocd /path/to/gigadevice/openocd
#   west debug  --openocd /path/to/gigadevice/openocd
#
# See the board documentation for details.

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
