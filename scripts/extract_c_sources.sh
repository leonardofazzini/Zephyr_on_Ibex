#!/bin/bash
# extract_c_sources.sh — Collect every .c/.S file actually compiled in a Zephyr
# build into a destination folder, plus the headers needed to recompile them
# from that folder. Directory structure is preserved.
#
# Reads compile_commands.json (the authoritative list of compiled files) and
# copies each source under a classified prefix:
#
#   <dest>/zephyr/...      files from $ZEPHYR_BASE (kernel, drivers, lib, arch)
#   <dest>/module/...      files from this project (custom drivers, app)
#   <dest>/generated/...   files generated inside <build>/ (DTS, syscalls, ISR tables)
#                          including <build>/zephyr/include/generated/* headers
#   <dest>/external/...    everything else (HAL modules, picolibc, etc.)
#
# The script copies, in addition to the .c/.S files:
#   - every .h file colocated in the same directory as a copied source
#     (needed for `#include "foo.h"` which gcc resolves relative to the .c)
#   - the entire <build>/zephyr/include/generated/ tree (autoconf.h,
#     devicetree_generated.h, syscall_list.h, version.h, …)
#
# Triggers ninja first to make sure post-link generated files exist
# (most notably isr_tables.c, which only appears after the pre-link).
#
# Usage:
#   scripts/extract_c_sources.sh <build-dir> <output-dir> [--include-asm]
#
# Example:
#   cd zephyr-workspace
#   west build -b secure_ibex zephyr/samples/hello_world -- -DDTC=/usr/bin/dtc
#   cd ..
#   scripts/extract_c_sources.sh zephyr-workspace/build sources_dump

set -euo pipefail

BUILD_DIR="${1:?Usage: $0 <build-dir> <output-dir> [--include-asm]}"
OUT_DIR="${2:?Usage: $0 <build-dir> <output-dir> [--include-asm]}"
INCLUDE_ASM=0
if [ "${3:-}" = "--include-asm" ]; then
    INCLUDE_ASM=1
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "Error: $BUILD_DIR/compile_commands.json not found." >&2
    echo "Run 'west build' first." >&2
    exit 1
fi

# Trigger code generation. Targeting isr_tables.c pulls in every earlier
# generation step (devicetree, syscalls, version, offsets, pre0.elf).
echo "=== Ensuring generated files exist ==="
ninja -C "$BUILD_DIR" zephyr/isr_tables.c >/dev/null

# Try to locate ZEPHYR_BASE from compile_commands.json so we can classify files.
# Pick the include path that ends with exactly '/zephyr/include' and whose
# parent looks like a real Zephyr tree (has kernel/, arch/, etc.).
ZEPHYR_BASE=$(python3 -c "
import json, os
with open('$BUILD_DIR/compile_commands.json') as f:
    cmds = json.load(f)
candidates = set()
for c in cmds:
    for part in c['command'].split():
        if part.startswith('-I') and part.endswith('/zephyr/include'):
            candidates.add(part[2:-len('/include')])
for base in candidates:
    if os.path.isdir(os.path.join(base, 'kernel')) and \
       os.path.isdir(os.path.join(base, 'arch')):
        print(base)
        break
")

if [ -z "$ZEPHYR_BASE" ]; then
    echo "Error: could not locate ZEPHYR_BASE from compile_commands.json" >&2
    exit 1
fi

PROJ_ROOT=$(realpath "$(dirname "$0")/..")

echo "Build dir:    $BUILD_DIR"
echo "ZEPHYR_BASE:  $ZEPHYR_BASE"
echo "Project root: $PROJ_ROOT"
echo "Output:       $OUT_DIR"
echo

mkdir -p "$OUT_DIR"

INCLUDE_ASM=$INCLUDE_ASM python3 - "$BUILD_DIR" "$OUT_DIR" "$ZEPHYR_BASE" "$PROJ_ROOT" <<'PYEOF'
import json, os, shutil, sys
from pathlib import Path
from collections import Counter

build_dir   = Path(sys.argv[1]).resolve()
out_dir     = Path(sys.argv[2]).resolve()
zephyr_base = Path(sys.argv[3]).resolve()
proj_root   = Path(sys.argv[4]).resolve()
include_asm = os.environ.get("INCLUDE_ASM") == "1"

with (build_dir / "compile_commands.json").open() as f:
    entries = json.load(f)

valid_src_exts = {".c"} | ({".S", ".s"} if include_asm else set())
buckets = Counter()
seen = set()
src_dirs = {}   # original-dir → (kind, dest-dir) — used to copy colocated headers


def classify(src: Path):
    """Return (kind, dest_rel_path)."""
    if build_dir in src.parents or src == build_dir:
        return "generated", Path("generated") / src.relative_to(build_dir)
    if zephyr_base in src.parents:
        return "zephyr", Path("zephyr") / src.relative_to(zephyr_base)
    if proj_root in src.parents:
        return "module", Path("module") / src.relative_to(proj_root)
    return "external", Path("external") / Path(*src.parts[1:])


# --- 1. Copy every compiled .c/.S source -------------------------------------
for entry in entries:
    src = Path(entry["file"]).resolve()
    if src.suffix not in valid_src_exts or not src.exists() or src in seen:
        continue
    seen.add(src)
    kind, rel = classify(src)
    target = out_dir / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, target)
    buckets[kind] += 1
    src_dirs[src.parent] = (kind, target.parent)

# --- 2. Copy headers colocated with each compiled source ---------------------
# gcc resolves `#include "foo.h"` relative to the source file's directory,
# so for each directory that holds a copied .c/.S, copy its .h/.hpp/.inc too.
header_exts = {".h", ".hpp", ".inc"}
header_count = 0
for orig_dir, (kind, dest_dir) in src_dirs.items():
    for f in orig_dir.iterdir():
        if f.is_file() and f.suffix in header_exts:
            shutil.copy2(f, dest_dir / f.name)
            header_count += 1

# --- 3. Copy the generated headers tree --------------------------------------
# <build>/zephyr/include/generated/ holds autoconf.h, devicetree_generated.h,
# version.h, syscall_list.h, syscalls/*.h, etc. Without this the extracted
# sources can't compile from scratch.
gen_include_src = build_dir / "zephyr" / "include" / "generated"
gen_include_count = 0
if gen_include_src.is_dir():
    gen_include_dest = out_dir / "generated" / "include" / "generated"
    if gen_include_dest.exists():
        shutil.rmtree(gen_include_dest)
    shutil.copytree(gen_include_src, gen_include_dest)
    gen_include_count = sum(
        1 for _ in gen_include_dest.rglob("*") if _.is_file()
    )

print(f"=== Done ===")
print(f"  sources ({'/'.join(sorted(valid_src_exts))})")
for k in ("zephyr", "module", "generated", "external"):
    if buckets[k]:
        print(f"    {k:<10} {buckets[k]:>5} files")
print(f"  colocated headers: {header_count} files")
print(f"  generated headers: {gen_include_count} files")
PYEOF

echo
echo "Layout:"
find "$OUT_DIR" -maxdepth 2 -type d | sort | sed "s|$OUT_DIR|  .|"
