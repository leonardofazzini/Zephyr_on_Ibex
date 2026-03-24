# Zephyr on Ibex

Porting Zephyr RTOS to the RISC-V Ibex core (Secure-Ibex variant), targeting
the Digilent Arty A7 FPGA board.

## Project structure

```
Zephyr_on_Ibex/
├── hw/Secure-Ibex/              # Submodule: Ibex SoC RTL
├── zephyr-workspace/            # Zephyr RTOS (managed by west, in .gitignore)
│   ├── zephyr/                  # Zephyr kernel and sources
│   └── modules/                 # Zephyr modules (HAL, libraries, etc.)
├── boards/lowrisc/secure_ibex/  # Board definition (out-of-tree)
├── soc/lowrisc/secure_ibex/     # SoC definition (out-of-tree)
├── dts/                         # Device tree sources and bindings
│   ├── riscv/lowrisc/           #   SoC-level .dtsi
│   └── bindings/serial/         #   DTS bindings for custom UART
├── drivers/serial/              # Custom UART driver for Ibex
├── doc/                         # Documentation
│   ├── secure_ibex_hw_description.md  # Full HW architecture
│   └── porting_log.md                 # Detailed porting log
├── zephyr/module.yml            # Out-of-tree module registration
├── CMakeLists.txt               # Out-of-tree driver build
├── Kconfig                      # Out-of-tree driver Kconfig
├── Makefile.verilator           # Verilator simulation build
├── .venv/                       # Python virtual environment
└── README.md
```

## Quick start

To build and test from a clean Ubuntu 24.04 system:

```bash
# 1. System dependencies
sudo apt install -y device-tree-compiler gperf python3-venv cmake ninja-build \
                    verilator libelf-dev srecord

# 2. Clone
git clone --recurse-submodules git@github.com:leonardofazzini/Zephyr_on_Ibex.git
cd Zephyr_on_Ibex

# 3. Python environment + west
python3 -m venv .venv
source .venv/bin/activate
pip install west

# 4. Zephyr workspace (~2 GB download)
west init -m https://github.com/zephyrproject-rtos/zephyr --mr main zephyr-workspace
cd zephyr-workspace && west update && pip install -r zephyr/scripts/requirements.txt && cd ..

# 5. Zephyr SDK
cd /tmp
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.0/zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-1.0.0 && ./setup.sh && mv /tmp/zephyr-sdk-1.0.0 ~/.local/zephyr-sdk-1.0.0
cd -

# 6. Build Zephyr for Secure-Ibex
cd Zephyr_on_Ibex/zephyr-workspace
export ZEPHYR_SDK_INSTALL_DIR=~/.local/zephyr-sdk-1.0.0
export ZEPHYR_BASE=$(pwd)/zephyr
export ZEPHYR_EXTRA_MODULES=$(realpath ..)
west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc

# 7. Build Verilator simulator
cd ..
make -f Makefile.verilator build

# 8. Run the simulation
make -f Makefile.verilator sim
```

Expected output in `uart0.log`:
```
*** Booting Zephyr OS build v4.3.0-... ***
Hello World! secure_ibex/secure_ibex
```

## Step-by-step installation

If the quick start is not enough or something goes wrong, see below.

### Prerequisites

- **OS**: Ubuntu 24.04 (or Debian derivative)
- **Python**: >= 3.12
- **CMake**: >= 3.28
- **Ninja**: >= 1.12
- **Verilator**: >= 5.0 (for simulation)

### 1. System packages

```bash
sudo apt install -y device-tree-compiler gperf python3-venv cmake ninja-build \
                    verilator libelf-dev srecord
```

### 2. Clone the repository

```bash
git clone --recurse-submodules git@github.com:leonardofazzini/Zephyr_on_Ibex.git
cd Zephyr_on_Ibex
```

If the repo was already cloned without submodules:
```bash
git submodule update --init --recursive
```

### 3. Python virtual environment and west

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install west
```

### 4. Initialize the Zephyr workspace

```bash
west init -m https://github.com/zephyrproject-rtos/zephyr --mr main zephyr-workspace
cd zephyr-workspace
west update
pip install -r zephyr/scripts/requirements.txt
cd ..
```

> `west update` downloads all Zephyr modules (~2 GB). This takes a while.

### 5. Zephyr SDK (RISC-V toolchain)

```bash
cd /tmp
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.0/zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-1.0.0_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-1.0.0
./setup.sh
mv /tmp/zephyr-sdk-1.0.0 ~/.local/zephyr-sdk-1.0.0
```

The minimal SDK already includes the RISC-V toolchain (`riscv64-zephyr-elf`).

### 6. Environment variables

Set these at the beginning of every work session:

```bash
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=~/.local/zephyr-sdk-1.0.0
export ZEPHYR_BASE=$(pwd)/zephyr-workspace/zephyr
export ZEPHYR_EXTRA_MODULES=$(pwd)
```

> Consider adding these lines to an `env.sh` script or your `~/.bashrc`.

## Building Zephyr firmware

### Build hello_world

```bash
cd zephyr-workspace
west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
```

The output ELF is at `zephyr-workspace/build/zephyr/zephyr.elf`.

### Build a different sample

```bash
cd zephyr-workspace
rm -rf build
west build -b secure_ibex zephyr/samples/basic/blinky -- -DDTC=/usr/bin/dtc
```

### Build details

| Parameter | Value |
|---|---|
| Board name | `secure_ibex` |
| Entry point | `0x00100000` (SRAM start) |
| Architecture | RV32IMC, ELF32, soft-float ABI |
| Total RAM | 128 KiB |
| RAM used (hello_world) | ~16 KiB (12%) |

> The `-DDTC=/usr/bin/dtc` flag uses the system device-tree-compiler instead
> of the one bundled in the SDK, which may have dynamic library issues.

## Verilator simulation

### Build the simulator (once)

```bash
make -f Makefile.verilator build
```

The first build takes a few minutes (Verilator + C++ compilation).
Subsequent builds are incremental.

### Run the simulation

```bash
make -f Makefile.verilator sim
```

Or with a custom ELF:
```bash
make -f Makefile.verilator sim ELF=path/to/firmware.elf
```

UART output is written to `uart0.log` and to a virtual pseudo-terminal
(`/dev/pts/N`, printed on stdout at startup).

To connect an interactive terminal to the simulated UART:
```bash
screen /dev/pts/N    # N is printed at simulator startup
```

To stop the simulation: `Ctrl+C`.

### Useful simulator options

| Option | Description |
|---|---|
| `--meminit=ram,FILE.elf` | Load an ELF into SRAM |
| `--term-after-cycles=N` | Stop after N clock cycles |
| `-t` | Enable tracing (generates `sim.fst` for GTKWave) |

## Tested samples

All samples below have been built and verified on the Verilator simulator.

The general workflow is always the same:

```bash
# 1. Set up environment (once per terminal session)
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=~/.local/zephyr-sdk-1.0.0
export ZEPHYR_BASE=$(pwd)/zephyr-workspace/zephyr
export ZEPHYR_EXTRA_MODULES=$(pwd)

# 2. Build
cd zephyr-workspace
rm -rf build
west build -b secure_ibex zephyr/samples/<SAMPLE_PATH> -- -DDTC=/usr/bin/dtc

# 3. Simulate (from project root)
cd ..
make -f Makefile.verilator sim

# 4. Check output
cat uart0.log
```

### hello_world

```bash
west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
```

Expected output (`uart0.log`):
```
*** Booting Zephyr OS build v4.3.0-... ***
Hello World! secure_ibex/secure_ibex
uart:~$
```

- RAM: ~57 KiB (44%) — includes the Zephyr shell, enabled by default
- Proves: boot sequence, UART TX, console subsystem, shell init
- Simulation: 50M cycles (~1 sec simulated) is enough

### philosophers (Dining Philosophers)

```bash
west build -b secure_ibex zephyr/samples/philosophers -- \
    -DDTC=/usr/bin/dtc -DCONFIG_SHELL=n -DCONFIG_SHELL_BACKEND_SERIAL=n
```

Expected output (`uart0.log`):
```
*** Booting Zephyr OS build v4.3.0-... ***
Demo Description
----------------
An implementation of a solution to the Dining Philosophers problem ...

Philosopher 4 [C:-1]   EATING  [  25 ms ]
Philosopher 5 [C:-2]   EATING  [  25 ms ]
Philosopher 3 [P: 0]   EATING  [  25 ms ]
...
```

- RAM: ~33 KiB (25%)
- Proves: **timer interrupts work** (k_sleep), preemptive multithreading,
  mutex synchronization, thread priorities (cooperative C:-1/-2 and
  preemptible P:0-3)
- Simulation: 200M cycles (~4 sec simulated) to see multiple rounds.
  Philosophers cycle through STARVING -> HOLDING ONE FORK -> EATING ->
  THINKING states with randomized delays.
- Shell is disabled here (`-DCONFIG_SHELL=n`) to keep output clean and
  reduce RAM usage.

### shell_module (Zephyr interactive shell)

```bash
west build -b secure_ibex zephyr/samples/subsys/shell/shell_module -- -DDTC=/usr/bin/dtc
```

Expected output (`uart0.log`):
```
uart:~$
```

- RAM: ~86 KiB (66%)
- Proves: interrupt-driven UART RX, shell subsystem, command processing
- Simulation: 50M cycles for the prompt to appear. To interact with the
  shell, connect to the PTY printed at startup:
  ```bash
  screen /dev/pts/N
  ```
  Then type `help`, `kernel uptime`, `device list`, etc.
- The shell uses polling backend for TX (no HW TX interrupt), and a
  kernel timer for RX polling.

### synchronization

```bash
west build -b secure_ibex zephyr/samples/synchronization -- \
    -DDTC=/usr/bin/dtc -DCONFIG_SHELL=n -DCONFIG_SHELL_BACKEND_SERIAL=n
```

Expected output (`uart0.log`):
```
*** Booting Zephyr OS build v4.3.0-... ***
thread_a: Hello World from cpu 0 on secure_ibex!
thread_b: Hello World from cpu 0 on secure_ibex!
thread_a: Hello World from cpu 0 on secure_ibex!
thread_b: Hello World from cpu 0 on secure_ibex!
...
```

- RAM: ~20 KiB (15%)
- Proves: semaphore synchronization, k_sleep, two threads alternating
- Simulation: 200M cycles to see several rounds of alternation.

### Samples that do NOT work

Samples requiring hardware not present on this board will fail to build:

| Sample | Reason |
|---|---|
| `basic/blinky` | Requires `led0` alias (GPIO driver) |
| `basic/threads` | Requires `led0`, `led1` aliases |
| `bluetooth/*` | No Bluetooth hardware |
| `net/*` | No network hardware |
| `drivers/spi/*` | No SPI driver yet |
| `sensor/*` | No sensor hardware |

### Disabling the shell for lighter builds

The shell is enabled by default in the board defconfig. To build any
sample without it (saves ~40 KiB RAM):

```bash
west build -b secure_ibex zephyr/samples/<path> -- \
    -DDTC=/usr/bin/dtc -DCONFIG_SHELL=n -DCONFIG_SHELL_BACKEND_SERIAL=n
```

## Target hardware

The Secure-Ibex SoC is a minimal RISC-V system built around the lowRISC Ibex core:

| Peripheral | Address | Description |
|---|---|---|
| SRAM | `0x00100000` | 128 KiB, code + data + stack |
| Timer | `0x80000000` | mtime/mtimecmp, 64-bit, IRQ mcause 7 |
| UART | `0x80001000` | 3 registers (RX/TX/STATUS), 115200 baud, IRQ mcause 16 |
| GPIO | `0x80002000` | 8-in / 16-out |
| PWM | `0x80003000` | 12 channels, 8-bit |

- **CPU**: lowRISC Ibex, RV32IMC @ 50 MHz, single-hart
- **FPGA board**: Digilent Arty A7-35T

For the full hardware documentation, see
[doc/secure_ibex_hw_description.md](doc/secure_ibex_hw_description.md).

For the detailed porting log, see
[doc/porting_log.md](doc/porting_log.md).
