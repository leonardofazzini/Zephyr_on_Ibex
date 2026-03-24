/*
 * Copyright (c) 2026 Leonardo Fazzini
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System clock driver for the Ibex Reference System timer.
 *
 * Register map (from hw/Secure-Ibex/vendor/lowrisc_ibex/shared/rtl/timer.sv):
 *   0x00  MTIME_LOW      (R/W)  lower 32 bits of mtime
 *   0x04  MTIME_HIGH     (R/W)  upper 32 bits of mtime
 *   0x08  MTIMECMP_LOW   (R/W)  lower 32 bits of mtimecmp
 *   0x0C  MTIMECMP_HIGH  (R/W)  upper 32 bits of mtimecmp
 *
 * mtime increments every clock cycle (50 MHz = 20 ns resolution).
 * IRQ asserted when mtime >= mtimecmp, cleared by writing mtimecmp.
 * Connected to irq_timer_i -> mcause 7 (standard RISC-V machine timer).
 */

#include <limits.h>

#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/sys_clock.h>
#include <zephyr/spinlock.h>
#include <zephyr/irq.h>

#define DT_DRV_COMPAT lowrisc_ibex_timer

#define TIMER_BASE  DT_INST_REG_ADDR(0)
#define TIMER_IRQN  DT_INST_IRQN(0)

/* Register offsets */
#define MTIME_LOW_REG      0x00
#define MTIME_HIGH_REG     0x04
#define MTIMECMP_LOW_REG   0x08
#define MTIMECMP_HIGH_REG  0x0C

#define CYC_PER_TICK ((uint32_t)(sys_clock_hw_cycles_per_sec() \
				 / CONFIG_SYS_CLOCK_TICKS_PER_SEC))

#define cycle_diff_t   unsigned long
#define CYCLE_DIFF_MAX (~(cycle_diff_t)0)

/*
 * Maximum cycles between two ISR invocations.
 * Capped to avoid overflow in sys_clock_announce() (INT32_MAX ticks)
 * and in the cycle_diff_t conversion.  Use 3/4 of the smaller limit.
 */
#define CYCLES_MAX_1  ((uint64_t)INT32_MAX * (uint64_t)CYC_PER_TICK)
#define CYCLES_MAX_2  ((uint64_t)CYCLE_DIFF_MAX)
#define CYCLES_MAX_3  MIN(CYCLES_MAX_1, CYCLES_MAX_2)
#define CYCLES_MAX_4  (CYCLES_MAX_3 / 2 + CYCLES_MAX_3 / 4)
#define CYCLES_MAX    (CYCLES_MAX_4 + LSB_GET(CYCLES_MAX_4))

static struct k_spinlock lock;
static uint64_t last_count;
static uint64_t last_ticks;
static uint32_t last_elapsed;

#if defined(CONFIG_TEST)
const int32_t z_sys_timer_irq_for_test = TIMER_IRQN;
#endif

static uint64_t ibex_mtime(void)
{
	volatile uint32_t *base = (volatile uint32_t *)TIMER_BASE;
	uint32_t lo, hi;

	/* Guard against rollover: re-read if high word changed */
	do {
		hi = sys_read32((mem_addr_t)&base[MTIME_HIGH_REG / 4]);
		lo = sys_read32((mem_addr_t)&base[MTIME_LOW_REG / 4]);
	} while (sys_read32((mem_addr_t)&base[MTIME_HIGH_REG / 4]) != hi);

	return ((uint64_t)hi << 32) | lo;
}

static void ibex_set_mtimecmp(uint64_t value)
{
	/*
	 * The registers are NOT internally latched for multi-word writes.
	 * Set high word to max first to prevent spurious interrupts.
	 */
	sys_write32(0xFFFFFFFF, TIMER_BASE + MTIMECMP_HIGH_REG);
	sys_write32((uint32_t)value, TIMER_BASE + MTIMECMP_LOW_REG);
	sys_write32((uint32_t)(value >> 32), TIMER_BASE + MTIMECMP_HIGH_REG);
}

static void timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	k_spinlock_key_t key = k_spin_lock(&lock);

	uint64_t now = ibex_mtime();
	uint64_t dcycles = now - last_count;
	uint32_t dticks = (cycle_diff_t)dcycles / CYC_PER_TICK;

	last_count += (cycle_diff_t)dticks * CYC_PER_TICK;
	last_ticks += dticks;
	last_elapsed = 0;

	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		uint64_t next = last_count + CYC_PER_TICK;

		ibex_set_mtimecmp(next);
	}

	k_spin_unlock(&lock, key);
	sys_clock_announce(dticks);
}

void sys_clock_set_timeout(int32_t ticks, bool idle)
{
	ARG_UNUSED(idle);

	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t cyc;

	if (ticks == K_TICKS_FOREVER) {
		cyc = last_count + CYCLES_MAX;
	} else {
		cyc = (last_ticks + last_elapsed + ticks) * CYC_PER_TICK;
		if ((cyc - last_count) > CYCLES_MAX) {
			cyc = last_count + CYCLES_MAX;
		}
	}
	ibex_set_mtimecmp(cyc);

	k_spin_unlock(&lock, key);
}

uint32_t sys_clock_elapsed(void)
{
	if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t now = ibex_mtime();
	uint64_t dcycles = now - last_count;
	uint32_t dticks = (cycle_diff_t)dcycles / CYC_PER_TICK;

	last_elapsed = dticks;
	k_spin_unlock(&lock, key);
	return dticks;
}

uint32_t sys_clock_cycle_get_32(void)
{
	return (uint32_t)ibex_mtime();
}

uint64_t sys_clock_cycle_get_64(void)
{
	return ibex_mtime();
}

static int sys_clock_driver_init(void)
{
	IRQ_CONNECT(TIMER_IRQN, 0, timer_isr, NULL, 0);
	last_ticks = ibex_mtime() / CYC_PER_TICK;
	last_count = last_ticks * CYC_PER_TICK;
	ibex_set_mtimecmp(last_count + CYC_PER_TICK);
	irq_enable(TIMER_IRQN);
	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
