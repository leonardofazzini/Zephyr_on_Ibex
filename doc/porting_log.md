# Porting Log: Zephyr RTOS on Secure-Ibex

This document describes in detail every step taken to port Zephyr RTOS to the Secure-Ibex SoC (built around the lowRISC Ibex RISC-V core), from understanding the hardware all the way to a working Verilator simulation.


## Phase 1 — Hardware study

Before writing any Zephyr code, a thorough understanding of the SoC architecture was necessary. The main reference file is:

[`hw/Secure-Ibex/rtl/system/ibex_reference_system_core.sv`](../hw/Secure-Ibex/rtl/system/ibex_reference_system_core.sv)

### What was analyzed

1. **Memory map** — Read the `localparam` declarations in the top-level RTL to extract base addresses and sizes for every peripheral.

2. **Peripherals** — Read each RTL module
   ([`uart.sv`](../hw/Secure-Ibex/rtl/system/uart.sv),
   [`gpio.sv`](../hw/Secure-Ibex/rtl/system/gpio.sv),
   [`timer.sv`](../hw/Secure-Ibex/vendor/lowrisc_ibex/shared/rtl/timer.sv),
   [`pwm_wrapper.sv`](../hw/Secure-Ibex/rtl/system/pwm_wrapper.sv))
   to understand registers, FIFO depths, protocol, and interrupt behavior.

3. **Bus** — The bus ([`bus.sv`](../hw/Secure-Ibex/vendor/lowrisc_ibex/shared/rtl/bus.sv)) is a custom interconnect with fixed-priority arbitration, req/gnt/rvalid handshake, 32-bit data and address. It is NOT TL-UL, NOT AXI.

4. **CPU** — Ibex configured as RV32IMC (no bit-manipulation), RegFileFPGA, fast multiply, 10 HPM counters, 2 hardware breakpoints. Boots at `0x00100000`.

5. **Interrupts** — No PLIC. Only Ibex fast interrupts (mcause 16-30). Timer on mcause 7 (standard RISC-V). UART on mcause 16 (fast IRQ 0).

6. **Clock** — IO_CLK 100 MHz -> `clkgen_xil7series` -> clk_sys 50 MHz.

7. **FPGA top** — [`top_artya7.sv`](../hw/Secure-Ibex/rtl/fpga/top_artya7.sv) instantiates the core with GpoWidth=8, GpiWidth=8, PwmWidth=12. DMI is not connected (tied off).

The output of this analysis is:
[`doc/secure_ibex_hw_description.md`](secure_ibex_hw_description.md)

### Key differences from OpenTitan

[OpenTitan EarlGrey](https://docs.zephyrproject.org/latest/boards/lowrisc/opentitan_earlgrey/doc/index.html) is the only other Zephyr target based on Ibex, but the SoC is very different:

| Aspect | OpenTitan | Secure-Ibex |
|---|---|---|
| Timer | Standard CLINT + AON | Custom @ 0x80000000 |
| Interrupt controller | SiFive PLIC | No PLIC, fast IRQ only |
| UART | `lowrisc,opentitan-uart` (TL-UL) | Custom 3-register |
| Bus | TileLink (TL-UL) | Custom req/gnt bus |
| Memory | SRAM + Flash + ROM | SRAM only, 128 KiB |
| ROM start offset | 0x400 (manifest header) | 0x0 (no header) |

Consequence: OpenTitan drivers cannot be reused. Custom UART driver and new DTS bindings are required.

---

## Phase 2 — Zephyr environment setup

### System packages

```
sudo apt install -y device-tree-compiler gperf python3-venv cmake ninja-build
```

### Python virtualenv and west

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install west
```

### Zephyr workspace

```bash
west init -m https://github.com/zephyrproject-rtos/zephyr --mr main zephyr-workspace
cd zephyr-workspace && west update
pip install -r zephyr/scripts/requirements.txt
```

This downloads Zephyr (v4.3.99, main branch) and all modules (~2 GB). The workspace lives in `zephyr-workspace/` and is listed in [`.gitignore`](../.gitignore) since it is managed by west, not by our repo.

### Zephyr SDK

SDK 0.17.0 was tried first but is incompatible with Zephyr main (requires SDK >= 1.0). Installed SDK 1.0.0:

```bash
wget .../zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz
tar xf ... && cd zephyr-sdk-1.0.0 && ./setup.sh
mv /tmp/zephyr-sdk-1.0.0 ~/.local/zephyr-sdk-1.0.0
```

The minimal SDK 1.0.0 already includes the `riscv64-zephyr-elf` toolchain.

### Environment verification

Test build on `qemu_riscv32`:

```bash
west build -b qemu_riscv32 zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
```

The `-DDTC=/usr/bin/dtc` flag is necessary because the `dtc` bundled in the SDK has dynamic library issues on Ubuntu 24.04. Using the system dtc (`apt install device-tree-compiler`) resolves the problem.

---

## Phase 3 — SoC definition

### How Zephyr discovers hardware

Zephyr has a layered hardware abstraction. From bottom to top:

1. **Architecture** (`arch/riscv/`) — shared RISC-V code: context switching interrupt entry, CSR access. *Already in Zephyr, nothing to write*.
2. **SoC** (`soc/`) — describes a specific chip: what ISA extensions it has, clock speed, number of interrupts, linker script. **We must create this.**
3. **Board** (`boards/`) — describes a specific PCB: which SoC it uses, which peripherals are enabled, pin assignments. **We must create this.**
4. **Device Tree** (`dts/`) — machine-readable description of the memory map and peripheral configuration. **We must create this.**
5. **Drivers** (`drivers/`) — code that talks to peripherals described in the device tree. **We must create the UART driver** (the timer driver already exists in Zephyr).

Normally all these files live inside the Zephyr source tree. But Zephyr also supports **out-of-tree** definitions: we keep everything in our own repo and tell Zephyr where to find it. The mechanism is:

- [`zephyr/module.yml`](../zephyr/module.yml) at project root — a YAML file that tells Zephyr "this directory contains board definitions, SoC definitions, DTS files, and extra Kconfig/CMake for drivers". It registers `board_root`, `soc_root`, and `dts_root` all pointing to `.` (our project root).
- The environment variable `ZEPHYR_EXTRA_MODULES` passed at build time, which points to our project root so Zephyr loads the module.

### What the SoC definition must tell Zephyr

The SoC definition answers questions like:
- What architecture is this? (RISC-V)
- What privilege levels does it support? (Machine mode only)
- Does it have a PLIC? (No)
- How does it handle interrupts? (Vectored mode)
- What is the clock speed? (50 MHz)
- Where should the linker place code? (Standard RISC-V layout)

### [`soc/lowrisc/secure_ibex/`](../soc/lowrisc/secure_ibex/)

Five files were created. The directory structure follows Zephyr's convention: `soc/<vendor>/<soc_name>/`. We used `lowrisc` as vendor since Ibex is a lowRISC project.

[**`soc.yml`**](../soc/lowrisc/secure_ibex/soc.yml) — Declares the SoC name so Zephyr's build system can discover it:
```yaml
socs:
- name: secure_ibex
```

[**`Kconfig.soc`**](../soc/lowrisc/secure_ibex/Kconfig.soc) — Defines a boolean Kconfig symbol `SOC_SECURE_IBEX`. When a board selects this SoC, this symbol becomes `y` and activates all SoC-specific configuration:
```kconfig
config SOC_SECURE_IBEX
    bool
config SOC
    default "secure_ibex" if SOC_SECURE_IBEX
```

[**`Kconfig`**](../soc/lowrisc/secure_ibex/Kconfig) — The core of the SoC definition. When `SOC_SECURE_IBEX` is selected, it automatically pulls in a set of architectural features via `select` statements:
```kconfig
config SOC_SECURE_IBEX
    select ATOMIC_OPERATIONS_C
    select INCLUDE_RESET_VECTOR
    select RISCV
    select RISCV_PRIVILEGED
    select RISCV_VECTORED_MODE
    select GEN_IRQ_VECTOR_TABLE
```

Each `select` has a specific reason:

| Symbol | Why |
|---|---|
| `RISCV` | Tells Zephyr this is a RISC-V chip, so it uses `arch/riscv/` |
| `RISCV_PRIVILEGED` | Ibex implements the RISC-V privileged ISA (M-mode CSRs like `mstatus`, `mtvec`, `mepc`) |
| `RISCV_VECTORED_MODE` | Ibex's `mtvec` register is hardwired to vectored mode — each interrupt has its own entry in the vector table. Without this, Zephyr would try to set `mtvec` to direct mode, which Ibex ignores |
| `GEN_IRQ_VECTOR_TABLE` | Tells Zephyr to auto-generate the interrupt vector table at link time, filling it with handler addresses |
| `INCLUDE_RESET_VECTOR` | Tells Zephyr to include its own reset vector code (the `__reset` symbol that jumps to `__initialize`), so we don't need a custom startup assembly file |
| `ATOMIC_OPERATIONS_C` | Ibex only implements RV32IMC — it does **not** have the "A" (atomic) extension. Without this, Zephyr would emit `lr.w`/`sc.w` instructions that Ibex cannot execute. This flag tells Zephyr to emulate atomics using interrupt-disable/enable pairs in C |

What we deliberately did **NOT** select:
- **`RISCV_HAS_PLIC`** — Secure-Ibex has no Platform-Level Interrupt Controller. Interrupts go directly to the CPU via fast interrupt lines. Omitting this means Zephyr won't try to initialize a PLIC.

[**`Kconfig.defconfig`**](../soc/lowrisc/secure_ibex/Kconfig.defconfig) — Sets numeric defaults that other parts of Zephyr need:
```kconfig
config SYS_CLOCK_HW_CYCLES_PER_SEC
    default 50000000           # 50 MHz
config NUM_IRQS
    default 32                 # 32 interrupt lines (mcause 0-31)
```

- `SYS_CLOCK_HW_CYCLES_PER_SEC` = 50000000 because the system clock is 50 MHz (100 MHz FPGA oscillator divided by 2 via `clkgen_xil7series`). Zephyr uses this to convert between clock cycles and time units (ms, us). The timer driver reads this value to calculate how many cycles correspond to one OS tick.
- `NUM_IRQS` = 32 because Ibex supports mcause values 0-31 (16 standard + 16 fast interrupts). Zephyr allocates an ISR table of this size.

[**`CMakeLists.txt`**](../soc/lowrisc/secure_ibex/CMakeLists.txt) — Tells the build system which linker script to use:
```cmake
set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld
    CACHE INTERNAL "")
```

We use Zephyr's standard RISC-V linker script rather than writing a custom one. This script reads the memory layout from the device tree (`ram0` node) and places `.text`, `.rodata`, `.data`, `.bss`, and the stack in the right regions. Since our SoC has a single SRAM starting at `0x00100000`, and no flash (XIP is disabled), the linker puts everything in that SRAM.

---

## Phase 4 — Board definition

### SoC vs Board: what's the difference?

In Zephyr's model:
- The **SoC** defines the silicon — what the chip can do.
- The **Board** defines a specific product — which SoC is on it, which peripherals are actually connected and enabled, and any board-specific defaults.

For example, the Secure-Ibex SoC has a UART *capability*, but the board definition is what says "yes, UART0 is connected and should be used as the console". A different board using the same SoC could choose not to enable the UART, or could use a different peripheral as the console.

### [`boards/lowrisc/secure_ibex/`](../boards/lowrisc/secure_ibex/)

The directory structure follows `boards/<vendor>/<board_name>/`.

[**`board.yml`**](../boards/lowrisc/secure_ibex/board.yml) — Metadata that Zephyr's build system uses to discover and identify the board. The `socs` field links this board to our SoC definition:
```yaml
board:
  name: secure_ibex
  full_name: Secure-Ibex Reference System on Arty A7
  vendor: lowrisc
  socs:
  - name: secure_ibex
```

[**`secure_ibex.yaml`**](../boards/lowrisc/secure_ibex/secure_ibex.yaml) — Hardware profile used by Zephyr's test infrastructure to decide which tests can run on this board:
```yaml
identifier: secure_ibex
name: Secure-Ibex Reference System
type: mcu
arch: riscv
toolchain:
  - zephyr
ram: 128
```

The `ram: 128` tells the test framework this board has 128 KiB of RAM, so tests that require more memory will be skipped.

[**`Kconfig.secure_ibex`**](../boards/lowrisc/secure_ibex/Kconfig.secure_ibex) — The glue between board and SoC. When the user passes `-b secure_ibex` to the build, Zephyr sets `BOARD_SECURE_IBEX=y`, which in turn pulls in the entire SoC configuration:
```kconfig
config BOARD_SECURE_IBEX
    select SOC_SECURE_IBEX
```

[**`secure_ibex_defconfig`**](../boards/lowrisc/secure_ibex/secure_ibex_defconfig) — Default Kconfig values applied when building for this board. These are the minimum settings needed for a working hello_world:
```
CONFIG_SERIAL=y          # Enable the serial subsystem
CONFIG_CONSOLE=y         # Enable console output
CONFIG_UART_CONSOLE=y    # Use UART as the console backend
CONFIG_BUILD_OUTPUT_BIN=y  # Generate a .bin file in addition to .elf
CONFIG_XIP=n             # No execute-in-place: we have no flash, code
                         # runs from SRAM (loaded at boot)
```

`CONFIG_XIP=n` is critical. XIP (execute-in-place) means code stays in flash and only data is copied to RAM. Our system has no flash — the entire firmware is loaded into SRAM by the simulator or FPGA loader. With `XIP=n`, the linker puts both code and data in the same RAM region.

[**`secure_ibex.dts`**](../boards/lowrisc/secure_ibex/secure_ibex.dts) — The board-level device tree. It includes the SoC-level `.dtsi` and then makes board-specific choices:
```dts
/dts-v1/;
#include <lowrisc/secure_ibex.dtsi>

/ {
    model = "Secure-Ibex Reference System on Arty A7";
    chosen {
        zephyr,console = &uart0;     /* printf goes here */
        zephyr,shell-uart = &uart0;  /* Zephyr shell goes here */
        zephyr,sram = &ram0;         /* main memory for code + data */
    };
};

&uart0 { status = "okay"; };   /* enable the UART */
&mtimer { status = "okay"; };  /* enable the timer */
```

The `chosen` node is how Zephyr knows which peripheral to use for what. Without `zephyr,sram = &ram0`, Zephyr wouldn't know where to place the heap and stack. Without `zephyr,console = &uart0`, `printk()` would have nowhere to output.

Peripherals in the `.dtsi` are `status = "disabled"` by default. The board `.dts` selectively enables the ones that are actually wired up on this particular board. This pattern allows the same SoC `.dtsi` to be reused by different boards that may not use all peripherals.

---

## Phase 5 — SoC device tree

### What is a device tree and why do we need one?

The device tree is a data structure (not code) that describes hardware. Zephyr reads it at build time to:
- Know the memory layout (where RAM starts and how big it is)
- Generate C macros for peripheral base addresses, interrupt numbers, etc.
- Automatically instantiate drivers for peripherals that are enabled
- Configure the linker script

The device tree has two layers:
- **`.dtsi`** (device tree source include) — describes the SoC hardware. Shared across all boards that use this SoC.
- **`.dts`** (device tree source) — board-specific. Includes the `.dtsi` and overrides/enables things for a specific board.

### [`dts/riscv/lowrisc/secure_ibex.dtsi`](../dts/riscv/lowrisc/secure_ibex.dtsi)

This is the SoC-level device tree. Every value was extracted from the RTL source ([`ibex_reference_system_core.sv`](../hw/Secure-Ibex/rtl/system/ibex_reference_system_core.sv)), specifically the `localparam` declarations for base addresses and sizes.

```dts
/ {
    cpus {
        cpu@0 {
            compatible = "lowrisc,ibex", "riscv";
            riscv,isa-base = "rv32i";
            riscv,isa-extensions = "i", "m", "c", "zicsr", "zifencei";

            hlic: interrupt-controller {
                compatible = "riscv,cpu-intc";
            };
        };
    };

    soc {
        ram0: memory@100000 {
            reg = <0x00100000 0x20000>;      /* 128 KiB */
        };

        mtimer: timer@80000000 {
            compatible = "lowrisc,ibex-timer";
            reg = <0x80000000 0x10>;
            interrupts-extended = <&hlic 7>;
            status = "disabled";
        };

        uart0: serial@80001000 {
            compatible = "lowrisc,ibex-uart";
            reg = <0x80001000 0x1000>;
            interrupts-extended = <&hlic 16>;
            status = "disabled";
        };

        /* gpio0, pwm0 also defined but omitted here for brevity */
    };
};
```

How each node maps to the RTL:

| DTS node | RTL source | How the value was found |
|---|---|---|
| `ram0: reg = <0x00100000 0x20000>` | `MEM_START = 32'h00100000`, `MEM_SIZE = 128 * 1024` | Lines 39-40 of `ibex_reference_system_core.sv` |
| `mtimer: reg` | `TIMER_START = 32'h80000000` | Line 43. Register offsets 0x0/0x4/0x8/0xC from [`timer.sv`](../hw/Secure-Ibex/vendor/lowrisc_ibex/shared/rtl/timer.sv) |
| `uart0: reg = <0x80001000 0x1000>` | `UART_START = 32'h80001000` | Line 51 |
| `interrupts-extended = <&hlic 7>` | `.irq_timer_i(timer_irq)` | Line 268. mcause 7 is the standard RISC-V machine timer interrupt |
| `interrupts-extended = <&hlic 16>` | `.irq_fast_i({14'b0, uart_irq})` | Line 269. The UART IRQ is fast interrupt 0, which maps to mcause 16 |

The `compatible` strings are important: they are the key that Zephyr uses to match a device tree node to a driver. `"lowrisc,ibex-timer"` matches our custom timer driver. `"lowrisc,ibex-uart"` matches our custom UART driver.

### Timer: custom driver (`lowrisc,ibex-timer`)

The Secure-Ibex timer has a non-standard register layout compared to the SiFive CLINT (which many RISC-V boards use). The CLINT has mtime at offset +0xBFF8 and mtimecmp at +0x4000 from its base. Our timer has:

| Register | Offset | RTL constant |
|---|---|---|
| mtime low | +0x0 | `MTIME_LOW = 0` |
| mtime high | +0x4 | `MTIME_HIGH = 4` |
| mtimecmp low | +0x8 | `MTIMECMP_LOW = 8` |
| mtimecmp high | +0xC | `MTIMECMP_HIGH = 12` |

While Zephyr's built-in `riscv,machine-timer` driver could work (it reads absolute addresses from `reg-names` in the DTS), we use a custom driver ([`drivers/timer/timer_ibex.c`](../drivers/timer/timer_ibex.c)) to have full control over the hardware. The driver accesses the four 32-bit registers directly via fixed offsets from the base address `0x80000000`, matching the RTL layout exactly. This makes it easy to add future features like mtime reset, custom tick scaling, or power management hooks.

The DTS node uses a single `reg` entry covering all 16 bytes of the register space:
```dts
reg = <0x80000000 0x10>;
```

The driver implements the full Zephyr system clock API: `sys_clock_set_timeout()`, `sys_clock_elapsed()`, `sys_clock_cycle_get_32()`, `sys_clock_cycle_get_64()`, with tickless support and 64-bit cycle counter.

---

## Phase 6 — Custom UART driver

### Why a custom driver is needed

The Secure-Ibex UART (defined in [`hw/Secure-Ibex/rtl/system/uart.sv`](../hw/Secure-Ibex/rtl/system/uart.sv)) is very different from any UART that Zephyr already has drivers for:

| Feature | Ibex UART | OpenTitan UART | 16550 (most common) |
|---|---|---|---|
| Registers | 3 | ~12 | 8+ |
| Baud config | Hardware-fixed | Software NCO | Software divisor |
| FIFO | 128B RX, 128B TX | Configurable | 16B typical |
| IRQ control | None (always fires) | Per-event enable | IER register |

There are only 3 registers, at offsets from the base address `0x80001000`:

| Offset | Name | R/W | Bits | Description |
|---|---|---|---|---|
| 0x0 | RX | R | [7:0] | Read dequeues one byte from the 128-byte RX FIFO |
| 0x4 | TX | W | [7:0] | Write enqueues one byte into the 128-byte TX FIFO |
| 0x8 | STATUS | R | [1:0] | bit 0: RX FIFO is empty (1=empty, 0=data available); bit 1: TX FIFO is full (1=full, 0=space available) |

The baud rate is **not configurable in software**. It is set at synthesis time by the RTL parameters `ClockFrequency` (50 MHz) and `BaudRate` (115200). The hardware computes `ClocksPerBaud = 50000000 / 115200 = 434` and uses that as a fixed divider. This means the driver's `init()` function has nothing to configure.

### Step 1: DTS binding

Before writing the driver, we need to tell Zephyr what the `compatible` string `"lowrisc,ibex-uart"` means and what properties the DTS node should have.

[**`dts/bindings/serial/lowrisc,ibex-uart.yaml`**](../dts/bindings/serial/lowrisc,ibex-uart.yaml):

```yaml
compatible: "lowrisc,ibex-uart"
include: uart-controller.yaml    # inherits standard UART properties
                                 # (current-speed, etc.)
properties:
  reg:
    required: true               # base address is mandatory
```

The `include: uart-controller.yaml` pulls in standard properties like `current-speed` from Zephyr's base UART binding. The `reg` property is required because the driver needs to know the base address.

### Step 2: Driver code

[**`drivers/serial/uart_ibex.c`**](../drivers/serial/uart_ibex.c)

A Zephyr UART driver must implement the `uart_driver_api` interface. For a minimal console, only two functions are needed:

**`poll_out(dev, c)`** — send one byte, blocking until there is space:
```c
static void uart_ibex_poll_out(const struct device *dev, unsigned char c)
{
    const struct uart_ibex_config *cfg = dev->config;

    /* Spin until TX FIFO has space (STATUS bit 1 = 0) */
    while (sys_read32(cfg->base + UART_STATUS_REG) & UART_STATUS_TX_FULL) {
    }

    sys_write32((uint32_t)c, cfg->base + UART_TX_REG);
}
```

**`poll_in(dev, c)`** — try to receive one byte, return -1 if nothing:
```c
static int uart_ibex_poll_in(const struct device *dev, unsigned char *c)
{
    const struct uart_ibex_config *cfg = dev->config;

    if (sys_read32(cfg->base + UART_STATUS_REG) & UART_STATUS_RX_EMPTY) {
        return -1;    /* no data available */
    }

    *c = (unsigned char)sys_read32(cfg->base + UART_RX_REG);
    return 0;
}
```

The driver uses `sys_read32()` / `sys_write32()` which are Zephyr's portable MMIO accessors — they compile to a simple volatile pointer dereference on RISC-V.

**Auto-instantiation**: The macro at the bottom of the file:
```c
#define UART_IBEX_INIT(n) ...
DT_INST_FOREACH_STATUS_OKAY(UART_IBEX_INIT)
```

This iterates over every DTS node with `compatible = "lowrisc,ibex-uart"` and `status = "okay"`, and for each one creates a device instance with the base address from `reg`. In our case there is one: `uart0 @ 0x80001000`.

### Step 3: Kconfig and CMake

[**`drivers/serial/Kconfig.ibex_uart`**](../drivers/serial/Kconfig.ibex_uart) — Defines the `CONFIG_UART_IBEX` option:
```kconfig
config UART_IBEX
    bool "Ibex Reference System UART driver"
    default y
    depends on DT_HAS_LOWRISC_IBEX_UART_ENABLED
    select SERIAL_HAS_DRIVER
```

The `depends on DT_HAS_LOWRISC_IBEX_UART_ENABLED` means this option only appears if the device tree has at least one `lowrisc,ibex-uart` node with `status = "okay"`. Zephyr auto-generates `DT_HAS_*` symbols from the DTS. The `default y` means the driver is enabled automatically when the hardware is present — no manual configuration needed.

[**`drivers/serial/CMakeLists.txt`**](../drivers/serial/CMakeLists.txt) — Tells CMake to compile the driver when enabled:
```cmake
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_UART_IBEX uart_ibex.c)
```

### Step 4: Out-of-tree wiring

For Zephyr to discover our out-of-tree driver, three files at the project root connect everything together:

1. [**`zephyr/module.yml`**](../zephyr/module.yml) — registers this project as a Zephyr module with custom board, SoC, DTS, Kconfig, and CMake roots:
   ```yaml
   build:
     cmake: .
     kconfig: Kconfig
     settings:
       board_root: .
       soc_root: .
       dts_root: .
   ```

2. [**`Kconfig`**](../Kconfig) — top-level Kconfig that pulls in our driver's Kconfig:
   ```kconfig
   rsource "drivers/serial/Kconfig.ibex_uart"
   ```

3. [**`CMakeLists.txt`**](../CMakeLists.txt) — top-level CMake that adds our driver subdirectory:
   ```cmake
   if(CONFIG_UART_IBEX)
     add_subdirectory(drivers/serial)
   endif()
   ```

4. At build time, the environment variable `ZEPHYR_EXTRA_MODULES` points to our project root, so Zephyr loads the module and processes all of the above.

### How it all connects (build-time flow)

```
User runs: west build -b secure_ibex ...
  |
  v
Zephyr sees ZEPHYR_EXTRA_MODULES -> reads zephyr/module.yml
  |
  v
Discovers boards/lowrisc/secure_ibex/ -> loads secure_ibex.dts
  |
  v
DTS includes dts/riscv/lowrisc/secure_ibex.dtsi
  |
  v
DTS has node: uart0 { compatible = "lowrisc,ibex-uart"; status = "okay"; }
  |
  v
Zephyr generates: DT_HAS_LOWRISC_IBEX_UART_ENABLED=y
  |
  v
Kconfig: CONFIG_UART_IBEX=y (because default y + depends on DT_HAS_...)
  |
  v
CMake: compiles drivers/serial/uart_ibex.c
  |
  v
Driver macro DT_INST_FOREACH_STATUS_OKAY creates device "uart0" at 0x80001000
  |
  v
Board defconfig has CONFIG_UART_CONSOLE=y + chosen { zephyr,console = &uart0 }
  |
  v
printk("Hello World!") -> console subsystem -> uart_ibex_poll_out() -> MMIO write to 0x80001004
```

---

## Phase 7 — Custom timer driver

### Why a custom driver

Zephyr's built-in `riscv,machine-timer` driver (`drivers/timer/riscv_machine_timer.c`) could technically work with our hardware — it reads mtime/mtimecmp addresses from the device tree via `reg-names`, so it is not tied to the SiFive CLINT layout. However, using it means depending on an upstream driver that we cannot modify. A custom driver gives full control over the timer hardware, making it straightforward to add features like mtime reset, custom tick scaling, or power management hooks in the future.

### Hardware register map

From [`timer.sv`](../hw/Secure-Ibex/vendor/lowrisc_ibex/shared/rtl/timer.sv):

| Offset | Name | R/W | Description |
|---|---|---|---|
| 0x00 | MTIME_LOW | R/W | Lower 32 bits of the 64-bit mtime counter |
| 0x04 | MTIME_HIGH | R/W | Upper 32 bits of mtime |
| 0x08 | MTIMECMP_LOW | R/W | Lower 32 bits of the 64-bit compare value |
| 0x0C | MTIMECMP_HIGH | R/W | Upper 32 bits of mtimecmp |

Key behaviors:
- `mtime` increments every clock cycle (50 MHz = 20 ns resolution)
- IRQ asserted when `mtime >= mtimecmp`, cleared by writing to `mtimecmp`
- Connected to `irq_timer_i` → mcause 7 (standard RISC-V machine timer interrupt)
- Registers are NOT internally latched — multi-word writes need careful sequencing

### Step 1: DTS binding

[**`dts/bindings/timer/lowrisc,ibex-timer.yaml`**](../dts/bindings/timer/lowrisc,ibex-timer.yaml):

```yaml
compatible: "lowrisc,ibex-timer"
include: base.yaml
properties:
  reg:
    required: true
  interrupts-extended:
    required: true
```

Unlike the upstream driver (which needs two separate `reg` entries with `reg-names` for mtime and mtimecmp), our binding uses a single `reg` covering all 16 bytes because we access registers via fixed offsets from the base address.

### Step 2: DTS node update

The timer node in [`secure_ibex.dtsi`](../dts/riscv/lowrisc/secure_ibex.dtsi) was changed from:
```dts
mtimer: timer@80000000 {
    compatible = "riscv,machine-timer";
    reg = <0x80000000 0x8 0x80000008 0x8>;
    reg-names = "mtime", "mtimecmp";
    interrupts-extended = <&hlic 7>;
};
```
to:
```dts
mtimer: timer@80000000 {
    compatible = "lowrisc,ibex-timer";
    reg = <0x80000000 0x10>;
    interrupts-extended = <&hlic 7>;
    status = "disabled";
};
```

The timer is `disabled` by default in the `.dtsi` (like the UART) and enabled in the board `.dts` with `&mtimer { status = "okay"; };`.

### Step 3: Driver code

[**`drivers/timer/timer_ibex.c`**](../drivers/timer/timer_ibex.c)

A Zephyr system clock driver is not a standard device driver — it implements a set of global functions that the kernel calls directly. There is no device API struct; instead the driver exports:

| Function | Purpose |
|---|---|
| `sys_clock_set_timeout(ticks, idle)` | Schedule next timer interrupt (tickless mode) |
| `sys_clock_elapsed()` | Return ticks elapsed since last announce |
| `sys_clock_cycle_get_32()` | Return current 32-bit cycle count |
| `sys_clock_cycle_get_64()` | Return current 64-bit cycle count |

The driver is initialized via `SYS_INIT()` at `PRE_KERNEL_2` priority, which registers the ISR and schedules the first tick.

**Reading mtime** — requires a rollover guard because the 64-bit counter is read as two 32-bit words:
```c
static uint64_t ibex_mtime(void)
{
    volatile uint32_t *base = (volatile uint32_t *)TIMER_BASE;
    uint32_t lo, hi;

    do {
        hi = sys_read32((mem_addr_t)&base[MTIME_HIGH_REG / 4]);
        lo = sys_read32((mem_addr_t)&base[MTIME_LOW_REG / 4]);
    } while (sys_read32((mem_addr_t)&base[MTIME_HIGH_REG / 4]) != hi);

    return ((uint64_t)hi << 32) | lo;
}
```

**Writing mtimecmp** — the high word must be set to `0xFFFFFFFF` first to prevent a spurious interrupt between the two writes:
```c
static void ibex_set_mtimecmp(uint64_t value)
{
    sys_write32(0xFFFFFFFF, TIMER_BASE + MTIMECMP_HIGH_REG);
    sys_write32((uint32_t)value, TIMER_BASE + MTIMECMP_LOW_REG);
    sys_write32((uint32_t)(value >> 32), TIMER_BASE + MTIMECMP_HIGH_REG);
}
```

**The ISR** — reads the current cycle count, calculates elapsed ticks, advances the tracking state, and announces ticks to the kernel:
```c
static void timer_isr(const void *arg)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    uint64_t now = ibex_mtime();
    uint32_t dticks = (cycle_diff_t)(now - last_count) / CYC_PER_TICK;

    last_count += (cycle_diff_t)dticks * CYC_PER_TICK;
    last_ticks += dticks;
    last_elapsed = 0;

    if (!IS_ENABLED(CONFIG_TICKLESS_KERNEL)) {
        ibex_set_mtimecmp(last_count + CYC_PER_TICK);
    }

    k_spin_unlock(&lock, key);
    sys_clock_announce(dticks);   /* must be called outside the spinlock */
}
```

`sys_clock_announce()` is the key kernel function: it tells the scheduler that `dticks` ticks have passed, which triggers timeout processing and context switches.

### Step 4: Kconfig and CMake

[**`drivers/timer/Kconfig.ibex_timer`**](../drivers/timer/Kconfig.ibex_timer):
```kconfig
config IBEX_TIMER
    bool "Ibex Reference System timer driver"
    default y
    depends on DT_HAS_LOWRISC_IBEX_TIMER_ENABLED
    select TICKLESS_CAPABLE
    select TIMER_HAS_64BIT_CYCLE_COUNTER
```

The `depends on DT_HAS_LOWRISC_IBEX_TIMER_ENABLED` works exactly like the UART driver: Zephyr auto-generates this symbol when the DTS has a `lowrisc,ibex-timer` node with `status = "okay"`. The old `RISCV_MACHINE_TIMER` is automatically excluded because the DTS no longer has any `riscv,machine-timer` node.

`TICKLESS_CAPABLE` declares that the driver supports tickless operation (variable-length tick intervals). `TIMER_HAS_64BIT_CYCLE_COUNTER` enables the 64-bit `sys_clock_cycle_get_64()` API.

[**`drivers/timer/CMakeLists.txt`**](../drivers/timer/CMakeLists.txt):
```cmake
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_IBEX_TIMER timer_ibex.c)
```

### Step 5: Out-of-tree wiring

The root [`Kconfig`](../Kconfig) and [`CMakeLists.txt`](../CMakeLists.txt) were extended with:
```kconfig
rsource "drivers/timer/Kconfig.ibex_timer"
```
```cmake
if(CONFIG_IBEX_TIMER)
  add_subdirectory(drivers/timer)
endif()
```

---

## Phase 8 — Build and verification

### Build command

```bash
cd zephyr-workspace
export ZEPHYR_EXTRA_MODULES=/path/to/Zephyr_on_Ibex
west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
```

### Result

Build succeeded on the first attempt:

```
[119/119] Linking C executable zephyr/zephyr.elf
Memory region         Used Size  Region Size  %age Used
             RAM:       15808 B       128 KB     12.06%
```

### ELF verification

```
$ riscv64-zephyr-elf-readelf -h zephyr.elf
  Class:                             ELF32
  Machine:                           RISC-V
  Entry point address:               0x100000
  Flags:                             0x1, RVC, soft-float ABI
```

- Entry point `0x100000` = SRAM start, correct
- RVC enabled (compressed instructions)
- Soft-float ABI (Ibex has no FPU)

---

## Phase 9 — Verilator simulation

### FuseSoC problem

The original Secure-Ibex build flow uses FuseSoC, but dependency resolution fails because of OpenTitan's `primgen` generator (used to generate primitive wrappers like `prim_ram_1p`, `prim_ram_2p`).

Error: `lowrisc_ibex_sim_shared was ignored because it depends on missing packages`

### Solution: Direct Makefile

Created [`Makefile.verilator`](../Makefile.verilator) (at the project root, not inside the submodule) which completely bypasses FuseSoC:

1. **Explicitly lists all RTL files** (~70 .sv files) in the correct compilation order (packages first, then primitives, then core, then peripherals, then top).

2. **Uses pre-existing prim wrappers** from `vendor/lowrisc_ibex/dv/uvm/core_ibex/common/prim/` instead of the primgen-generated ones.

3. **Generates `prim_ram_2p.sv` on the fly** — the only missing wrapper. The Makefile creates it as a module that instantiates `prim_generic_ram_2p`.

4. **Lists all C/C++ files** for the DPI testbench (~12 files: uartdpi, dmidpi, memutil, simctrl, pcounts, testbench).

5. **Verilator flags**: `--cc --exe --trace --trace-fst -Wno-fatal -DVERILATOR`

6. **Link flags**: `-pthread -lutil -lelf` (for PTY, ELF loading, TCP server)

### Fixes required during the build

1. **Missing prim_cipher_pkg.sv** — used by `prim_lfsr.sv`, added to the package list.

2. **Warnings treated as errors** — added `-Wno-fatal` to Verilator flags (27 warnings from upstream lowRISC code, not ours).

### Execution

```bash
make -f Makefile.verilator sim
```

The simulator:
- Loads the ELF directly into memory (uses segment LMAs)
- Instantiates a `uartdpi` module that creates a PTY and writes to `uart0.log`
- Executes the Ibex core cycle by cycle

### Result

```
*** Booting Zephyr OS build v4.3.0-9462-g44ace049cd41 ***
Hello World! secure_ibex/secure_ibex
```

Zephyr booted correctly, initialized the UART console, and printed the hello_world message.

Simulation statistics:
- 5,000,000 cycles simulated
- ~16,000 cycles to complete boot + print
- ~9,500 instructions executed
- Speed: ~1.7 MHz simulated

---

## Summary of created files

| File | Purpose |
|---|---|
| [`soc/lowrisc/secure_ibex/soc.yml`](../soc/lowrisc/secure_ibex/soc.yml) | SoC declaration |
| [`soc/lowrisc/secure_ibex/Kconfig.soc`](../soc/lowrisc/secure_ibex/Kconfig.soc) | SoC boolean option |
| [`soc/lowrisc/secure_ibex/Kconfig`](../soc/lowrisc/secure_ibex/Kconfig) | Architectural features |
| [`soc/lowrisc/secure_ibex/Kconfig.defconfig`](../soc/lowrisc/secure_ibex/Kconfig.defconfig) | Defaults (50 MHz, 32 IRQs) |
| [`soc/lowrisc/secure_ibex/CMakeLists.txt`](../soc/lowrisc/secure_ibex/CMakeLists.txt) | Linker script |
| [`boards/lowrisc/secure_ibex/board.yml`](../boards/lowrisc/secure_ibex/board.yml) | Board metadata |
| [`boards/lowrisc/secure_ibex/secure_ibex.yaml`](../boards/lowrisc/secure_ibex/secure_ibex.yaml) | Board HW profile |
| [`boards/lowrisc/secure_ibex/Kconfig.secure_ibex`](../boards/lowrisc/secure_ibex/Kconfig.secure_ibex) | Links board to SoC |
| [`boards/lowrisc/secure_ibex/secure_ibex.dts`](../boards/lowrisc/secure_ibex/secure_ibex.dts) | Board device tree |
| [`boards/lowrisc/secure_ibex/secure_ibex_defconfig`](../boards/lowrisc/secure_ibex/secure_ibex_defconfig) | Default config |
| [`dts/riscv/lowrisc/secure_ibex.dtsi`](../dts/riscv/lowrisc/secure_ibex.dtsi) | SoC device tree |
| [`dts/bindings/serial/lowrisc,ibex-uart.yaml`](../dts/bindings/serial/lowrisc,ibex-uart.yaml) | UART DTS binding |
| [`dts/bindings/timer/lowrisc,ibex-timer.yaml`](../dts/bindings/timer/lowrisc,ibex-timer.yaml) | Timer DTS binding |
| [`drivers/serial/uart_ibex.c`](../drivers/serial/uart_ibex.c) | UART driver (polling + interrupt-driven) |
| [`drivers/serial/Kconfig.ibex_uart`](../drivers/serial/Kconfig.ibex_uart) | UART driver Kconfig |
| [`drivers/serial/CMakeLists.txt`](../drivers/serial/CMakeLists.txt) | UART driver build |
| [`drivers/timer/timer_ibex.c`](../drivers/timer/timer_ibex.c) | Timer driver (system clock) |
| [`drivers/timer/Kconfig.ibex_timer`](../drivers/timer/Kconfig.ibex_timer) | Timer driver Kconfig |
| [`drivers/timer/CMakeLists.txt`](../drivers/timer/CMakeLists.txt) | Timer driver build |
| [`zephyr/module.yml`](../zephyr/module.yml) | Out-of-tree module registration |
| [`CMakeLists.txt`](../CMakeLists.txt) | Out-of-tree build entry point |
| [`Kconfig`](../Kconfig) | Out-of-tree Kconfig entry point |
| [`Makefile.verilator`](../Makefile.verilator) | Simulator build (bypasses FuseSoC) |
| [`doc/secure_ibex_hw_description.md`](secure_ibex_hw_description.md) | Full HW documentation |

---

## Phase 10 — Interrupt-driven UART

### Why interrupts matter

The polling UART driver from Phase 6 works for `printk()` output, but it wastes CPU cycles spinning on the status register when waiting for input. More importantly, Zephyr's shell subsystem and many other components (Bluetooth, networking, logging backends) require the **interrupt-driven UART API** — they register a callback that fires asynchronously when data arrives.

### Hardware interrupt behavior

From the RTL (`uart.sv`, line 153):
```verilog
assign uart_irq_o = !rx_fifo_empty;
```

The IRQ is **level-triggered**: it stays asserted as long as there is data in the 128-byte RX FIFO. There is **no TX interrupt** — the hardware only signals RX data availability.

The IRQ line connects to Ibex's fast interrupt input 0 (mcause 16), as wired in `ibex_reference_system_core.sv`:
```verilog
.irq_fast_i({14'b0, uart_irq})   // uart_irq → bit 0 → mcause 16
```

There is also **no interrupt enable/disable register** in the UART itself. The only way to mask the interrupt is at the CPU level (via the RISC-V `mie` CSR), which Zephyr's `irq_enable()` / `irq_disable()` handle.

### Driver changes

The driver was extended with the full `CONFIG_UART_INTERRUPT_DRIVEN` API, guarded by `#ifdef` so the polling-only build still works.

**New data structure** — stores the callback and per-instance flags:
```c
struct uart_ibex_data {
    uart_irq_callback_user_data_t cb;
    void *cb_data;
    bool tx_irq_enabled;
    bool rx_irq_enabled;
    bool in_isr;
};
```

**New functions added**:

| Function | Purpose |
|---|---|
| `fifo_fill()` | Write multiple bytes to TX FIFO (stops when full) |
| `fifo_read()` | Read multiple bytes from RX FIFO (stops when empty) |
| `irq_tx_enable()` | Set software flag + **kick-start** callback |
| `irq_tx_disable()` | Clear software flag |
| `irq_tx_ready()` | Returns 1 if TX enabled and FIFO not full |
| `irq_tx_complete()` | Returns 1 if TX FIFO is not full |
| `irq_rx_enable()` | Set software flag + `irq_enable(16)` |
| `irq_rx_disable()` | Clear software flag + `irq_disable(16)` if TX also off |
| `irq_rx_ready()` | Returns 1 if RX FIFO has data |
| `irq_is_pending()` | Returns rx_ready OR tx_ready |
| `irq_update()` | Returns 1 (no cached state to update) |
| `irq_callback_set()` | Stores the user callback and data pointer |

**The TX kick-start trick**:

Since there is no hardware TX interrupt, when `irq_tx_enable()` is called (e.g. by the shell when it has output to send), the driver directly invokes the user callback:

```c
static void uart_ibex_irq_tx_enable(const struct device *dev)
{
    struct uart_ibex_data *data = dev->data;
    data->tx_irq_enabled = true;

    /* No HW TX interrupt — call the callback so it can fill the FIFO */
    if (!data->in_isr && data->cb) {
        data->cb(dev, data->cb_data);
    }
}
```

The `in_isr` guard prevents infinite recursion: the shell callback calls `irq_tx_enable()` from within the ISR, so without this guard we'd have the callback calling itself endlessly.

**The ISR**:

```c
static void uart_ibex_isr(const struct device *dev)
{
    struct uart_ibex_data *data = dev->data;
    if (data->cb) {
        data->in_isr = true;
        data->cb(dev, data->cb_data);
        data->in_isr = false;
    }
}
```

The ISR fires whenever the RX FIFO has data (level-triggered). Inside the callback, the shell (or any user) calls `irq_rx_ready()` and `fifo_read()` to drain the FIFO, then `irq_tx_ready()` and `fifo_fill()` to send any pending output. Reading the RX FIFO clears the interrupt (since `uart_irq_o = !rx_fifo_empty`).

**IRQ registration via init**:

```c
static int uart_ibex_init(const struct device *dev)
{
    const struct uart_ibex_config *cfg = dev->config;
    cfg->irq_config_func(dev);    /* calls IRQ_CONNECT */
    return 0;
}
```

The `IRQ_CONNECT(DT_INST_IRQN(0), 0, uart_ibex_isr, ...)` macro registers our ISR for IRQ 16 (from the DTS `interrupts-extended = <&hlic 16>`). The actual interrupt is not enabled until `irq_rx_enable()` is called.

### Kconfig changes

```kconfig
config UART_IBEX
    ...
    select SERIAL_SUPPORT_INTERRUPT
```

`SERIAL_SUPPORT_INTERRUPT` must be selected **unconditionally** (not behind `if UART_INTERRUPT_DRIVEN`) because it declares that the driver *supports* interrupts. This allows `CONFIG_UART_INTERRUPT_DRIVEN=y` to be set by the board defconfig. Selecting it conditionally on `UART_INTERRUPT_DRIVEN` creates a circular dependency.

### Board defconfig changes

Added to `secure_ibex_defconfig`:
```
CONFIG_UART_INTERRUPT_DRIVEN=y
```

---

## Phase 11 — Zephyr shell

### What is the Zephyr shell

Zephyr includes an interactive command-line shell (`CONFIG_SHELL=y`) that works over UART. It provides built-in commands (`kernel threads`, `kernel uptime`, `device list`, etc.) and allows applications to register custom commands.

### Configuration

Added to `secure_ibex_defconfig`:
```
CONFIG_SHELL=y
CONFIG_SHELL_BACKEND_SERIAL=y
CONFIG_SHELL_BACKEND_SERIAL_API_POLLING=y
```

The shell backend automatically uses `zephyr,shell-uart` from the device tree `chosen` node (already set to `&uart0` in Phase 4).

### Why polling backend for the shell

The Zephyr shell backend has three API modes:
1. **Interrupt-driven** (`SHELL_BACKEND_SERIAL_API_INTERRUPT_DRIVEN`) — uses `uart_fifo_fill()` / `uart_fifo_read()` in ISR callbacks
2. **Polling** (`SHELL_BACKEND_SERIAL_API_POLLING`) — uses `poll_out()` / `poll_in()` with a kernel timer for RX polling
3. **Async** (`SHELL_BACKEND_SERIAL_API_ASYNC`) — uses DMA-based async API

The interrupt-driven mode fails on Secure-Ibex because of the missing TX interrupt. The TX path works as follows:

1. Shell puts output data in a ring buffer
2. Calls `uart_irq_tx_enable()`
3. Our driver's kick-start calls the callback once
4. Callback reads from ring buffer, fills the 128-byte TX FIFO
5. Callback returns — **and TX stalls**

The problem: after the initial kick-start, the callback is never called again because there is no HW TX IRQ. The ISR only fires on RX events (data received). If no characters arrive, the TX FIFO drains at baud rate but nobody refills it. Result: only the first ~128 bytes of output appear (the shell prompt `ESC[m` is just 3 bytes).

The **polling backend** avoids this entirely: `poll_out()` sends each byte synchronously (spinning on STATUS.TX_FULL), and a kernel timer periodically calls `poll_in()` for RX. This is slightly less efficient but works reliably with our hardware.

The interrupt-driven UART API is still available in the driver for applications that need asynchronous RX notifications (e.g., a UART-based protocol stack).

### Memory usage

| Sample | RAM usage | % of 128 KiB |
|---|---|---|
| hello_world (no shell) | ~16 KiB | 12% |
| hello_world (with shell) | ~57 KiB | 44% |
| shell_module | ~86 KiB | 66% |

The shell adds significant overhead (~30 KiB for the shell subsystem, command processing, history buffer, etc.), but still fits comfortably in 128 KiB.

### Build and test

```bash
# Shell sample
west build -b secure_ibex zephyr/samples/subsys/shell/shell_module -- -DDTC=/usr/bin/dtc

# hello_world (also gets shell via defconfig)
west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
```

### Simulation result

```
$ make -f Makefile.verilator sim
=== UART OUTPUT (uart0.log) ===
*** Booting Zephyr OS build v4.3.0-9462-g44ace049cd41 ***
Hello World! secure_ibex/secure_ibex
uart:~$
```

Both the hello_world message and the interactive shell prompt appear. The `uart:~$` prompt is ready to accept commands via the simulated PTY.

Note: the simulator logs `Illegal instruction (hart 0) at PC 0x00100080` at cycle 15. This is a **benign tracer artifact** during the Ibex pipeline reset — the instruction at that address (`0x00010413` = `addi s0, sp, 0`) is valid RV32I. Execution proceeds normally after this message.

---

## Summary of created files

| File | Purpose |
|---|---|
| [`soc/lowrisc/secure_ibex/soc.yml`](../soc/lowrisc/secure_ibex/soc.yml) | SoC declaration |
| [`soc/lowrisc/secure_ibex/Kconfig.soc`](../soc/lowrisc/secure_ibex/Kconfig.soc) | SoC boolean option |
| [`soc/lowrisc/secure_ibex/Kconfig`](../soc/lowrisc/secure_ibex/Kconfig) | Architectural features |
| [`soc/lowrisc/secure_ibex/Kconfig.defconfig`](../soc/lowrisc/secure_ibex/Kconfig.defconfig) | Defaults (50 MHz, 32 IRQs) |
| [`soc/lowrisc/secure_ibex/CMakeLists.txt`](../soc/lowrisc/secure_ibex/CMakeLists.txt) | Linker script |
| [`boards/lowrisc/secure_ibex/board.yml`](../boards/lowrisc/secure_ibex/board.yml) | Board metadata |
| [`boards/lowrisc/secure_ibex/secure_ibex.yaml`](../boards/lowrisc/secure_ibex/secure_ibex.yaml) | Board HW profile |
| [`boards/lowrisc/secure_ibex/Kconfig.secure_ibex`](../boards/lowrisc/secure_ibex/Kconfig.secure_ibex) | Links board to SoC |
| [`boards/lowrisc/secure_ibex/secure_ibex.dts`](../boards/lowrisc/secure_ibex/secure_ibex.dts) | Board device tree |
| [`boards/lowrisc/secure_ibex/secure_ibex_defconfig`](../boards/lowrisc/secure_ibex/secure_ibex_defconfig) | Default config (with shell + IRQ UART) |
| [`dts/riscv/lowrisc/secure_ibex.dtsi`](../dts/riscv/lowrisc/secure_ibex.dtsi) | SoC device tree |
| [`dts/bindings/serial/lowrisc,ibex-uart.yaml`](../dts/bindings/serial/lowrisc,ibex-uart.yaml) | UART DTS binding |
| [`dts/bindings/timer/lowrisc,ibex-timer.yaml`](../dts/bindings/timer/lowrisc,ibex-timer.yaml) | Timer DTS binding |
| [`drivers/serial/uart_ibex.c`](../drivers/serial/uart_ibex.c) | UART driver (polling + interrupt-driven) |
| [`drivers/serial/Kconfig.ibex_uart`](../drivers/serial/Kconfig.ibex_uart) | UART driver Kconfig |
| [`drivers/serial/CMakeLists.txt`](../drivers/serial/CMakeLists.txt) | UART driver build |
| [`drivers/timer/timer_ibex.c`](../drivers/timer/timer_ibex.c) | Timer driver (system clock) |
| [`drivers/timer/Kconfig.ibex_timer`](../drivers/timer/Kconfig.ibex_timer) | Timer driver Kconfig |
| [`drivers/timer/CMakeLists.txt`](../drivers/timer/CMakeLists.txt) | Timer driver build |
| [`zephyr/module.yml`](../zephyr/module.yml) | Out-of-tree module registration |
| [`CMakeLists.txt`](../CMakeLists.txt) | Out-of-tree build entry point |
| [`Kconfig`](../Kconfig) | Out-of-tree Kconfig entry point |
| [`Makefile.verilator`](../Makefile.verilator) | Simulator build (bypasses FuseSoC) |
| [`doc/secure_ibex_hw_description.md`](secure_ibex_hw_description.md) | Full HW documentation |

---

## Possible next steps

- **FPGA test on Arty A7** — synthesize and load on the real board
- **GPIO driver** — for LEDs and switches
- **PWM driver** — to control the RGB LEDs
- **SPI driver** — for external peripherals
- **Zephyr logging backend** — structured logging over UART
