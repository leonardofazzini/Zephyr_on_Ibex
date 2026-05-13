# CLAUDE.md — Project context for Zephyr on Ibex

## What this project is

A port of Zephyr RTOS to the Secure-Ibex Reference System — a minimal RISC-V SoC (RV32IMC, Ibex core, 128 KiB SRAM, 50 MHz, M-mode only). The target FPGA board is the Digilent Arty A7. The port is fully working: Zephyr boots, prints to UART, runs multithreaded demos, and supports an interactive shell — all verified in Verilator simulation.

There is a sister project at `../FreeRTOS_on_Ibex/` that ports FreeRTOS to the same hardware. The two projects are completely independent (separate git repos, no shared code), but target identical hardware.

## Hardware quick reference

| Peripheral | Address | Key detail |
|---|---|---|
| SRAM | `0x00100000` | 128 KiB, code + data + stack |
| Timer | `0x80000000` | 64-bit mtime/mtimecmp, mcause 7 |
| UART | `0x80001000` | 3 registers (RX/TX/STATUS), 115200 fixed, mcause 16 |
| GPIO | `0x80002000` | 8-in / 16-out, no IRQ |
| PWM | `0x80003000` | 12-channel, no IRQ |

CPU boots at `0x00100080`. No PLIC, no CLINT — only Ibex fast interrupts. Vectored mtvec only.

Full hardware docs: `doc/secure_ibex_hw_description.md`

## Key files and what they do

### Out-of-tree Zephyr module (the core of the port)

| File | Purpose |
|---|---|
| `zephyr/module.yml` | Registers this repo as a Zephyr module (board_root, soc_root, dts_root) |
| `CMakeLists.txt` | Top-level CMake: adds driver subdirectories |
| `Kconfig` | Top-level Kconfig: includes driver Kconfig files |

### SoC definition (`soc/lowrisc/secure_ibex/`)

| File | Purpose |
|---|---|
| `soc.yml` | SoC name declaration |
| `Kconfig.soc` | Defines `SOC_SECURE_IBEX` symbol |
| `Kconfig` | Selects arch features: `RISCV`, `RISCV_VECTORED_MODE`, `ATOMIC_OPERATIONS_C`, etc. |
| `Kconfig.defconfig` | Defaults: 50 MHz clock, 32 IRQs |
| `CMakeLists.txt` | Points to Zephyr's standard RISC-V linker script |

### Board definition (`boards/lowrisc/secure_ibex/`)

| File | Purpose |
|---|---|
| `board.yml` | Board metadata + SoC link |
| `secure_ibex.yaml` | HW profile for test framework (arch: riscv, ram: 128) |
| `Kconfig.secure_ibex` | Links board to SoC via `select SOC_SECURE_IBEX` |
| `secure_ibex_defconfig` | Default config: serial, console, UART, shell, interrupt-driven UART |
| `secure_ibex.dts` | Board device tree: enables uart0 and mtimer, sets `chosen` nodes |

### Device tree (`dts/`)

| File | Purpose |
|---|---|
| `riscv/lowrisc/secure_ibex.dtsi` | SoC-level device tree: CPU, RAM, timer, UART, GPIO, PWM nodes |
| `bindings/serial/lowrisc,ibex-uart.yaml` | DTS binding for the custom UART |
| `bindings/timer/lowrisc,ibex-timer.yaml` | DTS binding for the custom timer |

### Custom drivers (`drivers/`)

| File | Purpose |
|---|---|
| `serial/uart_ibex.c` | UART driver: polling + interrupt-driven API |
| `serial/Kconfig.ibex_uart` | UART driver Kconfig (auto-enabled via DTS) |
| `serial/CMakeLists.txt` | UART driver build |
| `timer/timer_ibex.c` | System clock driver: mtime/mtimecmp, tickless, 64-bit cycle counter |
| `timer/Kconfig.ibex_timer` | Timer driver Kconfig (replaces upstream `riscv,machine-timer`) |
| `timer/CMakeLists.txt` | Timer driver build |

### Simulation

| File | Purpose |
|---|---|
| `Makefile.verilator` | Builds Verilator model from ~70 SV files + ~12 C++ testbench files, bypassing FuseSoC |

### Documentation

| File | Purpose |
|---|---|
| `doc/secure_ibex_hw_description.md` | Complete SoC hardware architecture (extracted from RTL) |
| `doc/porting_log.md` | Detailed porting log: 11 phases, every decision explained |

## How to build and test

```bash
# Activate environment
source .venv/bin/activate
export ZEPHYR_BASE=$(pwd)/zephyr-workspace/zephyr
export ZEPHYR_EXTRA_MODULES=$(pwd)

# Build hello_world
cd zephyr-workspace
west build -b secure_ibex zephyr/samples/hello_world --pristine -- -DDTC=/usr/bin/dtc

# Simulate
cd ..
make -f Makefile.verilator sim

# Check output
cat uart0.log
```

The `-DDTC=/usr/bin/dtc` flag is needed because the SDK-bundled dtc has dynamic library issues on Ubuntu 24.04.

## Architecture decisions worth knowing

1. **Custom timer driver instead of upstream `riscv,machine-timer`**: the DTS uses `compatible = "lowrisc,ibex-timer"` with a single `reg = <0x80000000 0x10>` covering all 4 registers. This gives full control over the hardware. The upstream driver is automatically excluded because the DTS has no `riscv,machine-timer` node.

2. **Shell uses polling backend**: the Ibex UART has no hardware TX interrupt (only RX). The interrupt-driven shell backend stalls after the first 128-byte TX FIFO fill. `CONFIG_SHELL_BACKEND_SERIAL_API_POLLING` avoids this.

3. **No PLIC selected in SoC Kconfig**: `RISCV_HAS_PLIC` is deliberately NOT selected. Ibex uses fast interrupts (mcause 16-30) directly wired to the CPU.

4. **`ATOMIC_OPERATIONS_C` selected**: Ibex has no "A" extension. Without this, Zephyr would emit `lr.w`/`sc.w` instructions that trap.

5. **`RISCV_VECTORED_MODE` selected**: Ibex only supports vectored mtvec. Without this, Zephyr would try direct mode, which Ibex silently ignores.

## Common tasks

- **Build a different sample**: `west build -b secure_ibex zephyr/samples/<path> --pristine -- -DDTC=/usr/bin/dtc`
- **Disable shell** (saves ~40 KiB): add `-DCONFIG_SHELL=n -DCONFIG_SHELL_BACKEND_SERIAL=n`
- **Rebuild simulator after RTL changes**: `make -f Makefile.verilator clean build`
- **Run with custom ELF**: `make -f Makefile.verilator sim ELF=path/to/file.elf`
- **Interactive shell via PTY**: `screen /dev/pts/N` (N printed at simulator startup)

## What NOT to modify

- `hw/Secure-Ibex/` — git submodule, do not edit directly
- `zephyr-workspace/` — managed by west, in `.gitignore`
- `.venv/` — Python virtual environment, in `.gitignore`
