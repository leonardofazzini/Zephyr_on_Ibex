#!/bin/bash
# extract_standalone.sh — Extract a self-contained build directory from a
# Zephyr west build. You can then modify src/main.c and recompile with
# just 'make' — no west, no cmake, no Zephyr build system.
#
# How it works:
#   - Copies all pre-built Zephyr libraries (.a) — kernel, arch, drivers, etc.
#   - Copies the linker script and generated headers
#   - Copies the application source (main.c)
#   - Generates a Makefile that recompiles ONLY your app code and links
#     against the pre-built libraries.
#
# Usage:
#   # 1. Normal Zephyr build first:
#   cd zephyr-workspace
#   west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
#
#   # 2. Extract:
#   ../scripts/extract_standalone.sh build ../standalone_hello
#
#   # 3. Edit your code:
#   nano standalone_hello/src/main.c
#
#   # 4. Rebuild (fast — only recompiles your code):
#   cd standalone_hello && make
#
#   # 5. Simulate:
#   cd .. && make -f Makefile.verilator sim ELF=standalone_hello/firmware.elf

set -euo pipefail

BUILD_DIR="${1:?Usage: $0 <build-dir> <output-dir>}"
OUT_DIR="${2:?Usage: $0 <build-dir> <output-dir>}"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "Error: $BUILD_DIR/compile_commands.json not found."
    echo "Run 'west build' first."
    exit 1
fi

echo "=== Extracting standalone build from $BUILD_DIR ==="

mkdir -p "$OUT_DIR/src" "$OUT_DIR/include" "$OUT_DIR/lib"

# --- 1. Extract compiler and Zephyr paths ---
COMPILER=$(python3 -c "
import json
with open('$BUILD_DIR/compile_commands.json') as f:
    print(json.load(f)[0]['command'].split()[0])
")
TOOLCHAIN_BIN=$(dirname "$COMPILER")

# Find ZEPHYR_BASE from include paths
ZEPHYR_BASE=$(python3 -c "
import json
with open('$BUILD_DIR/compile_commands.json') as f:
    cmds = json.load(f)
for c in cmds:
    for part in c['command'].split():
        if '/zephyr/include' in part and part.startswith('-I'):
            path = part[2:]
            # strip /include from end
            base = path.rsplit('/include', 1)[0]
            if '/zephyr' in base:
                print(base)
                exit()
")

echo "Compiler:    $COMPILER"
echo "ZEPHYR_BASE: $ZEPHYR_BASE"

# --- 2. Copy application source ---
echo "Copying application source..."
python3 -c "
import json, shutil, os

with open('$BUILD_DIR/compile_commands.json') as f:
    cmds = json.load(f)

for c in cmds:
    src = c['file']
    if '/samples/' in src or ('/app/' in c.get('directory','') and '/zephyr/' not in src):
        dest = '$OUT_DIR/src/' + os.path.basename(src)
        shutil.copy2(src, dest)
        print(f'  {os.path.basename(src)}')
"

# --- 3. Copy generated headers ---
echo "Copying generated headers..."
cp -r "$BUILD_DIR/zephyr/include/generated" "$OUT_DIR/include/generated"

# --- 4. Copy linker script ---
echo "Copying linker script..."
cp "$BUILD_DIR/zephyr/linker.cmd" "$OUT_DIR/linker.cmd"

# --- 5. Copy ALL static libraries ---
echo "Copying pre-built libraries..."
find "$BUILD_DIR" -name "*.a" | while read -r lib; do
    name=$(basename "$lib")
    cp "$lib" "$OUT_DIR/lib/$name"
done
LIB_COUNT=$(find "$OUT_DIR/lib" -name "*.a" 2>/dev/null | wc -l)
echo "  $LIB_COUNT libraries"

# --- 6. Extract compile flags for the app ---
echo "Extracting compiler flags..."
python3 << PYEOF
import json, os

with open('$BUILD_DIR/compile_commands.json') as f:
    cmds = json.load(f)

# Find the compile command for main.c (the app)
app_cmd = None
for c in cmds:
    if '/samples/' in c['file'] or ('main.c' in c['file'] and '/zephyr/' not in c['file']):
        app_cmd = c
        break

if not app_cmd:
    app_cmd = cmds[0]

parts = app_cmd['command'].split()

# Extract flags, translating generated include paths to local
cflags = []
includes = []
i = 1
while i < len(parts):
    p = parts[i]
    if p == '-c' or p == '-o' or p == '-MD' or p == '-MT' or p == '-MF':
        i += 2; continue
    if p.startswith('-MF') or p.startswith('-MT') or p.startswith('-MD'):
        i += 1; continue
    if p.endswith('.c') or p.endswith('.o'):
        i += 1; continue
    if p.startswith('-fmacro-prefix-map'):
        i += 1; continue

    # Remap -imacros: generated headers → local, others → keep original path
    if p == '-imacros':
        imacros_path = parts[i+1]
        basename_f = os.path.basename(imacros_path)
        if 'generated' in imacros_path:
            cflags.append(f'-imacros \$(DIR)/include/generated/zephyr/{basename_f}')
        else:
            cflags.append(f'-imacros {imacros_path}')
        i += 2; continue

    if p.startswith('-I'):
        path = p[2:] if len(p) > 2 else parts[i+1]
        if 'generated' in path:
            includes.append('-I\$(DIR)/include/generated')
            includes.append('-I\$(DIR)/include/generated/zephyr')
        else:
            includes.append(f'-I{path}')
        if len(p) == 2: i += 1
        i += 1; continue

    if p == '-isystem':
        includes.append(f'-isystem {parts[i+1]}')
        i += 2; continue

    cflags.append(p)
    i += 1

# Deduplicate includes
seen = set()
unique_includes = []
for inc in includes:
    if inc not in seen:
        seen.add(inc)
        unique_includes.append(inc)

with open('$OUT_DIR/build_flags.mk', 'w') as f:
    f.write('# Auto-extracted from Zephyr build — compiler flags for app code\n\n')
    f.write(f'CC      := $COMPILER\n')
    f.write(f'OBJCOPY := ${TOOLCHAIN_BIN}/riscv64-zephyr-elf-objcopy\n')
    f.write(f'SIZE    := ${TOOLCHAIN_BIN}/riscv64-zephyr-elf-size\n\n')
    f.write('CFLAGS := \\\\\n')
    for fl in cflags:
        f.write(f'  {fl} \\\\\n')
    f.write('\n\n')
    f.write('INCLUDES := \\\\\n')
    for inc in unique_includes:
        f.write(f'  {inc} \\\\\n')
    f.write('\n')

print(f'  {len(cflags)} flags, {len(unique_includes)} include paths')
PYEOF

# --- 7. Remove libapp.a (we replace it with our own main.o) ---
rm -f "$OUT_DIR/lib/libapp.a"

# --- 8. Generate standalone Makefile ---
echo "Generating Makefile..."
cat > "$OUT_DIR/Makefile" << 'EOF'
# Standalone Makefile for Zephyr firmware
# Recompiles ONLY your application code, links against pre-built Zephyr libs.
#
#   make              — build firmware.elf
#   make clean        — remove build artifacts
#   make info         — show build configuration
#
# Edit src/main.c (or add more .c files to src/) and run 'make'.

DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))

include $(DIR)/build_flags.mk

# Your application sources (add more .c files to src/ as needed)
APP_SRCS := $(wildcard $(DIR)/src/*.c)
APP_OBJS := $(APP_SRCS:$(DIR)/src/%.c=$(DIR)/obj/%.o)

# Pre-built Zephyr libraries.
# - WHOLE_LIBS: linked with --whole-archive (all symbols pulled in)
# - NO_WHOLE_LIBS: linked normally (only referenced symbols pulled in)
WHOLE_LIBS := $(filter-out %libkernel.a %libisr_tables.a, $(wildcard $(DIR)/lib/*.a))
NO_WHOLE_LIBS := $(filter %libkernel.a %libisr_tables.a, $(wildcard $(DIR)/lib/*.a))

LDFLAGS := \
  -T $(DIR)/linker.cmd \
  -nostdlib -static \
  -fuse-ld=bfd \
  -mabi=ilp32 -march=rv32imc_zicsr_zifencei -mcmodel=medlow \
  -Wl,--gc-sections \
  -Wl,--build-id=none \
  -Wl,--sort-common=descending \
  -Wl,--sort-section=alignment \
  -Wl,-no-pie \
  -Wl,-X -Wl,-N \
  -Wl,--orphan-handling=warn \
  -Wl,-u,_OffsetAbsSyms \
  -Wl,-u,_ConfigAbsSyms \
  -Wl,--undefined=_sw_isr_table \
  -Wl,--undefined=_irq_vector_table \
  -Wl,--print-memory-usage \
  -specs=picolibc.specs \
  -DPICOLIBC_LONG_LONG_PRINTF_SCANF

TARGET := $(DIR)/firmware.elf

.PHONY: all clean info

all: $(TARGET)

$(TARGET): $(APP_OBJS)
	$(CC) $(APP_OBJS) $(LDFLAGS) \
	  -Wl,--whole-archive $(WHOLE_LIBS) \
	  -Wl,--no-whole-archive $(NO_WHOLE_LIBS) \
	  -Wl,--start-group -lc -lgcc -Wl,--end-group \
	  -o $@
	$(SIZE) $@

$(DIR)/obj/%.o: $(DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(DIR)/obj $(TARGET)

info:
	@echo "Compiler:     $(CC)"
	@echo "App sources:  $(APP_SRCS)"
	@echo "Whole libs:   $(WHOLE_LIBS)"
	@echo "Normal libs:  $(NO_WHOLE_LIBS)"
	@echo "Target:       $(TARGET)"
EOF

# --- Summary ---
echo ""
echo "=== Extraction complete ==="
echo ""
echo "  $OUT_DIR/"
echo "  ├── src/main.c          ← YOUR CODE (edit this)"
echo "  ├── include/generated/  ← autoconf.h, devicetree_generated.h"
echo "  ├── lib/*.a             ← pre-built Zephyr ($LIB_COUNT libraries)"
echo "  ├── linker.cmd          ← linker script"
echo "  ├── build_flags.mk      ← compiler flags"
echo "  └── Makefile            ← run 'make' to build"
echo ""
echo "Workflow:"
echo "  1. Edit $OUT_DIR/src/main.c"
echo "  2. cd $OUT_DIR && make"
echo "  3. make -f Makefile.verilator sim ELF=$OUT_DIR/firmware.elf"
