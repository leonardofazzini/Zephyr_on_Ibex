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
 *
 * Interrupt: level-triggered, asserted when RX FIFO is not empty.
 *   (uart_irq_o = !rx_fifo_empty in RTL)
 *   No hardware TX interrupt — TX is kick-started in software.
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>

#define DT_DRV_COMPAT lowrisc_ibex_uart

#define UART_RX_REG     0x0
#define UART_TX_REG     0x4
#define UART_STATUS_REG 0x8

#define UART_STATUS_RX_EMPTY BIT(0)
#define UART_STATUS_TX_FULL  BIT(1)

struct uart_ibex_config {
	mem_addr_t base;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	void (*irq_config_func)(const struct device *dev);
#endif
};

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
struct uart_ibex_data {
	uart_irq_callback_user_data_t cb;
	void *cb_data;
	bool tx_irq_enabled;
	bool rx_irq_enabled;
	bool in_isr;
};
#endif

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

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

static int uart_ibex_fifo_fill(const struct device *dev,
			       const uint8_t *tx_data, int size)
{
	const struct uart_ibex_config *cfg = dev->config;
	int i;

	for (i = 0; i < size; i++) {
		if (sys_read32(cfg->base + UART_STATUS_REG) &
		    UART_STATUS_TX_FULL) {
			break;
		}
		sys_write32(tx_data[i], cfg->base + UART_TX_REG);
	}
	return i;
}

static int uart_ibex_fifo_read(const struct device *dev,
			       uint8_t *rx_data, int size)
{
	const struct uart_ibex_config *cfg = dev->config;
	int i;

	for (i = 0; i < size; i++) {
		if (sys_read32(cfg->base + UART_STATUS_REG) &
		    UART_STATUS_RX_EMPTY) {
			break;
		}
		rx_data[i] = (uint8_t)sys_read32(cfg->base + UART_RX_REG);
	}
	return i;
}

static void uart_ibex_irq_tx_enable(const struct device *dev)
{
	struct uart_ibex_data *data = dev->data;

	data->tx_irq_enabled = true;

	/*
	 * No HW TX interrupt — kick-start the callback so the shell
	 * (or any user) can fill the TX FIFO immediately.
	 * Guard against re-entrancy: if called from within the ISR
	 * callback, the ISR loop will pick up TX readiness naturally.
	 */
	if (!data->in_isr && data->cb) {
		data->cb(dev, data->cb_data);
	}
}

static void uart_ibex_irq_tx_disable(const struct device *dev)
{
	struct uart_ibex_data *data = dev->data;

	data->tx_irq_enabled = false;
}

static int uart_ibex_irq_tx_ready(const struct device *dev)
{
	struct uart_ibex_data *data = dev->data;
	const struct uart_ibex_config *cfg = dev->config;

	if (!data->tx_irq_enabled) {
		return 0;
	}

	return !(sys_read32(cfg->base + UART_STATUS_REG) &
		 UART_STATUS_TX_FULL);
}

static int uart_ibex_irq_tx_complete(const struct device *dev)
{
	const struct uart_ibex_config *cfg = dev->config;

	return !(sys_read32(cfg->base + UART_STATUS_REG) &
		 UART_STATUS_TX_FULL);
}

static void uart_ibex_irq_rx_enable(const struct device *dev)
{
	struct uart_ibex_data *data = dev->data;

	data->rx_irq_enabled = true;
	irq_enable(DT_INST_IRQN(0));
}

static void uart_ibex_irq_rx_disable(const struct device *dev)
{
	struct uart_ibex_data *data = dev->data;

	data->rx_irq_enabled = false;

	/* Only disable at HW level if TX is also off */
	if (!data->tx_irq_enabled) {
		irq_disable(DT_INST_IRQN(0));
	}
}

static int uart_ibex_irq_rx_ready(const struct device *dev)
{
	const struct uart_ibex_config *cfg = dev->config;

	return !(sys_read32(cfg->base + UART_STATUS_REG) &
		 UART_STATUS_RX_EMPTY);
}

static int uart_ibex_irq_is_pending(const struct device *dev)
{
	return uart_ibex_irq_rx_ready(dev) || uart_ibex_irq_tx_ready(dev);
}

static int uart_ibex_irq_update(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1;
}

static void uart_ibex_irq_callback_set(const struct device *dev,
					uart_irq_callback_user_data_t cb,
					void *user_data)
{
	struct uart_ibex_data *data = dev->data;

	data->cb = cb;
	data->cb_data = user_data;
}

static void uart_ibex_isr(const struct device *dev)
{
	struct uart_ibex_data *data = dev->data;

	if (data->cb) {
		data->in_isr = true;
		data->cb(dev, data->cb_data);
		data->in_isr = false;
	}
}

#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int uart_ibex_init(const struct device *dev)
{
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	const struct uart_ibex_config *cfg = dev->config;

	cfg->irq_config_func(dev);
#else
	ARG_UNUSED(dev);
#endif
	return 0;
}

static DEVICE_API(uart, uart_ibex_driver_api) = {
	.poll_in = uart_ibex_poll_in,
	.poll_out = uart_ibex_poll_out,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = uart_ibex_fifo_fill,
	.fifo_read = uart_ibex_fifo_read,
	.irq_tx_enable = uart_ibex_irq_tx_enable,
	.irq_tx_disable = uart_ibex_irq_tx_disable,
	.irq_tx_ready = uart_ibex_irq_tx_ready,
	.irq_tx_complete = uart_ibex_irq_tx_complete,
	.irq_rx_enable = uart_ibex_irq_rx_enable,
	.irq_rx_disable = uart_ibex_irq_rx_disable,
	.irq_rx_ready = uart_ibex_irq_rx_ready,
	.irq_is_pending = uart_ibex_irq_is_pending,
	.irq_update = uart_ibex_irq_update,
	.irq_callback_set = uart_ibex_irq_callback_set,
#endif
};

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

#define UART_IBEX_IRQ_HANDLER(n)					      \
	static void uart_ibex_irq_config_##n(const struct device *dev)	      \
	{								      \
		IRQ_CONNECT(DT_INST_IRQN(n), 0, uart_ibex_isr,		      \
			    DEVICE_DT_INST_GET(n), 0);			      \
	}

#define UART_IBEX_CONFIG_IRQ(n) .irq_config_func = uart_ibex_irq_config_##n,
#define UART_IBEX_DATA_DECL(n) static struct uart_ibex_data uart_ibex_data_##n;
#define UART_IBEX_DATA_PTR(n) &uart_ibex_data_##n

#else

#define UART_IBEX_IRQ_HANDLER(n)
#define UART_IBEX_CONFIG_IRQ(n)
#define UART_IBEX_DATA_DECL(n)
#define UART_IBEX_DATA_PTR(n) NULL

#endif

#define UART_IBEX_INIT(n)						      \
	UART_IBEX_IRQ_HANDLER(n)					      \
	UART_IBEX_DATA_DECL(n)						      \
	static const struct uart_ibex_config uart_ibex_config_##n = {	      \
		.base = DT_INST_REG_ADDR(n),				      \
		UART_IBEX_CONFIG_IRQ(n)					      \
	};								      \
									      \
	DEVICE_DT_INST_DEFINE(n, uart_ibex_init, NULL,			      \
			      UART_IBEX_DATA_PTR(n),			      \
			      &uart_ibex_config_##n, PRE_KERNEL_1,	      \
			      CONFIG_SERIAL_INIT_PRIORITY,		      \
			      &uart_ibex_driver_api);

DT_INST_FOREACH_STATUS_OKAY(UART_IBEX_INIT)
