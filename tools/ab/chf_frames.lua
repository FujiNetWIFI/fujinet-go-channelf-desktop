-- chf_frames.lua -- dump every frame of MAME's `channelf` family, for the A/B
-- against core/chf (tools/ab/*.py).
--
-- Environment:
--   CHF_FRAMES    binary file: each notifier's screen bitmap, as MAME holds it
--                 (visible area, 204 wide; 232 high NTSC, 290 PAL), 32-bit LE ARGB
--   CHF_VRAM      binary file: at each notifier the 8192 VRAM bytes and the four
--                 port latches (ports 0, 1, 4, 5 as last written), 8196 bytes
--   CHF_IOLOG     text file: `W <cycle> <port> <data>` for every I/O write and
--                 `F <n> <cycle>` per frame, the cycle being the F8's clock
--                 count (the machine's time over the CPU's attoseconds a clock,
--                 which is what the F8 core's cycle counter is)
--   CHF_TICK      text file: `<n> <tick>` per frame, the FujiNet cartridge's 60 Hz
--                 tick as the console reads it at $FC0E/F (high, low, high)
--   CHF_CPU_HZ    the CPU clock MAME was given as an integer: 1789772 (NTSC,
--                 the default) or 2000000 (PAL)
--   CHF_FRAMES_N  stop after this many frames (default 60)
--   CHF_PRESS     "frame:port:mask:value,..." from that frame, set the fields of
--                 ioport `port` that `mask` selects to `value` (0 or 1), e.g.
--                 120:RIGHT_C:128:1,126:RIGHT_C:128:0
--   CHF_RESET     "n,n,...": soft-reset the machine (MAME's F3) at those notifiers
--   CHF_PROBE     a Lua file run at the first frame with the helpers below
--
-- MAME's frame notifier runs AFTER the screen update but pixels() still holds
-- the previous frame, so dump j of CHF_FRAMES is frame j's picture (dump 0 is
-- blank) while CHF_VRAM's record j is the VRAM at frame j + 1 -- the VRAM the
-- frame the NEXT dump shows was drawn from. tools/ab/chfab.py has the pairing.
--
-- MAME runs this chunk on a coroutine that the collector frees once the chunk
-- returns; the taps and the notifier keep using its lua_State, so one GC cycle
-- crashes MAME. Never collect.
collectgarbage("stop")

-- A soft reset runs the autoboot script again; the first run's taps and notifier
-- are still live, so the second must do nothing.
if _G.chf_frames_started then return end
_G.chf_frames_started = true

local function openw(var)
  local p = os.getenv(var)
  return p and assert(io.open(p, "wb")) or nil
end
local framesf = openw("CHF_FRAMES")
local vramf = openw("CHF_VRAM")
local logf = openw("CHF_IOLOG")
local tickf = openw("CHF_TICK")
local nmax = tonumber(os.getenv("CHF_FRAMES_N") or "60")
local cpu = manager.machine.devices[":maincpu"]
local scr = manager.machine.screens[":screen"]
local vram = manager.machine.memory.regions[":vram"]

local CPU_HZ = tonumber(os.getenv("CHF_CPU_HZ") or "1789772")
local APC = 1000000000000000000 // CPU_HZ         -- attoseconds per CPU clock (truncated, as HZ_TO_ATTOSECONDS)
-- floor((sec * 1e18 + atto) / APC) without overflowing a 64-bit integer past 9 seconds
local ONE = 1000000000000000000
local Q, R = ONE // APC, ONE % APC
local function now_cycles()
  local tm = manager.machine.time
  return tm.seconds * Q + (tm.seconds * R + tm.attoseconds) // APC
end

-- the port latches, from the I/O write stream (the driver's m_latch is not exported)
local latch = { [0] = 0, [1] = 0, [4] = 0, [5] = 0 }
local taps = {}
taps[1] = cpu.spaces["io"]:install_write_tap(0, 0xff, "chf_iow", function(off, data, mask)
  if latch[off] ~= nil then latch[off] = data & 0xff end
  if logf then logf:write("W ", now_cycles(), " ", off, " ", data & 0xff, "\n") end
end)

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

local resets = {}
for n_ in string.gmatch(os.getenv("CHF_RESET") or "", "%d+") do resets[tonumber(n_)] = true end

local probe = os.getenv("CHF_PROBE") and loadfile(os.getenv("CHF_PROBE")) or nil

local n = 0
emu.add_machine_frame_notifier(function()
  n = n + 1
  for _, pr in ipairs(presses) do
    if n == pr.f then setport(pr.port, pr.mask, pr.value) end
  end
  if probe and n == 1 then probe() end
  if logf then logf:write("F ", n, " ", now_cycles(), "\n") end
  if framesf then
    local px = scr:pixels()          -- (string, w, h): only the string
    framesf:write(px)
  end
  if vramf then
    vramf:write(vram:read(0, 8192))
    vramf:write(string.char(latch[0], latch[1], latch[4], latch[5]))
  end
  if resets[n] then manager.machine:soft_reset() end
  if tickf then
    local sp = cpu.spaces["program"]
    local hi = sp:read_u8(0xFC0F)
    local lo = sp:read_u8(0xFC0E)
    tickf:write(n - 1, " ", hi * 256 + lo, "\n")        -- frame index as chf_headless numbers them
  end
  if n >= nmax then
    if logf then logf:flush(); logf:close() end
    if framesf then framesf:flush(); framesf:close() end
    if vramf then vramf:flush(); vramf:close() end
    if tickf then tickf:flush(); tickf:close() end
    manager.machine:exit()
  end
end)
