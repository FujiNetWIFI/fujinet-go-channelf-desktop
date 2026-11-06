-- f8_trace.lua -- MAME side of the F8 conformance test: logs the F8's state
-- before every instruction of a probe cartridge on the channelf driver, then
-- exits when the probe reaches its end marker.
--
-- Run as an -autoboot_script of `mame channelf -bios sl31253 -cart probe.bin
-- -debug -debugger none ...` from MAME's own directory (tools/ab/f8_ab.py does).
-- The "none" debugger front end never stops the CPU, so the trace is set up
-- here with the debugger's `trace` command and its `tracelog` action, which
-- MAME runs before each instruction.
--
-- Environment:
--   F8_TRACE   the trace file to write
--   F8_DONE    the end marker address, with a 0x prefix (a bare A00 would be the
--              accumulator): a breakpoint there ends the run, and a frame limit
--              backs it up
--   F8_FRAMES  the frame limit (default 600)
--   F8_TAP     when set, also log every I/O port access and every access to the
--              cart RAM ($2800-$2FFF) there, with the CPU's clock count at the
--              access (tools/ab/f8_ab.py --timing): "IR|IW|MR|MW addr data cycles"
--
-- One row per instruction, the state BEFORE it executes, all hex but the last:
--   pc a w is pc1 dc0 dc1 r0..r15 s1 s2 cycles
-- r0-r15 are the scratchpad bytes the debugger names (R0-R8, J, HU HL, KU KL, QU
-- QL), packed as 32 digits; s1 and s2 are two weighted sums over the other 48
-- bytes, r16..r63 (read through the data space, db@):
--   s1 = sum (i + 1) * r[16 + i],  s2 = sum (i * i + 3) * r[16 + i]
-- cycles is the CPU's clocks since power-on. (`pc` is not a symbol of this CPU
-- in the debugger; curpc is the address of the instruction. Numbers in the
-- expressions carry the 0x prefix: a bare A or AB is the accumulator.)
--
-- SPDX-License-Identifier: GPL-3.0-or-later

local trace_file = os.getenv("F8_TRACE") or "f8.trace"
local done = os.getenv("F8_DONE") or "0x1EFC"
local limit = tonumber(os.getenv("F8_FRAMES") or "600")

local s1, s2 = {}, {}
for i = 0, 47 do
  s1[#s1 + 1] = string.format("0x%X*(db@0x%X)", i + 1, 16 + i)
  s2[#s2 + 1] = string.format("0x%X*(db@0x%X)", i * i + 3, 16 + i)
end

local dbg = manager.machine.debugger
dbg:command('trace ' .. trace_file ..
            ',maincpu,noloop,{tracelog "%04X %02X %02X %02X %04X %04X %04X ' ..
            '%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X ' ..
            '%08X %08X %d\\n",curpc,a,w,is,pc1,dc0,dc1,' ..
            'r0,r1,r2,r3,r4,r5,r6,r7,r8,j,hu,hl,ku,kl,qu,ql,' ..
            table.concat(s1, "+") .. ',' .. table.concat(s2, "+") .. ',totalcycles}')
-- the breakpoint's action quits MAME; the trace flushes after every line
dbg:command('bpset ' .. done .. ',1,{quit}')

local tap_file = os.getenv("F8_TAP")
if tap_file then
  -- MAME's scheduler time inside a handler is the running CPU's local time, which
  -- is the start of the bus cycle making the access (the state's clocks are spent
  -- after it); the CPU clock is Colorburst / 2 (channelf.cpp; the Lua API has no clock())
  local out = io.open(tap_file, "w")
  local cpu = manager.machine.devices[":maincpu"]
  local clk = 3579545 / 2
  local function log(kind, addr, data)
    out:write(string.format("%s %04X %02X %.3f\n", kind, addr, data & 0xff, manager.machine.time:as_double() * clk))
    out:flush()
  end
  local io_space = cpu.spaces["io"]
  local prog = cpu.spaces["program"]
  _G.F8_TAPS = {
    io_space:install_write_tap(0, 0xff, "f8_iow", function(off, data, mask) log("IW", off, data) end),
    io_space:install_read_tap(0, 0xff, "f8_ior", function(off, data, mask) log("IR", off, data) end),
    prog:install_write_tap(0x2800, 0x2fff, "f8_mw", function(off, data, mask) log("MW", off, data) end),
    prog:install_read_tap(0x2800, 0x2fff, "f8_mr", function(off, data, mask) log("MR", off, data) end),
  }
end

local frames = 0
_G.F8_FRAME = emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames >= limit then
    manager.machine:exit()
  end
end)
