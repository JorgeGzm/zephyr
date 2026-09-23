# Copyright (c) 2026 Jorge Guzman
# SPDX-License-Identifier: Apache-2.0

zephyr_sources(soc.c)
zephyr_sources_ifdef(CONFIG_PM power.c)

zephyr_include_directories(.)
