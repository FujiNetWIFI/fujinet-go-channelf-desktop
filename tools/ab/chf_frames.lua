-- chf_frames.lua -- dump every frame (and optionally every UVI access) of
-- MAME's `channelf` driver, for the A/B against core/chf (tools/ab/cart_ab.py,
-- config_golden.py).
--
-- Environment:
--   CHF_FRAMES    binary file: every frame's 192x262 pixels, 32-bit LE ARGB
--   CHF_FRAMES_N  stop after this many frames (default 60)
--   CHF_LOG       text file: `W <cycle> <addr> <data> <line>` (line = the UVI's m_line) for every write to
--                 $1800-$1AFF and `F <n> <cycle>` per frame, the cycle being
--                 the 2650's local count (895000 Hz), like chf_headless -buslog
--   CHF_PRESS     "frame:port:mask:value,..." from that frame, set the fields
--                 of ioport `port` that `mask` selects to `value` (0 or 1),
--                 e.g. 120:controller1_col2:8:1,126:controller1_col2:8:0
--
-- MAME runs this chunk on a coroutine that the collector frees once the chunk
-- returns; the taps and the notifier keep using its lua_State, so one GC cycle
-- crashes MAME. Never collect.
collectgarbage("stop")

local framesf = os.getenv("CHF_FRAMES") and assert(io.open(os.getenv("CHF_FRAMES"), "wb")) or nil
local logf = os.getenv("CHF_LOG") and assert(io.open(os.getenv("CHF_LOG"), "w")) or nil
local nmax = tonumber(os.getenv("CHF_FRAMES_N") or "60")
local cpu = manager.machine.devices[":maincpu"]
local scr = manager.machine.screens[":screen"]
local prog = cpu.spaces["program"]

local APC = 1000000000000000000 // 895000   -- attoseconds per CPU cycle (truncated)
local function now_cycles()
  local tm = manager.machine.time
  return (tm.seconds * 1000000000000000000 + tm.attoseconds) // APC
end

-- the UVI's own line counter (m_line), read through the device's saved items
local lineitem = nil
local ok, it = pcall(function() return emu.item(manager.machine.devices[":"].items["0/m_line"]) end)
if ok then lineitem = it end

local tap
if logf then
  tap = prog:install_write_tap(0x1800, 0x1aff, "uvi_w", function(off, data, mask)
    logf:write("W ", now_cycles(), " ", off, " ", data, " ", lineitem and lineitem:read(0) or -1, "\n")
  end)
end

local presses = {}
for spec in string.gmatch(os.getenv("CHF_PRESS") or "", "[^,]+") do
  local f, p, m, v = string.match(spec, "(%d+):([^:]+):(%d+):(%d+)")
  if f then presses[#presses + 1] = { f = tonumber(f), port = p, mask = tonumber(m), value = tonumber(v) } end
end
local function setport(portname, mask, value)
  if string.sub(portname, 1, 1) ~= ":" then portname = ":" .. portname end
  local port = manager.machine.ioport.ports[portname]
  if not port then return end
  for _, fld in pairs(port.fields) do
    if (fld.mask & mask) ~= 0 then fld:set_value(value) end
  end
end

local n = 0
emu.add_machine_frame_notifier(function()
  n = n + 1
  for _, pr in ipairs(presses) do
    if n == pr.f then setport(pr.port, pr.mask, pr.value) end
  end
  if logf then logf:write("F ", n, " ", now_cycles(), "\n") end
  if framesf then
    local px = scr:pixels()
    framesf:write(px)
  end
  if n >= nmax then
    if logf then logf:flush(); logf:close() end
    if framesf then framesf:close() end
    manager.machine:exit()
  end
end)
