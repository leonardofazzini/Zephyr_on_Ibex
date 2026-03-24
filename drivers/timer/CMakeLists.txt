# Copyright (c) 2026 Leonardo Fazzini
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_IBEX_TIMER timer_ibex.c)
