#!/usr/bin/env bash
# gen_dasmf8_golden.sh -- regenerate core/tests/dasmf8_golden.h from MAME's own
# F8 disassembler (src/devices/cpu/f8/f8dasm.cpp), compiled standalone with a
# short emu.h shim. Needs a MAME source tree and g++.
#
#   tools/ab/gen_dasmf8_golden.sh          MAME_SRC defaults to ~/Workspace/mame
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MAME_SRC="${MAME_SRC:-$HOME/Workspace/mame}/src"
CXX="${CXX:-g++}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/shim"
cat > "$WORK/shim/emu.h" <<'SHIM'
// stand-in for MAME's emu.h: f8dasm.cpp needs only these
#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include "coretmpl.h"
#include "disasmintf.h"
#include "strformat.h"
using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
using offs_t = u32;
SHIM

"$CXX" -std=c++20 -O1 -I "$WORK/shim" -I "$MAME_SRC/lib/util" -I "$MAME_SRC/osd" \
    -I "$MAME_SRC/devices/cpu/f8" \
    "$HERE/f8_dasm_golden.cpp" \
    "$MAME_SRC/devices/cpu/f8/f8dasm.cpp" \
    "$MAME_SRC/lib/util/disasmintf.cpp" "$MAME_SRC/lib/util/strformat.cpp" \
    -o "$WORK/gen"
"$WORK/gen" > "$ROOT/core/tests/dasmf8_golden.h"
echo "$ROOT/core/tests/dasmf8_golden.h: $(wc -l < "$ROOT/core/tests/dasmf8_golden.h") lines"
