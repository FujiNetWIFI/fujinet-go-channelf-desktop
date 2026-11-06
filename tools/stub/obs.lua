-- obs.lua -- black-box observer for the Channel F BIOS entry points
-- (MAME `channelf` driver).  Driven by tools/stub/observe.py; see SPEC.md.
--
-- The driver never looks at, dumps or disassembles BIOS bytes.  It only
--   1. waits for the console to hand control to the probe cartridge ($0802),
--   2. writes a complete pre-state (A, W, ISAR, DC0, DC1, PC1, the 64-byte
--      scratchpad, the port latches, all of VRAM),
--   3. points the CPU at a launch pad in the cartridge (NOP, then PI / PK /
--      LR P0,Q / JMP $0000), and
--   4. waits for the CPU to come back to the pad's return spin, then writes
--      the post-state as one JSON line.
-- Only the state before the call and after the return is recorded, plus a
-- summary of the bus traffic in between: OUT instructions (port, value, which
-- VRAM cells they strobed), IN instructions (count per port) and reads of the
-- cartridge area before it is entered.  No program counter inside the BIOS is
-- ever sampled, and no BIOS address is read.
--
-- Environment:
--   OBS_MODE    cases | cold | bad   (default cases)
--   OBS_CASES   input file (mode cases): lines
--                 P <name> <base> <ret>        pad: first byte (NOP) and the
--                                              address of its return spin (hex)
--                 M maxframes <n>              per-case frame limit (decimal)
--                 C <id> <pad> A W IS DC0 DC1 PC1 P0 P1 P4 P5 VSPEC r0..r63
--                                              (hex; VSPEC = z | c<v> | r<seed>)
--   OBS_OUT     output file (JSON lines)
--   OBS_FRAMES  mode bad/cold: frames to run before giving up (default 300)
--
-- MAME runs this chunk on a coroutine that the collector frees once the chunk
-- returns; the taps and the notifier keep using its lua_State, so one GC
-- cycle crashes MAME.  Never collect, and keep the per-write work free of
-- allocations.
collectgarbage("stop")

local mode = os.getenv("OBS_MODE") or "cases"
local outf = assert(io.open(assert(os.getenv("OBS_OUT")), "w"))
local maxframes_boot = tonumber(os.getenv("OBS_FRAMES") or "300")

local m = manager.machine
local cpu = m.devices[":maincpu"]
local reg = cpu.spaces["register"]      -- the 64-byte scratchpad
local ios = cpu.spaces["io"]
local prog = cpu.spaces["program"]
local vram = m.memory.regions[":vram"]
local function item(n) return emu.item(cpu.items["0/" .. n]) end
local it_pc0, it_dbus = item("m_pc0"), item("m_dbus")
local sA, sW, sIS = cpu.state["A"], cpu.state["W"], cpu.state["IS"]
local sPC1, sDC0, sDC1 = cpu.state["PC1"], cpu.state["DC0"], cpu.state["DC1"]

local APC = 1000000000000000000 // 895000   -- attoseconds per CPU tick
local function now_ticks()
  -- seconds * 1e18 would overflow a Lua integer after 9 s of machine time
  local tm = m.time
  return tm.seconds * 895000 + tm.attoseconds // APC
end

local ENTRY = 0x0802

---------------------------------------------------------------------------
-- port-write tap: counters only (no allocation)
---------------------------------------------------------------------------
local cnt = {}           -- port -> number of writes
local lastv = {}         -- port -> last value written
local seen = {}          -- port*256+value -> true
local seenlist = {}      -- list of port*256+value, in first-seen order
local nseen = 0
local first = {}         -- first 24 writes: port*256+value
local nfirst = 0
local strobes = 0
local nwrites = 0
local t_firstw, t_lastw = nil, nil
local rowr, colr, valr = 0, 0, 0     -- the driver's val/row/col registers
local wrv = {}           -- cell -> last value strobed in, 4 = never written
local wrn = 0
for i = 0, 8191 do wrv[i] = 4 end

local function reset_io_log(p1, p4, p5)
  cnt, lastv, seen, seenlist, nseen = {}, {}, {}, {}, 0
  first, nfirst, strobes, nwrites = {}, 0, 0, 0
  t_firstw, t_lastw = nil, nil
  valr = ((p1 ~ 0xff) >> 6) & 3
  colr = (p4 | 0x80) ~ 0xff
  rowr = (p5 | 0xc0) ~ 0xff
  for i = 0, 8191 do wrv[i] = 4 end
end

-- reads of the cartridge area ($0800-$FFFF) made before the cartridge is
-- entered, and IN instructions (per port): what a RESET path looks at
local cartreads = {}
local ncartreads = 0
local logging_reads = true
local inr = {}
ios:install_read_tap(0, 0xff, "obs_ior", function(off, data, mask)
  inr[off] = (inr[off] or 0) + 1
end)
prog:install_read_tap(0x0800, 0xffff, "obs_cartrd", function(off, data, mask)
  if logging_reads and ncartreads < 64 and off ~= 0x0802 and off ~= 0x0803 and not (off >= 0x0820 and off < 0x0880) then
    ncartreads = ncartreads + 1; cartreads[ncartreads] = off
  end
end)

ios:install_write_tap(0, 0xff, "obs_iow", function(off, data, mask)
  data = data & 0xff
  cnt[off] = (cnt[off] or 0) + 1
  lastv[off] = data
  nwrites = nwrites + 1
  local key = off * 256 + data
  if not seen[key] and nseen < 400 then
    seen[key] = true; nseen = nseen + 1; seenlist[nseen] = key
  end
  if nfirst < 24 then nfirst = nfirst + 1; first[nfirst] = key end
  if off == 1 then valr = ((data ~ 0xff) >> 6) & 3
  elseif off == 4 then colr = (data | 0x80) ~ 0xff
  elseif off == 5 then rowr = (data | 0xc0) ~ 0xff
  elseif off == 0 and (data & 0x20) ~= 0 then
    strobes = strobes + 1
    wrv[rowr * 128 + colr] = valr
  end
end)

---------------------------------------------------------------------------
-- entry / launch / return taps (cartridge ROM only; never BIOS addresses)
---------------------------------------------------------------------------
local entered = false
local t_entry = nil
prog:install_read_tap(ENTRY, ENTRY, "obs_entry", function(off, data, mask)
  if not t_entry then t_entry = now_ticks() end
  entered = true
end)

local pads = {}          -- name -> {base=, ret=}
local cur = nil          -- current case
local t_launch, t_ret = nil, nil
local ret_watch = -1
prog:install_read_tap(0x0800, 0x087f, "obs_pad", function(off, data, mask)
  if off == ret_watch + 0 and not t_ret and t_launch then t_ret = now_ticks() end
  if cur and off == cur.base + 1 and not t_launch then t_launch = now_ticks() end
end)

---------------------------------------------------------------------------
-- JSON helpers
---------------------------------------------------------------------------
local function hexlist(t, n)
  local s = {}
  for i = 1, n do s[i] = tostring(t[i]) end
  return "[" .. table.concat(s, ",") .. "]"
end

local function vram_rle()
  local parts = {}
  local v = vram:read_u8(0)
  local n = 1
  for i = 1, 8191 do
    local x = vram:read_u8(i)
    if x == v then n = n + 1
    else parts[#parts + 1] = v .. "," .. n; v = x; n = 1 end
  end
  parts[#parts + 1] = v .. "," .. n
  return "[" .. table.concat(parts, ";") .. "]"
end

local function wr_rle()
  local parts = {}
  local v = wrv[0]
  local n = 1
  for i = 1, 8191 do
    local x = wrv[i]
    if x == v then n = n + 1
    else parts[#parts + 1] = v .. "," .. n; v = x; n = 1 end
  end
  parts[#parts + 1] = v .. "," .. n
  return "[" .. table.concat(parts, ";") .. "]"
end

local function io_json()
  local s = {}
  local cr = {}
  for i = 1, ncartreads do cr[i] = tostring(cartreads[i]) end
  local ir = {}
  local ip = {}
  for p in pairs(inr) do ip[#ip + 1] = p end
  table.sort(ip)
  for _, p in ipairs(ip) do ir[#ir + 1] = string.format('"%d":%d', p, inr[p]) end
  local ports = {}
  for p in pairs(cnt) do ports[#ports + 1] = p end
  table.sort(ports)
  local c, l = {}, {}
  for _, p in ipairs(ports) do
    c[#c + 1] = string.format('"%d":%d', p, cnt[p])
    l[#l + 1] = string.format('"%d":%d', p, lastv[p])
  end
  local fl = {}
  for i = 1, nfirst do fl[i] = tostring(first[i]) end
  local vl = {}
  for i = 1, nseen do vl[i] = tostring(seenlist[i]) end
  return string.format('{"n":%d,"cnt":{%s},"last":{%s},"strobes":%d,"first":[%s],"seen":[%s],"cartreads":[%s],"inr":{%s},"wcells":%s}',
    nwrites, table.concat(c, ","), table.concat(l, ","), strobes,
    table.concat(fl, ","), table.concat(vl, ","), table.concat(cr, ","), table.concat(ir, ","), wr_rle())
end

local function regs_json()
  local t = {}
  for i = 0, 63 do t[i + 1] = reg:read_u8(i) end
  return hexlist(t, 64)
end

---------------------------------------------------------------------------
-- cases
---------------------------------------------------------------------------
local cases = {}
local maxframes = 400
if mode == "cases" then
  local cf = assert(io.open(assert(os.getenv("OBS_CASES")), "r"))
  for line in cf:lines() do
    local tok = {}
    for w in string.gmatch(line, "%S+") do tok[#tok + 1] = w end
    if tok[1] == "P" then
      pads[tok[2]] = { base = tonumber(tok[3], 16), ret = tonumber(tok[4], 16) }
    elseif tok[1] == "M" and tok[2] == "maxframes" then
      maxframes = tonumber(tok[3])
    elseif tok[1] == "C" then
      local c = { id = tonumber(tok[2]), pad = tok[3] }
      c.a, c.w, c.is = tonumber(tok[4], 16), tonumber(tok[5], 16), tonumber(tok[6], 16)
      c.dc0, c.dc1, c.pc1 = tonumber(tok[7], 16), tonumber(tok[8], 16), tonumber(tok[9], 16)
      c.p0, c.p1, c.p4, c.p5 = tonumber(tok[10], 16), tonumber(tok[11], 16), tonumber(tok[12], 16), tonumber(tok[13], 16)
      c.vspec = tok[14]
      c.r = {}
      for i = 0, 63 do c.r[i] = tonumber(tok[15 + i], 16) end
      cases[#cases + 1] = c
    end
  end
  cf:close()
end

local function fill_vram(spec)
  local kind = string.sub(spec, 1, 1)
  local sum = 0
  if kind == "z" then
    for i = 0, 8191 do vram:write_u8(i, 0) end
  elseif kind == "c" then
    local v = tonumber(string.sub(spec, 2), 16)
    for i = 0, 8191 do vram:write_u8(i, v); sum = sum + i * v end
  elseif kind == "r" then
    local x = tonumber(string.sub(spec, 2), 16)
    for i = 0, 8191 do
      x = x * 6364136223846793005 + 1442695040888963407
      local v = (x >> 33) & 3
      vram:write_u8(i, v)
      sum = sum + i * v
    end
  end
  return sum & 0xffffffff
end

local function inject(c)
  -- ports first (a port-0 strobe writes one VRAM cell; the fill comes after)
  ios:write_u8(1, c.p1); ios:write_u8(4, c.p4); ios:write_u8(5, c.p5); ios:write_u8(0, c.p0)
  c.vsum = fill_vram(c.vspec)
  for i = 0, 63 do reg:write_u8(i, c.r[i]) end
  sA.value = c.a; sW.value = c.w; sIS.value = c.is
  sDC0.value = c.dc0; sDC1.value = c.dc1; sPC1.value = c.pc1
  reset_io_log(c.p1, c.p4, c.p5)
  cartreads, ncartreads, inr = {}, 0, {}
  logging_reads = (c.pad == "jmp")
  local pad = pads[c.pad]
  c.base = pad.base
  cur = c
  ret_watch = pad.ret
  t_launch, t_ret = nil, nil
  it_pc0:write(0, pad.base + 1)   -- the NOP at the pad base is about to run
  it_dbus:write(0, 0x2b)
end

local function dump_case(c, ok, frames)
  local pad = pads[c.pad]
  outf:write(string.format('{"id":%d,"ok":%d,"frames":%d,"ticks":%d,"a":%d,"w":%d,"is":%d,"pc1":%d,"dc0":%d,"dc1":%d,"vsum":%d,"r":%s,"vram":%s,"io":%s}\n',
    c.id, ok and 1 or 0, frames,
    (t_ret and t_launch) and (t_ret - t_launch) or -1,
    sA.value, sW.value, sIS.value, sPC1.value, sDC0.value, sDC1.value, c.vsum,
    regs_json(), vram_rle(), io_json()))
end

---------------------------------------------------------------------------
-- main loop (one call per video frame)
---------------------------------------------------------------------------
local frame = 0
local ci = 0
local cframes = 0
local phase = "boot"
local boot_t = nil

local finished = false
local function finish()
  finished = true
  outf:flush(); outf:close()
  m:exit()
end

local function step()
  if finished then return end
  frame = frame + 1
  if phase == "boot" then
    if mode == "bad" then
      if frame >= maxframes_boot then
        outf:write(string.format('{"mode":"bad","entered":%d,"frames":%d}\n', entered and 1 or 0, frame))
        finish()
      end
      return
    end
    local pc0, dbus = it_pc0:read(0), it_dbus:read(0)
    if entered and pc0 == ENTRY + 1 and dbus == 0x90 then
      if mode == "cold" then
        outf:write(string.format('{"mode":"cold","entered":1,"frames":%d,"entry_ticks":%d,"a":%d,"w":%d,"is":%d,"pc1":%d,"dc0":%d,"dc1":%d,"r":%s,"vram":%s,"io":%s}\n',
          frame, t_entry or -1, sA.value, sW.value, sIS.value, sPC1.value, sDC0.value, sDC1.value,
          regs_json(), vram_rle(), io_json()))
        finish()
      end
      phase = "cases"
      ci = 1
      inject(cases[ci]); cframes = 0
      return
    end
    if frame >= maxframes_boot then
      outf:write('{"mode":"' .. mode .. '","entered":0}\n')
      finish()
    end
    return
  end
  -- phase cases
  cframes = cframes + 1
  local c = cases[ci]
  local pad = pads[c.pad]
  local pc0, dbus = it_pc0:read(0), it_dbus:read(0)
  local done = (pc0 == pad.ret + 1 and dbus == 0x90)
  if done or cframes >= maxframes then
    dump_case(c, done, cframes)
    ci = ci + 1
    if ci > #cases then finish() return end
    if not done then
      -- put the CPU back in the cartridge spin so the next case starts clean
      sPC1.value = ENTRY
      it_pc0:write(0, ENTRY + 1); it_dbus:write(0, 0x90)
    end
    inject(cases[ci]); cframes = 0
  end
end

emu.add_machine_frame_notifier(function()
  local ok, err = pcall(step)
  if not ok then
    outf:write('{"error":"' .. tostring(err):gsub('"', "'") .. '"}\n')
    finish()
  end
end)
