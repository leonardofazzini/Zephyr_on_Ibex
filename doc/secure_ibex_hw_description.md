# Secure-Ibex Hardware Architecture — Reference for Zephyr Porting

This document describes the hardware architecture of the **Ibex Reference System**
(non-SMTCTX variant) as implemented in `hw/Secure-Ibex/`. Its purpose is to
serve as the single source of truth when writing Zephyr SoC/board definitions,
device-tree files, and low-level drivers.

Source of truth: `rtl/system/ibex_reference_system_core.sv`

---

## 1. System Overview

The Ibex Reference System is a minimal RISC-V SoC built around the **lowRISC Ibex**
core (RV32IMC). It targets Xilinx 7-series FPGAs (primarily Arty A7-35T/100T).

```
                  ┌─────────────────────────────────────────────┐
                  │           ibex_reference_system_core         │
                  │                                             │
                  │  ┌──────────┐       ┌──────────────────┐    │
  UART_RX ───────►│  │  ibex_top │─instr─►│                  │    │
  UART_TX ◄───────│  │  (RV32IM) │─data──►│    bus (2H/7D)   │    │
                  │  └──────────┘       │  priority arbiter │    │
  GP_I[7:0] ─────►│                      └──────┬───────────┘    │
  GP_O[15:0]◄─────│                             │                │
  PWM_O[11:0]◄────│      ┌──────────────────────┼──────┐        │
                  │      │  ┌───┐ ┌────┐ ┌────┐ │┌───┐ │        │
                  │      │  │RAM│ │UART│ │GPIO│ ││TMR│ │        │
                  │      │  │128K│ │    │ │    │ ││   │ │        │
                  │      │  └───┘ └────┘ └────┘ │└───┘ │        │
                  │      │  ┌───┐ ┌────┐ ┌────┐ │      │        │
                  │      │  │PWM│ │ DBG│ │SIM │ │      │        │
                  │      │  └───┘ └────┘ └────┘ │      │        │
                  │      └──────────────────────┘      │        │
                  └─────────────────────────────────────────────┘
```

---

## 2. CPU Core Configuration

| Parameter         | Value                          | Note                              |
|-------------------|--------------------------------|-----------------------------------|
| ISA               | RV32IMC                        | No bit-manipulation (RV32BNone)   |
| RegFile           | RegFileFPGA                    | FPGA-optimised register file      |
| RV32M             | RV32MFast                      | Fast single-cycle multiply        |
| MHPMCounterNum    | 10                             | Hardware performance monitors     |
| DbgTriggerEn      | 1                              | Debug triggers enabled            |
| DbgHwBreakNum     | 2                              | Hardware breakpoints              |
| Hart ID           | 0                              | Single-hart system                |
| Boot address      | `0x00100000`                   | First instr fetched at +0x80      |
| PMP               | Standard ePMP (Smepmp)         |                                   |

**Boot sequence**: The core starts fetching at `boot_addr + 0x80 = 0x00100080`.
The vector table occupies `0x00100000..0x0010007F` (32 entries × 4 bytes each).
Entry 0x80 is the reset vector which jumps to `reset_handler`.

**Note on RV32C**: The Ibex core compiled for this system includes the C (compressed)
extension as part of the standard toolchain configuration (`-march=rv32imc`).

---

## 3. Memory Map

All addresses are physical; there is no MMU. Address decoding uses base+mask.

| Region       | Base Address   | Size    | End Address    | Description                    |
|-------------|----------------|---------|----------------|--------------------------------|
| **SRAM**    | `0x00100000`   | 128 KiB | `0x0011FFFF`   | Code + data (dual-port RAM)    |
| **SimCtrl** | `0x00020000`   | 1 KiB   | `0x000203FF`   | Verilator only, not on FPGA    |
| **Timer**   | `0x80000000`   | 4 KiB   | `0x80000FFF`   | RISC-V machine timer (mtime)   |
| **UART0**   | `0x80001000`   | 4 KiB   | `0x80001FFF`   | Serial port                    |
| **GPIO**    | `0x80002000`   | 4 KiB   | `0x80002FFF`   | General-purpose I/O            |
| **PWM**     | `0x80003000`   | 4 KiB   | `0x80003FFF`   | 12-channel PWM                 |
| **Debug**   | `0x1A110000`   | 64 KiB  | `0x1A11FFFF`   | RISC-V Debug Module (PULP)     |

### Memory layout for SW (from linker script)

```
0x00100000 ┌──────────────────┐
           │  .vectors (128B) │  32 exception entries + reset @ 0x80
0x00100080 │  reset_handler   │
           │  .text           │
           │  .rodata         │
           │  .data           │
           │  .bss            │
           │  (56 KiB total)  │
0x0010E000 ├──────────────────┤
           │  Stack (8 KiB)   │  _stack_start = 0x00110000 (grows down)
0x00110000 └──────────────────┘
           │  ... unused ...  │
0x00120000 └──────────────────┘  End of 128 KiB SRAM
```

---

## 4. Bus Architecture

**Module**: `bus.sv` (vendor/lowrisc_ibex/shared/rtl/bus.sv)

This is a **simplistic priority-based bus** — NOT TL-UL, NOT AXI. It is a custom
synchronous bus with these properties:

- **Data width**: 32 bits
- **Address width**: 32 bits
- **Arbitration**: Strict priority (lower host index wins)
- **Latency**: All devices must respond in the **next cycle** after request
- **Protocol signals**:
  - `req` / `gnt` — request/grant handshake
  - `we` — write enable
  - `be[3:0]` — byte enables
  - `wdata[31:0]` / `rdata[31:0]` — data
  - `rvalid` — response valid (1 cycle after req)
  - `err` — error response

### Bus Hosts (2)

| Index | Name      | Description                              |
|-------|-----------|------------------------------------------|
| 0     | `CoreD`   | Ibex core data port                      |
| 1     | `DbgHost` | Debug module SBA (System Bus Access)     |

**Note**: The instruction port does **NOT** go through the bus. It has a
dedicated path to the dual-port RAM (port B) and to the debug memory. This is
crucial: instruction fetches can only target RAM or Debug memory.

### Bus Devices (7)

| Index | Name      | Base         | Description         |
|-------|-----------|-------------|---------------------|
| 0     | `Ram`     | 0x00100000  | SRAM                |
| 1     | `Gpio`    | 0x80002000  | GPIO                |
| 2     | `Pwm`     | 0x80003000  | PWM                 |
| 3     | `Uart`    | 0x80001000  | UART                |
| 4     | `Timer`   | 0x80000000  | Machine timer       |
| 5     | `SimCtrl` | 0x00020000  | Simulator control   |
| 6     | `DbgDev`  | 0x1A110000  | Debug module memory |

---

## 5. Peripherals — Detailed Register Maps

### 5.1 UART (`uart.sv`)

- **Base**: `0x80001000`
- **Clock**: 50 MHz (ClockFrequency parameter)
- **Baud rate**: 115200 (BaudRate parameter)
- **FIFOs**: 128-byte RX, 128-byte TX

| Offset | Name     | R/W | Bits  | Description                              |
|--------|----------|-----|-------|------------------------------------------|
| 0x00   | RX       | R   | [7:0] | Read byte from RX FIFO (auto-dequeue)    |
| 0x04   | TX       | W   | [7:0] | Write byte to TX FIFO                    |
| 0x08   | STATUS   | R   | [1:0] | bit 0: RX FIFO empty; bit 1: TX FIFO full|

**IRQ**: `uart_irq_o` = high whenever RX FIFO is **not empty** (level-triggered).
Connected to `irq_fast_i[0]` → mcause bit 16.

### 5.2 Timer (`timer.sv`)

- **Base**: `0x80000000`
- **Width**: 64-bit mtime / mtimecmp

| Offset | Name       | R/W | Description                         |
|--------|------------|-----|-------------------------------------|
| 0x00   | MTIME      | R/W | Lower 32 bits of mtime counter      |
| 0x04   | MTIMEH     | R/W | Upper 32 bits of mtime counter      |
| 0x08   | MTIMECMP   | R/W | Lower 32 bits of compare value      |
| 0x0C   | MTIMECMPH  | R/W | Upper 32 bits of compare value      |

**Behavior**:
- `mtime` increments **every clock cycle** (50 MHz → 20 ns resolution)
- IRQ asserted when `mtime >= mtimecmp`, cleared by writing to `mtimecmp`
- Connected to `irq_timer_i` → standard RISC-V machine timer interrupt (mcause bit 7)

**Important for Zephyr**: This is NOT a memory-mapped CLINT. The timer address
`0x80000000` does NOT follow the SiFive CLINT layout. The register offsets are
different: CLINT has mtime at +0xBFF8, but here mtime is at +0x0.

### 5.3 GPIO (`gpio.sv`)

- **Base**: `0x80002000`
- **Input width**: 8 bits (GpiWidth), with debouncing (~10 µs at 500 clock cycles)
- **Output width**: 16 bits (GpoWidth) — 8 bits on Arty A7 (GpoWidth=8 at top)

| Offset | Name      | R/W | Bits           | Description                      |
|--------|-----------|-----|----------------|----------------------------------|
| 0x00   | OUT       | R/W | [GpoWidth-1:0] | GPIO output register            |
| 0x04   | IN        | R   | [GpiWidth-1:0] | Raw GPIO input (3-stage sync)   |
| 0x08   | IN_DBNC   | R   | [GpiWidth-1:0] | Debounced GPIO input            |

**No IRQ** from GPIO.

### 5.4 PWM (`pwm_wrapper`)

- **Base**: `0x80003000`
- **Channels**: 12 (PwmWidth=12)
- **Counter size**: 8-bit

Each channel has registers at offset `channel_index * 8`:

| Offset (per ch.) | Name    | R/W | Description          |
|-------------------|---------|-----|----------------------|
| +0x00             | WIDTH   | R/W | Pulse width value    |
| +0x04             | COUNTER | R   | Current counter val  |

**No IRQ** from PWM.

---

## 6. Interrupt Map

The Ibex core supports the following interrupt inputs:

| Signal            | mcause bit | Connected to     | Notes                        |
|-------------------|-----------|------------------|------------------------------|
| `irq_software_i`  | 3         | tied to `0`      | Not used                     |
| `irq_timer_i`     | 7         | Timer module     | mtime >= mtimecmp            |
| `irq_external_i`  | 11        | tied to `0`      | Not used                     |
| `irq_fast_i[0]`   | 16        | UART RX not empty| Level-triggered              |
| `irq_fast_i[1:14]`| 17-30     | tied to `0`      | Available for expansion      |
| `irq_nm_i`        | NMI       | tied to `0`      | Not used                     |

**Vector table layout** (vectored mode, `.vectors` section at `0x00100000`):
- Entry N (offset N×4): `jal x0, handler_N` for exceptions/interrupts 0..31
- Entry at offset 0x80: reset vector

---

## 7. Clock and Reset

### Clock

- **External input**: `IO_CLK` = 100 MHz (on Arty A7)
- **System clock**: `clk_sys` = **50 MHz**, generated by `clkgen_xil7series`
  (Xilinx MMCM/PLL-based clock generator)
- All peripherals run at `clk_sys` (50 MHz)

### Reset

- **External input**: `IO_RST_N` (active-low push button)
- Synchronized by `clkgen_xil7series` → `rst_sys_n`
- Core reset: `rst_core_n = rst_sys_n & ~ndmreset_req`
  - `ndmreset_req` comes from the Debug Module (allows debugger-initiated reset)

---

## 8. SRAM (Dual-Port RAM)

**Module**: `ram_2p`

- **Depth**: 128 KiB / 4 = 32768 words
- **Port A**: Connected to bus device `Ram` — data read/write
- **Port B**: Connected to instruction fetch path — read-only
- **Init**: Can be pre-loaded via `SRAMInitFile` parameter (Verilog `$readmemh`)

This dual-port design means instruction fetch and data access can occur
simultaneously without stalling, as long as data accesses target RAM.

---

## 9. Debug Module

**Module**: `dm_top` (from PULP `pulp_riscv_dbg`)

- **Base**: `0x1A110000`
- **Interface**: DMI (Debug Module Interface) — exposed via JTAG or DPI in simulation
- 1 hart supported
- Provides:
  - System Bus Access (SBA) — acts as bus host `DbgHost`
  - Debug memory for execution-based debug
  - Halt/resume/step control
  - `ndmreset` output for non-debug-module reset

**On the Arty A7 FPGA top** (`top_artya7.sv`): The DMI signals are **tied off**
(not connected). Debug is only functional in Verilator simulation where DPI
modules provide the JTAG interface.

---

## 10. FPGA Board Mapping (Arty A7)

| FPGA Signal   | Width | Board Feature          | SoC Connection        |
|---------------|-------|------------------------|-----------------------|
| `IO_CLK`      | 1     | 100 MHz oscillator     | Clock generator input |
| `IO_RST_N`    | 1     | Reset button           | System reset          |
| `SW[3:0]`     | 4     | DIP switches           | GP_I[7:4]             |
| `BTN[3:0]`    | 4     | Push buttons           | GP_I[3:0]             |
| `LED[3:0]`    | 4     | Green LEDs             | GP_O[7:4]             |
| `DISP_CTRL[3:0]`| 4  | Display control        | GP_O[3:0]             |
| `RGB_LED[11:0]`| 12   | RGB LEDs               | PWM_O[11:0]           |
| `UART_RX`     | 1     | USB-UART RX            | UART RX               |
| `UART_TX`     | 1     | USB-UART TX            | UART TX               |
| `SPI_RX/TX/SCK`| 3    | SPI (LCD)              | Not connected in core |

**Note**: On Arty A7, `GpoWidth` is set to **8** (not 16 as in the default
parameter). This means only 8 output bits are active: 4 LEDs + 4 display control.

---

## 11. Key Differences from Standard Platforms (Zephyr Porting Notes)

1. **No CLINT/PLIC**: The timer is at a custom address (`0x80000000`) with
   non-standard register offsets. Fast interrupts replace PLIC functionality.
   Zephyr's `riscv_machine_timer` driver will need a custom compatible or
   the timer driver must be written from scratch.

2. **Custom bus protocol**: Not TL-UL, not AXI, not Wishbone. The bus is a
   simple req/gnt/rvalid handshake. This is transparent to software (just
   MMIO), but important for understanding timing.

3. **No external interrupt controller**: Only 15 fast interrupt lines
   (`irq_fast_i[0:14]`), directly wired. Only `irq_fast_i[0]` (UART) is used.
   Ibex handles these via mcause values 16-30.

4. **Instruction fetch path is separate**: Not through the bus. Only RAM and
   debug memory are fetchable. Code must reside in SRAM.

5. **Single 128 KiB SRAM for everything**: Code, data, stack, heap all share
   the same 128 KiB memory. Zephyr minimal footprint (~20-40 KiB) fits, but
   this constrains application size.

6. **50 MHz system clock**: All timing calculations (baud rates, timer
   resolution) are based on this.

7. **Custom UART**: Not 16550, not OpenTitan UART. Three registers only
   (RX, TX, STATUS). Need a custom Zephyr UART driver.

8. **Boot at 0x00100000 + 0x80**: The vector table is at `0x00100000`, and the
   reset vector jumps to `reset_handler` from offset `0x80`.

---

## 12. File Reference

| What                     | Path (relative to hw/Secure-Ibex/)                          |
|--------------------------|-------------------------------------------------------------|
| SoC top-level RTL        | `rtl/system/ibex_reference_system_core.sv`                  |
| FPGA top (Arty A7)       | `rtl/fpga/top_artya7.sv`                                    |
| Bus interconnect         | `vendor/lowrisc_ibex/shared/rtl/bus.sv`                     |
| UART RTL                 | `rtl/system/uart.sv`                                        |
| GPIO RTL                 | `rtl/system/gpio.sv`                                        |
| Timer RTL                | `vendor/lowrisc_ibex/shared/rtl/timer.sv`                   |
| PWM wrapper RTL          | (in rtl/system/, pwm_wrapper + pwm submodules)              |
| SW register defines      | `sw/Standard_Ibex_Only_SW/Demo_System_Examples/c/common/reference_system_regs.h` |
| SW UART driver           | `sw/Standard_Ibex_Only_SW/Demo_System_Examples/c/common/uart.h` / `.c` |
| SW Timer driver          | `sw/Standard_Ibex_Only_SW/Demo_System_Examples/c/common/timer.h` / `.c` |
| SW GPIO driver           | `sw/Standard_Ibex_Only_SW/Demo_System_Examples/c/common/gpio.h` / `.c` |
| Startup code (crt0)      | `sw/Standard_Ibex_Only_SW/Demo_System_Examples/c/common/crt0.S` |
| Linker script            | `sw/Standard_Ibex_Only_SW/Demo_System_Examples/common/link.ld` |
| SVD device description   | `data/ibex.svd`                                              |
| FuseSoC core file        | `ibex_reference_system_core.core`                            |
