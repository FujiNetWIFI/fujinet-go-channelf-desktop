"""obslib -- shared plumbing for observe.py: build the probe cartridge, write
case files, run MAME with obs.lua, read the JSON-lines results.

Clean-room note: nothing here reads BIOS bytes.  MAME loads the BIOS ROMs
itself; this module only hands it a probe cartridge and a Lua observer that
records machine state before and after a call.
"""
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
MAME_DIR = os.path.expanduser(os.environ.get("MAME_DIR", "~/Workspace/mame"))
MAME = os.environ.get("MAME", os.path.join(MAME_DIR, "mame"))
ASL = os.path.expanduser(os.environ.get("ASL", "~/asl/asl"))
P2BIN = os.path.expanduser(os.environ.get("P2BIN", "~/asl/p2bin"))

M64 = (1 << 64) - 1

# Launch pads (see probe.asm): name -> (first byte = NOP, return spin)
PADS_FALLBACK = {
    "clr": (0x820, 0x824), "push": (0x830, 0x834), "pop": (0x840, 0x844),
    "pk": (0x850, 0x852), "lr": (0x860, 0x862), "jmp": (0x870, 0x802),
}
ENTRY = 0x802


def build_probe(workdir):
    """Assemble probe.asm -> probe.bin; returns (bin_path, pads)."""
    os.makedirs(workdir, exist_ok=True)
    asm = os.path.join(workdir, "probe.asm")
    shutil.copyfile(os.path.join(HERE, "probe.asm"), asm)
    subprocess.run([ASL, "-q", "-L", "probe.asm"], cwd=workdir, check=True,
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    subprocess.run([P2BIN, "probe.p", "probe.bin", "-r", "0x800-0x87f"], cwd=workdir,
                   check=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    lab = {}
    for line in open(os.path.join(workdir, "probe.lst"), errors="replace"):
        m = re.match(r"^\s*\d+/\s*([0-9A-Fa-f]+) :.*?\b((?:pad|ret)_\w+|entry):", line)
        if m:
            lab[m.group(2)] = int(m.group(1), 16)
    pads = {}
    for k, (b, r) in PADS_FALLBACK.items():
        base = lab.get("pad_" + k)
        ret = lab.get("ret_" + k, ENTRY) if k != "jmp" else ENTRY
        assert base == b and ret == r, (k, base, ret, b, r)
        pads[k] = (base, ret)
    return os.path.join(workdir, "probe.bin"), pads


def probe_variant(path, byte0=None, byte1=None, out=None):
    """Copy the probe image with $0800/$0801 replaced."""
    data = bytearray(open(path, "rb").read())
    if byte0 is not None:
        data[0] = byte0
    if byte1 is not None:
        data[1] = byte1
    open(out, "wb").write(data)
    return out


# ---------------------------------------------------------------------------
# VRAM patterns (mirrors obs.lua fill_vram)
# ---------------------------------------------------------------------------
def vram_expand(spec):
    k = spec[0]
    if k == "z":
        return [0] * 8192
    if k == "c":
        return [int(spec[1:], 16)] * 8192
    if k == "r":
        x = int(spec[1:], 16)
        out = []
        for _ in range(8192):
            x = (x * 6364136223846793005 + 1442695040888963407) & M64
            out.append((x >> 33) & 3)
        return out
    raise ValueError(spec)


def vram_checksum(v):
    return sum(i * x for i, x in enumerate(v)) & 0xFFFFFFFF


def rle_decode(s):
    out = []
    if not s:
        return out
    for part in s:
        v, n = part
        out.extend([v] * n)
    return out


def parse_rle(txt):
    """'v,n;v,n' -> list of (v, n)"""
    return [tuple(int(t) for t in p.split(",")) for p in txt.split(";")]


# ---------------------------------------------------------------------------
# cases
# ---------------------------------------------------------------------------
class Case:
    __slots__ = ("id", "pad", "a", "w", "is_", "dc0", "dc1", "pc1", "p0", "p1", "p4",
                 "p5", "vspec", "r", "tag")

    def __init__(self, **kw):
        for s in self.__slots__:
            setattr(self, s, kw.get(s))
        if self.r is None:
            self.r = [0] * 64
        if self.tag is None:
            self.tag = ""

    def copy(self):
        c = Case()
        for s in self.__slots__:
            v = getattr(self, s)
            setattr(c, s, list(v) if isinstance(v, list) else v)
        return c

    def line(self):
        f = [
            "C", str(self.id), self.pad, "%x" % self.a, "%x" % self.w, "%x" % self.is_,
            "%x" % self.dc0, "%x" % self.dc1, "%x" % self.pc1,
            "%x" % self.p0, "%x" % self.p1, "%x" % self.p4, "%x" % self.p5, self.vspec,
        ] + ["%x" % x for x in self.r]
        return " ".join(f)


def random_case(rng, pad, pads, vspec=None, target=None):
    """A fully random pre-state (the launch overrides what it must)."""
    c = Case(
        pad=pad, a=rng.randrange(256), w=rng.randrange(32), is_=rng.randrange(64),
        dc0=rng.randrange(65536), dc1=rng.randrange(65536), pc1=rng.randrange(65536),
        p0=rng.randrange(256), p1=rng.randrange(256), p4=rng.randrange(256),
        p5=rng.randrange(256),
        vspec=vspec or ("r%x" % rng.randrange(1 << 32)),
        r=[rng.randrange(256) for _ in range(64)],
    )
    apply_launch(c, pads, target)
    return c


def target_of(pad):
    return {"clr": 0x00D0, "push": 0x0107, "pop": 0x011E}.get(pad)


def apply_launch(c, pads, target=None):
    """Fix the fields the launch instruction overrides, so the case text says
    what the routine really saw."""
    if c.pad in ("clr", "push", "pop"):
        t = target_of(c.pad)
        c.a = t >> 8
        c.pc1 = pads[c.pad][1]          # PI leaves the return address here
    elif c.pad == "pk":                 # K := target; PK leaves PC1 = return address
        c.r[12], c.r[13] = target >> 8, target & 0xFF
        c.pc1 = pads["pk"][1]
    elif c.pad == "lr":                 # Q := target; PC1 is ours to set
        c.r[14], c.r[15] = target >> 8, target & 0xFF
        c.pc1 = pads["lr"][1]
    elif c.pad == "jmp":                # JMP $0000 loads A with the high byte
        c.a = 0
    return c


def write_cases(path, pads, cases, maxframes=400):
    with open(path, "w") as f:
        for name, (base, ret) in pads.items():
            f.write("P %s %x %x\n" % (name, base, ret))
        f.write("M maxframes %d\n" % maxframes)
        for c in cases:
            f.write(c.line() + "\n")


def run_mame(bios, cart, env_extra, seconds, rompath=None, timeout=3600):
    # private config/nvram dirs: parallel runs must not share (or touch) the user's
    scratch = tempfile.mkdtemp(prefix="chfobs-")
    cmd = [MAME, "channelf", "-bios", bios, "-cart", cart,
           "-autoboot_script", os.path.join(HERE, "obs.lua"),
           "-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
           "-cfg_directory", scratch, "-nvram_directory", scratch,
           "-state_directory", scratch, "-seconds_to_run", str(int(seconds))]
    if rompath:
        cmd += ["-rompath", rompath]
    env = dict(os.environ)
    env.update(env_extra)
    try:
        p = subprocess.run(["timeout", "-s", "KILL", str(timeout)] + cmd, cwd=MAME_DIR, env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return p.returncode, p.stdout.decode(errors="replace")


def read_jsonl(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(_loads(line))
    return out


def _loads(line):
    # the Lua writes RLE lists as [v,n;v,n;...] (not JSON); convert those fields
    def fix(m):
        body = m.group(2)
        runs = [[int(x) for x in p.split(",")] for p in body.split(";")] if body else []
        return '"%s":%s' % (m.group(1), json.dumps(runs))
    line = re.sub(r'"(vram|wcells)":\[([0-9,;]*)\]', fix, line)
    return json.loads(line)


def run_cases(bios, cart, pads, cases, workdir, tag, rompath=None, jobs=8, maxframes=400,
              batch=None):
    """Run `cases` through MAME in parallel batches; returns {id: result}."""
    os.makedirs(workdir, exist_ok=True)
    if batch is None:
        batch = max(1, (len(cases) + jobs - 1) // jobs)
    chunks = [cases[i:i + batch] for i in range(0, len(cases), batch)]

    def one(k):
        chunk = chunks[k]
        cf = os.path.join(workdir, "%s.cases.%d" % (tag, k))
        of = os.path.join(workdir, "%s.out.%d" % (tag, k))
        write_cases(cf, pads, chunk, maxframes)
        if os.path.exists(of):
            os.unlink(of)
        env = {"OBS_MODE": "cases", "OBS_CASES": cf, "OBS_OUT": of}
        rc, log = run_mame(bios, cart, env, seconds=len(chunk) * maxframes // 30 + 60,
                           rompath=rompath, timeout=len(chunk) * maxframes // 20 + 300)
        if not os.path.exists(of):
            raise RuntimeError("MAME produced no output (rc=%s): %s" % (rc, log[-800:]))
        return read_jsonl(of)

    res = {}
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for rows in ex.map(one, range(len(chunks))):
            for r in rows:
                if "error" in r:
                    raise RuntimeError("obs.lua error: %s" % r["error"])
                if "id" not in r:
                    raise RuntimeError("the console never handed control to the probe cartridge at $0802 "
                                       "(is [$0800] == $55 honoured by this BIOS?): %s" % r)
                res[r["id"]] = r
    return res
