/*
 * Copyright (c) 2026 Leonardo Fazzini
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART driver for the Ibex Reference System.
 *
 * Register map (from hw/Secure-Ibex/rtl/system/uart.sv):
 *   0x0  RX      (R)   - Read byte from RX FIFO
 *   0x4  TX      (W)   - Write byte to TX FIFO
 *   0x8  STATUS  (R)   - bit 0: RX empty, bit 1: TX full
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/uart.h>

#define DT_DRV_COMPAT lowrisc_ibex_uart

#define UART_RX_REG     0x0
#define UART_TX_REG     0x4
#define UART_STATUS_REG 0x8

#define UART_STATUS_RX_EMPTY BIT(0)
#define UART_STATUS_TX_FULL  BIT(1)

struct uart_ibex_config {
	mem_addr_t base;
};

static int uart_ibex_poll_in(const struct device *dev, unsigned char *c)
{
	const struct uart_ibex_config *cfg = dev->config;
	uint32_t status = sys_read32(cfg->base + UART_STATUS_REG);

	if (status & UART_STATUS_RX_EMPTY) {
		return -1;
	}

	*c = (unsigned char)sys_read32(cfg->base + UART_RX_REG);
	return 0;
}

static void uart_ibex_poll_out(const struct device *dev, unsigned char c)
{
	const struct uart_ibex_config *cfg = dev->config;

	/* Wait until TX FIFO has space */
	while (sys_read32(cfg->base + UART_STATUS_REG) & UART_STATUS_TX_FULL) {
	}

	sys_write32((uint32_t)c, cfg->base + UART_TX_REG);
}

static int uart_ibex_init(const struct device *dev)
{
	/* HW baud rate is set at synthesis time (ClockFrequency / BaudRate).
	 * No software configuration needed for baud rate.
	 */
	return 0;
}

static DEVICE_API(uart, uart_ibex_driver_api) = {
	.poll_in = uart_ibex_poll_in,
	.poll_out = uart_ibex_poll_out,
};

#define UART_IBEX_INIT(n)                                                     \
	static const struct uart_ibex_config uart_ibex_config_##n = {         \
		.base = DT_INST_REG_ADDR(n),                                 \
	};                                                                    \
                                                                              \
	DEVICE_DT_INST_DEFINE(n, uart_ibex_init, NULL, NULL,                  \
			      &uart_ibex_config_##n, PRE_KERNEL_1,            \
			      CONFIG_SERIAL_INIT_PRIORITY,                    \
			      &uart_ibex_driver_api);

DT_INST_FOREACH_STATUS_OKAY(UART_IBEX_INIT)
