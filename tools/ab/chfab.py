#!/usr/bin/env python3
"""chfab.py -- shared helpers for the whole-machine A/B tools (MAME is the
reference).

    import chfab

MAME is run with `-video none -sound none` and killed hard if it outlives its
budget (it ignores SIGTERM in that mode). Frames are what MAME's screen holds at
each frame notifier: the visible area, 204 wide, 232 high on the NTSC machines
and 290 on the PAL ones (rows drawn 5 lines tall); ours are always 204 x 232.

Frame pairing (see tools/ab/chf_frames.lua): MAME's frame notifier runs after
the screen update but pixels() still holds the previous frame, so ours[i] is
MAME's pixel dump i + 1, and ours[i]'s VRAM record is MAME's VRAM record i.
"""
import os
import signal
import subprocess
import tempfile

MAME_DIR = os.path.expanduser(os.environ.get("MAME_DIR", "~/Workspace/mame"))
MAME = os.path.join(MAME_DIR, "mame")
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
W = 204
H_NTSC, H_PAL = 232, 290
OUR_H = 232
OUR_FRAME = W * OUR_H * 4

# our machine name -> (MAME's name, PAL?, the CPU's integer clock, the first BIOS half)
MACHINES = {
    "channelf": ("channelf", False, 1789772, "sl31253"),
    "channlf2": ("channlf2", False, 1789772, "sl90025"),
    "sabavdpl": ("sabavdpl", True, 2000000, "sl31253"),
    "sabavpl2": ("sabavpl2", True, 2000000, "sl90025"),
}
# MAME's frame notifier: ours[i] = MAME pixel dump i + 1
LEAD = 1

VRAM_REC = 8192 + 4


def mame_frame_bytes(pal):
    return W * (H_PAL if pal else H_NTSC) * 4


def run_mame(cart, lua, env, seconds, machine="channelf", slot=None, bios=None, extra=(), timeout=None,
             log=None, softlist=None, sound=False):
    """Run MAME headless for `seconds` of emulated time, hard-killed after
    `timeout` wall seconds. `cart` is a file for -cart (or None); `slot` is
    -cartslot (e.g. "fujinet"); `softlist` is a software name to load from the
    list (with `extra` carrying a private -hashpath and -rompath).
    Returns the exit status. MAME runs from its own directory."""
    mname, pal, hz, first = MACHINES[machine]
    cmd = [MAME, mname, "-bios", bios or first, "-video", "none", "-sound", "none",
           "-seconds_to_run", str(seconds), "-nothrottle", "-skip_gameinfo"]
    if lua:
        cmd += ["-autoboot_script", lua]
    if slot:
        cmd += ["-cartslot", slot]
    if cart:
        cmd += ["-cart", cart]
    if softlist:
        cmd += ["-cart", softlist]
    cmd += list(extra)
    e = dict(os.environ)
    e.update({k: str(v) for k, v in env.items()})
    e.setdefault("CHF_CPU_HZ", str(hz))
    e.setdefault("FUJINET_TCP", "127.0.0.1:9")      # never the user's 9995 unless a test says so
    logf = open(log, "wb") if log else subprocess.DEVNULL
    p = subprocess.Popen(cmd, env=e, cwd=MAME_DIR, stdout=logf, stderr=subprocess.STDOUT,
                         start_new_session=True)
    try:
        p.wait(timeout=timeout or (30 + seconds * 3))
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
    if log:
        logf.close()
    return p.returncode


def find_runner():
    env = os.environ.get("CHF_HEADLESS")
    if env:
        return env
    for cand in ("build-wp2/core/tests/chf_headless", "build/core/tests/chf_headless",
                 "build-core/core/tests/chf_headless"):
        c = os.path.join(ROOT, cand)
        if os.path.exists(c):
            return c
    raise SystemExit("set CHF_HEADLESS to the chf_headless binary")


def bios_dir():
    """The directory holding the real BIOS (sl31253.rom, sl90025.rom, sl31254.rom),
    $CHF_BIOS_DIR, else MAME's own roms/channelf.zip unpacked next to nothing: None
    when absent (tests that need it SKIP)."""
    d = os.environ.get("CHF_BIOS_DIR")
    if d and os.path.exists(os.path.join(d, "sl31254.rom")):
        return d
    return None


def mame_to_ours(raw, pal):
    """One MAME pixel dump -> our 204 x 232 frame. The PAL machines draw each VRAM
    row 5 lines tall: take the first line of each row (and check the other four
    are identical, else the dump is not what we think it is)."""
    if not pal:
        return raw
    out = bytearray()
    rowb = W * 4
    for r in range(58):
        base = r * 5 * rowb
        line = raw[base:base + rowb]
        for k in range(1, 5):
            if raw[base + k * rowb:base + (k + 1) * rowb] != line:
                raise ValueError("PAL row %d: its five lines differ" % r)
        out += line * 4
    return bytes(out)


def load_mame_frames(path, pal):
    """MAME's pixel dumps -> a list of our-shaped frames, index = MAME's dump."""
    if not os.path.exists(path):
        return []
    data = open(path, "rb").read()
    fb = mame_frame_bytes(pal)
    return [mame_to_ours(data[i:i + fb], pal) for i in range(0, len(data) - fb + 1, fb)]


def load_ours_frames(path):
    data = open(path, "rb").read() if os.path.exists(path) else b""
    return [data[i:i + OUR_FRAME] for i in range(0, len(data) - OUR_FRAME + 1, OUR_FRAME)]


def load_vram(path):
    """Per-frame (vram bytes, latches bytes) records of -vram / CHF_VRAM."""
    data = open(path, "rb").read() if os.path.exists(path) else b""
    return [(data[i:i + 8192], data[i + 8192:i + VRAM_REC]) for i in range(0, len(data) - VRAM_REC + 1, VRAM_REC)]


def fnv_frame(px):
    """FNV-1a-style 64-bit hash of a frame's 0x00RRGGBB words, one multiply per
    pixel -- what chf_headless -hash and the config_frames test compute."""
    h = 14695981039346656037
    for i in range(0, len(px), 4):
        h ^= int.from_bytes(px[i:i + 3], "little")
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def work_dir():
    w = os.environ.get("CHFAB_WORK") or tempfile.mkdtemp(prefix="chfab-")
    os.makedirs(w, exist_ok=True)
    return w


# ---- inputs: our events -> MAME's ioport fields --------------------------------------------------
# panel bit k (0 TIME, 1 MODE, 2 HOLD, 3 START) is port 0 bit k, and a controller bit is the
# controller byte's bit, in MAME's PANEL / RIGHT_C / LEFT_C ports. Player 1 (ours 0) is RIGHT_C.
def panel_press(frame, bit, down):
    """-> a CHF_PRESS item. MAME applies a value set from its frame notifier one frame late
    (the ioport layer polls at the next frame), so its events go one frame earlier than ours,
    which latch at the start of the frame they name."""
    return "%d:PANEL:%d:%d" % (max(frame - 1, 0), 1 << bit, down)


def ctl_press(frame, player, bit, down):
    return "%d:%s:%d:%d" % (max(frame - 1, 0), "RIGHT_C" if player == 0 else "LEFT_C", 1 << bit, down)


# ---- the probe machine (tools/ab/probes/vm.asm) ---------------------------------------------------
ASL = os.path.expanduser(os.environ.get("ASL", "~/asl/asl"))
P2BIN = os.path.expanduser(os.environ.get("P2BIN", "~/asl/p2bin"))
LOGPORT = 0xF0


def have_asl():
    return os.path.exists(ASL) and os.path.exists(P2BIN)


_vm_cache = {}


def vm_image(work):
    """The assembled probe machine: $0800-$0AFF as bytes (the script goes at $0B00)."""
    if work in _vm_cache:
        return _vm_cache[work]
    src = os.path.join(HERE, "probes", "vm.asm")
    p = os.path.join(work, "vm.p")
    b = os.path.join(work, "vm.bin")
    subprocess.run([ASL, src, "-q", "-o", p], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   cwd=work)
    subprocess.run([P2BIN, p, b, "-r", "0x800-0xaff", "-l", "0xff", "-q"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _vm_cache[work] = open(b, "rb").read()
    assert len(_vm_cache[work]) == 0x300
    return _vm_cache[work]


class Script:
    """A probe's byte script: build it with the methods, then `.image(work)`."""

    def __init__(self):
        self.b = bytearray()

    def w0(self, v):
        self.b += bytes((1, v & 0xFF))
        return self

    def w1(self, v):
        self.b += bytes((2, v & 0xFF))
        return self

    def w4(self, v):
        self.b += bytes((3, v & 0xFF))
        return self

    def w5(self, v):
        self.b += bytes((4, v & 0xFF))
        return self

    def r(self, port):
        self.b += bytes(({0: 5, 1: 6, 4: 7, 5: 8}[port],))
        return self

    def wait(self, n):
        while n > 0:
            k = min(n, 255)
            self.b += bytes((9, k))
            n -= k
        return self

    def out(self, port, v):
        self.b += bytes((10, port & 0xFF, v & 0xFF))
        return self

    def inp(self, port):
        self.b += bytes((11, port & 0xFF))
        return self

    def store(self, addr, v):
        self.b += bytes((12, addr >> 8 & 0xFF, addr & 0xFF, v & 0xFF))
        return self

    def load(self, addr):
        self.b += bytes((13, addr >> 8 & 0xFF, addr & 0xFF))
        return self

    def pix(self, row, col, val, strobe=(0x60, 0x50), sound=0):
        """The BIOS's own sequence: colour, column, row (all complemented), strobe high, low."""
        self.w1(~(val << 6)).w4(~col).w5((~row & 0x3F) | (sound << 6))
        for s_ in strobe:
            self.w0(s_)
        return self

    def image(self, work, size=0x800):
        vm = vm_image(work)
        img = bytearray(vm) + bytes(self.b) + b"\x00"
        if len(img) > 0x2000:
            raise ValueError("probe script too long: %d" % len(img))
        img += b"\xFF" * (max(size, len(img)) - len(img))
        return bytes(img)


def write_probe(script, work, name):
    path = os.path.join(work, name)
    open(path, "wb").write(script.image(work))
    return path


# ---- one A/B run: MAME against chf_headless ------------------------------------------------------
def parse_iolog(path):
    """-> [(cycle, port, data)] of the `W` lines (the `F` lines mean different things on the two sides)."""
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path):
        t = line.split()
        if t and t[0] == "W":
            out.append((int(t[1]), int(t[2]), int(t[3])))
    return out


def ctl_args(ctls):
    """chf_headless takes a controller's whole byte per frame (-ctl FRAME:PLAYER:MASK): fold
    the bit events (frame, player, bit, down) into the held mask after each."""
    held = {0: 0, 1: 0}
    out = []
    for f, p, b, d in sorted(ctls, key=lambda e: e[0]):
        held[p] = (held[p] | (1 << b)) if d else (held[p] & ~(1 << b))
        out += ["-ctl", "%d:%d:%d" % (f, p, held[p])]
    return out


class Result:
    def __init__(self):
        self.n = 0                  # frames compared
        self.diff = []              # frame indexes whose pictures differ
        self.vdiff = []             # VRAM / latch records that differ
        self.nrec = 0
        self.iolog = None           # None: not asked; else (n_mame, n_ours, first mismatch or None)
        self.tick = None            # None: not asked; else (n compared, first mismatch or None, last tick, n frames)
        self.ok = False
        self.err = ""


def ab(machine, cart, frames, work, keys=(), ctls=(), fujinet=False, bios=None, softlist=None, mame_extra=(),
       ours_extra=(), iolog=False, skip=0, boip=None, mame_env=None, mame_slot=None, game_type=None, resets=(),
       tick=False):
    """Run MAME and chf_headless on one cartridge and compare every frame's picture, the VRAM and
    latches, and (iolog) every I/O write with its CPU clock. `cart` is the image both load (MAME's
    -cart, or for a software-list load `softlist` names the entry and `cart` is what ours loads)."""
    mname, pal, hz, first = MACHINES[machine]
    r = Result()
    biosd = bios_dir()
    runner = find_runner()
    files = {k: os.path.join(work, k) for k in ("mame.raw", "mame.vram", "ours.raw", "ours.vram", "mame.io", "ours.io",
                                                "mame.tick", "ours.tick")}
    for p in files.values():
        if os.path.exists(p):
            os.unlink(p)
    press = [panel_press(f, b, d) for f, b, d in keys] + [ctl_press(f, p, b, d) for f, p, b, d in ctls]
    env = {"CHF_FRAMES": files["mame.raw"], "CHF_VRAM": files["mame.vram"], "CHF_FRAMES_N": frames + 2,
           "CHF_PRESS": ",".join(press)}
    if iolog:
        env["CHF_IOLOG"] = files["mame.io"]
    if tick:
        env["CHF_TICK"] = files["mame.tick"]
    if resets:
        # MAME's soft reset at notifier n lands between frame n - 1 and n: after our frame n - 1
        env["CHF_RESET"] = ",".join(str(f + 1) for f in resets)
    env.update(mame_env or {})
    run_mame(None if softlist else os.path.abspath(cart), os.path.join(HERE, "chf_frames.lua"), env, frames / 49 + 4,
             machine=machine, slot=mame_slot or ("fujinet" if fujinet else None), timeout=600, softlist=softlist,
             extra=mame_extra)
    cmd = [runner, "-machine", machine, "-bios-dir", biosd, "-bios", first, "-frames", str(frames),
           "-raw", files["ours.raw"], "-vram", files["ours.vram"], "-cart" if fujinet else "-game", cart]
    if iolog:
        cmd += ["-iolog", files["ours.io"]]
    if tick:
        cmd += ["-tick", files["ours.tick"]]
    if game_type:
        cmd += ["-type", game_type]
    if boip:
        cmd += ["-boip", boip]
    for f, b, d in keys:
        cmd += ["-key", "%d:%d:%d" % (f, b, d)]
    for f in resets:
        cmd += ["-softreset", str(f)]
    cmd += ctl_args(ctls) + list(ours_extra)
    q = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=900)
    if q.returncode != 0:
        r.err = q.stderr.decode(errors="replace")[-300:]
        return r
    a = load_mame_frames(files["mame.raw"], pal)[LEAD:]
    b = load_ours_frames(files["ours.raw"])
    av, bv = load_vram(files["mame.vram"]), load_vram(files["ours.vram"])
    r.n = min(len(a), len(b))
    r.diff = [i for i in range(r.n) if a[i] != b[i] and i >= skip]
    r.nrec = min(len(av), len(bv), frames)
    r.vdiff = [i for i in range(r.nrec) if av[i] != bv[i]]
    if iolog:
        ma, ob = parse_iolog(files["mame.io"]), parse_iolog(files["ours.io"])
        # MAME ran two frames more; ours stops at `frames`: compare the common prefix of the time span
        last = ob[-1][0] if ob else 0
        ma = [e for e in ma if e[0] <= last]
        first_bad = None
        for i in range(max(len(ma), len(ob))):
            if i >= len(ma) or i >= len(ob) or ma[i] != ob[i]:
                first_bad = (i, ma[i] if i < len(ma) else None, ob[i] if i < len(ob) else None)
                break
        r.iolog = (len(ma), len(ob), first_bad)
    if tick:
        # MAME's notifier reads the tick at the vertical blank's exact time; ours reports that (column 2)
        # and what the console reads a few clocks later (column 3), which may be one more -- or, once a
        # booted game has switched the mailbox off, ROM bytes, which MAME reads the same
        ta = [l.split() for l in open(files["mame.tick"])] if os.path.exists(files["mame.tick"]) else []
        tb = [l.split() for l in open(files["ours.tick"])] if os.path.exists(files["ours.tick"]) else []
        m = min(len(ta), len(tb))
        bad = [i for i in range(m) if ta[i][0] != tb[i][0] or ta[i][1] not in (tb[i][1], tb[i][2])]
        r.tick = (m, bad[0] if bad else None, int(tb[-1][1]) if tb else None, len(tb))
    r.ok = (r.n > 0 and not r.diff and not r.vdiff and (r.iolog is None or r.iolog[2] is None) and r.nrec == frames
            and (r.tick is None or (r.tick[0] > 0 and r.tick[1] is None)))
    return r


def report(name, machine, r):
    """One line for a Result."""
    if r.err:
        return "FAIL %-34s %-8s %s" % (name[:34], machine, r.err.strip().splitlines()[-1] if r.err.strip() else "run failed")
    line = "%s %-34s %-8s %d/%d frames, %d/%d VRAM+latch records" % (
        "OK  " if r.ok else "DIFF", name[:34], machine, r.n - len(r.diff), r.n, r.nrec - len(r.vdiff), r.nrec)
    if r.iolog is not None:
        line += ", %d/%d I/O writes" % (r.iolog[1] if r.iolog[2] is None else r.iolog[2][0], r.iolog[1])
    if r.tick is not None:
        line += ", tick %s (%d frames -> %s)" % ("identical" if r.tick[1] is None else "DIFFERS at %d" % r.tick[1],
                                                 r.tick[3], r.tick[2])
    if not r.ok:
        line += " | first differing: frames %s, records %s" % (",".join(map(str, r.diff[:6])) or "-",
                                                               ",".join(map(str, r.vdiff[:6])) or "-")
        if r.iolog is not None and r.iolog[2]:
            line += ", io #%d mame=%s ours=%s" % r.iolog[2]
    return line
