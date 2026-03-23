# Copyright (c) 2026 Leonardo Fazzini
# SPDX-License-Identifier: Apache-2.0

# Out-of-tree drivers for the Secure-Ibex platform.

if(CONFIG_UART_IBEX)
  add_subdirectory(drivers/serial)
endif()
