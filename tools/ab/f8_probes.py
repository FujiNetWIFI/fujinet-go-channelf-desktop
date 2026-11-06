#!/usr/bin/env python3
"""f8_probes.py -- generates the F8 conformance probe cartridges.

No assembler is needed: a probe is built here from a tiny text assembler (asm)
that knows the F3850's mnemonics as MAME's disassembler prints them. Each probe
is a Channel F cartridge image (byte $55 at $0800, code from $0802) which
MAME's `channelf` driver loads as a "chess" cart: ROM at $0800 (and $3000 up),
2K of RAM at $2800-$2FFF. The BIOS jumps to $0802; the probe sets up every
register it uses (it cannot trust what the BIOS left) and reaches its `start`
label, then runs to `done` (a JMP to itself). MAME's F8 and our core must trace
identically from start to done (tools/ab/f8_ab.py, tools/ab/romc_ab.py).

What a probe may touch: its ROM, the cart RAM, the open bus above the image
(reads $FF) and the I/O ports. Ports 0, 1, 4 and 5 are the console's latches (a
probe writes one before it reads it back; a port 0 write with bit 5 set draws
into video RAM, which no probe can read); every other port reads 0. It never
reads the BIOS ($0000-$07FF), never raises an interrupt (the console has no
source; the vector is covered by core/tests/f8_irq_test.c), and keeps every
branch target inside the image. Random choices come from Rng, so an image is a
function of its seed (f8_ab.py --fresh holds the committed goldens to that).

SPDX-License-Identifier: GPL-3.0-or-later
"""

import re

BASE = 0x0800              # the cartridge's first address
IMG_MAX = 0x1F00           # one probe image; stays clear of the RAM at $2800
FILL = 0x2B                # NOP: a stray run slides along

REGS = ["R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "J", "HU", "HL", "KU", "KL", "QU", "QL"]
ISAR = ["S", "I", "D"]     # ISAR, ISAR++, ISAR--

# ---------------------------------------------------------------- the opcode table
# text -> opcode for the forms with no operand byte (spelled as MAME's disassembler
# prints them, spaces folded); IMM8/ABS16/REL name the ones that take bytes.
OPS = {}
IMM8 = {"LI": 0x20, "NI": 0x21, "OI": 0x22, "XI": 0x23, "AI": 0x24, "CI": 0x25, "IN": 0x26, "OUT": 0x27}
ABS16 = {"PI": 0x28, "JMP": 0x29, "DCI": 0x2A}
REL = {"BR7": 0x8F, "BR": 0x90, "BP": 0x81, "BC": 0x82, "BZ": 0x84, "BM": 0x91, "BNC": 0x92,
       "BNZ": 0x94, "BNO": 0x98}
NIB = {"LISU": 0x60, "LISL": 0x68, "LIS": 0x70, "INS": 0xA0, "OUTS": 0xB0}


def _build():
    for i in range(4):
        OPS["LR A,%s" % REGS[12 + i]] = i
        OPS["LR %s,A" % REGS[12 + i]] = 4 + i
    for name, op in (("LR K,P", 0x08), ("LR P,K", 0x09), ("LR A,IS", 0x0A), ("LR IS,A", 0x0B),
                     ("PK", 0x0C), ("LR P0,Q", 0x0D), ("LR Q,DC", 0x0E), ("LR DC,Q", 0x0F),
                     ("LR DC,H", 0x10), ("LR H,DC", 0x11), ("SR 1", 0x12), ("SL 1", 0x13),
                     ("SR 4", 0x14), ("SL 4", 0x15), ("LM", 0x16), ("ST", 0x17), ("COM", 0x18),
                     ("LNK", 0x19), ("DI", 0x1A), ("EI", 0x1B), ("POP", 0x1C), ("LR W,J", 0x1D),
                     ("LR J,W", 0x1E), ("INC", 0x1F), ("NOP", 0x2B), ("XDC", 0x2C), ("CLR", 0x70),
                     ("AM", 0x88), ("AMD", 0x89), ("NM", 0x8A), ("OM", 0x8B), ("XM", 0x8C),
                     ("CM", 0x8D), ("ADC", 0x8E)):
        OPS[name] = op
    for hi, fmt in ((0x30, "DS %s"), (0xC0, "AS %s"), (0xD0, "ASD %s"), (0xE0, "XS %s"),
                    (0xF0, "NS %s")):
        for i in range(12):
            OPS[fmt % REGS[i]] = hi + i
        for i in range(3):
            OPS[fmt % ISAR[i]] = hi + 12 + i
    for i in range(12):
        OPS["LR A,%s" % REGS[i]] = 0x40 + i
        OPS["LR %s,A" % REGS[i]] = 0x50 + i
    for i in range(3):
        OPS["LR A,%s" % ISAR[i]] = 0x4C + i
        OPS["LR %s,A" % ISAR[i]] = 0x5C + i


_build()


class _Lenient(dict):
    def __missing__(self, key):
        return 0


class Rng:
    """xorshift64*: the same sequence on every Python, so a probe image is a function of its
    seed alone (the committed goldens can be checked against the generator)"""

    M = 0xFFFFFFFFFFFFFFFF

    def __init__(self, seed):
        self.s = ((seed * 0x9E3779B97F4A7C15) + 1) & self.M or 1

    def _next(self):
        x = self.s
        x ^= x >> 12
        x ^= (x << 25) & self.M
        x ^= x >> 27
        self.s = x
        return (x * 0x2545F4914F6CDD1D) & self.M

    def randrange(self, n):
        return (self._next() >> 16) % n

    def choice(self, seq):
        return seq[self.randrange(len(seq))]

    def random(self):
        return (self._next() >> 11) / float(1 << 53)


def num(tok, labels, here):
    tok = tok.strip()
    m = re.fullmatch(r"H'([0-9A-Fa-f]+)'", tok)
    if m:
        return int(m.group(1), 16)
    expr = tok.replace("$", "0x") if re.search(r"\$[0-9A-Fa-f]", tok) and "0x" not in tok else tok
    env = _Lenient(labels) if isinstance(labels, _Lenient) else dict(labels)
    env["_"] = here
    return int(eval(expr, {"__builtins__": {}}, env))


def asm(lines, org=BASE, lenient=False):
    """Assemble the source (a list of lines, or one string) -> (dict addr -> byte, labels).
    Syntax: `label:` on its own or before an instruction; `; comment`; .org N;
    .byte a,b,...; .equ name,value; instructions as MAME prints them
    ("LR   A,KU", "BT   H'01',H'0810'") with numbers as 0x12, $12 or H'12'; branch and
    PI/JMP/DCI operands may be labels or label+n; `_` is the current address."""
    if isinstance(lines, str):
        lines = lines.splitlines()
    labels = _Lenient() if lenient else {}
    for pass_no in (1, 2):
        mem = {}
        pc = org
        for raw in lines:
            line = raw.split(";")[0].strip()
            while True:
                m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*):\s*", line)
                if not m:
                    break
                if pass_no == 1:
                    assert m.group(1) not in labels, "label %s twice" % m.group(1)
                    labels[m.group(1)] = pc
                line = line[m.end():]
            if not line:
                continue
            u = re.sub(r"\s+", " ", line).strip()
            if line.startswith(".org"):
                pc = num(line[4:], labels, pc)
                continue
            if line.startswith(".equ"):
                k, v = line[4:].split(",", 1)
                if pass_no == 1:
                    labels[k.strip()] = num(v, labels, pc)
                continue
            if line.startswith(".byte"):
                bs = [num(t, labels if pass_no == 2 else {}, pc) & 0xFF if pass_no == 2 else 0
                      for t in line[5:].split(",")]
                for b in bs:
                    mem[pc] = b
                    pc += 1
                continue
            mn, _, rest = u.partition(" ")
            mn = mn.upper()
            rest = rest.replace(" ", "")
            if mn in ("LR", "DS", "AS", "ASD", "XS", "NS"):     # a register by number: LR 5,A / DS 3
                rest = ",".join(REGS[int(t)] if t.isdigit() else t for t in rest.split(","))
            key = (mn + " " + rest.upper()).strip()
            ev = lambda t: num(t, labels, pc) if pass_no == 2 else 0
            if key in OPS:
                emit = [OPS[key]]
            elif mn in IMM8:
                emit = [IMM8[mn], ev(rest) & 0xFF]
            elif mn in ABS16:
                v = ev(rest) & 0xFFFF
                emit = [ABS16[mn], v >> 8, v & 0xFF]
            elif mn in REL:
                t = ev(rest)
                off = t - (pc + 1)
                assert pass_no == 1 or lenient or -128 <= off <= 127, "branch out of range at %04X -> %04X" % (pc, t)
                emit = [REL[mn], off & 0xFF]
            elif mn in ("BT", "BF"):
                n, _, tgt = rest.partition(",")
                n = ev(n)
                t = ev(tgt)
                off = t - (pc + 1)
                assert pass_no == 1 or lenient or -128 <= off <= 127, "branch out of range at %04X -> %04X" % (pc, t)
                assert 0 <= n < (8 if mn == "BT" else 16)
                emit = [(0x80 if mn == "BT" else 0x90) + n, off & 0xFF]
            elif mn in NIB:
                n = ev(rest)
                lim = {"LISU": 8, "LISL": 8, "LIS": 16, "INS": 16, "OUTS": 16}[mn]
                assert 0 <= n < lim, line
                emit = [NIB[mn] + n]
            else:
                raise AssertionError("cannot assemble: %r" % raw)
            for b in emit:
                mem[pc] = b
                pc += 1
    return mem, labels


class Image:
    """A probe cartridge image under construction: asm text in, bytes out."""

    def __init__(self):
        self.src = []

    def add(self, *lines):
        for l in lines:
            self.src.extend(l.splitlines())

    def build(self):
        mem, labels = asm(self.src)
        img = bytearray([FILL]) * (max(mem) - BASE + 1)
        for a, b in mem.items():
            assert a >= BASE
            img[a - BASE] = b
        return bytes(img), labels


# ---------------------------------------------------------------- generators
E8 = [0x00, 0x01, 0x0F, 0x10, 0x7F, 0x80, 0x81, 0xF0, 0xFF]
PORTS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 0x20, 0x21, 0x24, 0x25, 0x7F, 0x80, 0xFF]
# data counter values: RAM, the ROM's edges and the open bus above it (never the BIOS)
MEM_ADDRS = [0x2800, 0x2801, 0x2810, 0x28FF, 0x2900, 0x2FFE, 0x2FFF, 0x0800, 0x0801, 0x08FF, 0x0900,
             0x0FFF, 0x1000, 0x1234, 0x1EFF, 0x27FE, 0x27FF, 0x3000, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF]
RAM_ADDRS = [0x2800, 0x2801, 0x2810, 0x28FF, 0x2900, 0x2A55, 0x2FFE, 0x2FFF]
MEM_RD = {0x16, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D}    # LM, AM, AMD, NM, OM, XM, CM: read (DC0)


def setw(w):
    return ["LI %d" % w, "LR J,A", "LR W,J"]


def setis(v):
    return ["LI %d" % v, "LR IS,A"]


def byte(*bs):
    return ".byte " + ",".join("0x%02X" % (b & 0xFF) for b in bs)


def kset(lt):
    return ["LI %s>>8" % lt, "LR KU,A", "LI %s" % lt, "LR KL,A"]


def qset(lt):
    return ["LI %s>>8" % lt, "LR QU,A", "LI %s" % lt, "LR QL,A"]


def size_of(lines):
    mem, _ = asm(lines, org=0, lenient=True)
    return len(mem)


class Gen:
    """Probe images under construction: a prologue that sets every register the probe
    can see, the snippets, DONE and the scratchpad's initial values. Snippets go in
    through add(), which starts the next image when one is full."""

    def __init__(self, name, seed, budget=IMG_MAX - 0x140):
        self.name = name
        self.rnd = Rng(seed)
        self.budget = budget
        self.lab = 0
        self.done = []
        self._begin()

    def label(self):
        self.lab += 1
        return "L%d" % self.lab

    def _begin(self):
        r = self.rnd
        self.scr = [r.randrange(256) for _ in range(64)]
        self.src = [".org 0x0800", ".byte 0x55, 0x2B", ".org 0x0802",
                    # the console's latches, then the scratchpad from the table
                    "LIS 0", "OUTS 0", "OUTS 1", "OUTS 4", "OUTS 5",
                    "LIS 0", "LR IS,A", "DCI scrtab",
                    "pl1: LM", "LR S,A", "LR A,IS", "INC", "LR IS,A", "CI 0x40", "BNZ pl1",
                    # PC1, then DC1 and DC0
                    "LI 0x0A", "LR KU,A", "LI 0x55", "LR KL,A", "LR P,K",
                    "DCI 0x1234", "XDC", "DCI 0x2800",
                    # W from J, ISAR, A
                    "LR W,J", "LI 0x2D", "LR IS,A", "LI 0x3C",
                    "start:"]
        self.used = size_of(self.src) + 4 + 64

    def add(self, *lines):
        lines = [l for ln in lines for l in (ln if isinstance(ln, list) else [ln])]
        n = size_of(lines)
        if self.used + n > self.budget:
            self.finish()
            self._begin()
        self.src.extend(lines)
        self.used += n

    def finish(self):
        self.src += ["done: JMP done", "scrtab:", byte(*self.scr)]
        im = Image()
        im.add(*self.src)
        img, labels = im.build()
        assert len(img) <= 0x2000 - 0x100, "%s too big: %d" % (self.name, len(img))
        img += bytes([FILL]) * (0x2000 - len(img))
        self.done.append((img, labels["start"], labels["done"]))

    def result(self):
        self.finish()
        n = len(self.done)
        return [(self.name if n == 1 else "%s_%d" % (self.name, i), img, st, dn)
                for i, (img, st, dn) in enumerate(self.done)]


def rstate(rnd):
    a = rnd.choice(E8) if rnd.random() < 0.4 else rnd.randrange(256)
    return a, rnd.randrange(32), rnd.randrange(64)


def one(g, op, a=None, w=None, is_=None, dc0=None, imm=None, light=False):
    """one instruction under test, in a state set up just before it (not set up at all when
    `light`: whatever the earlier instructions left). Returns the lines."""
    rnd = g.rnd
    ra, rw, ri = rstate(rnd)
    a = ra if a is None else a
    w = rw if w is None else w
    is_ = ri if is_ is None else is_
    L = []
    st = (lambda: []) if light else (lambda: setw(w) + setis(is_))
    if op == 0x1D:                                  # LR W,J: any J
        L += ([] if light else ["LI %d" % rnd.randrange(256), "LR J,A"] + setis(is_))
        L += [byte(op)]
        return L
    if op in (0x0C, 0x0D):                          # PK, LR P0,Q: land on the next instruction
        lt = g.label()
        L += (kset(lt) if op == 0x0C else qset(lt)) + st() + ([] if light else ["LI %d" % a]) + [byte(op), lt + ":"]
        return L
    if op == 0x1C:                                  # POP: PC1 = the next instruction
        lt = g.label()
        L += kset(lt) + ["LR P,K"] + st() + ([] if light else ["LI %d" % a]) + [byte(op), lt + ":"]
        return L
    L += st()
    if op in MEM_RD or op == 0x17:
        L += ["DCI %d" % (dc0 if dc0 is not None else
                          rnd.choice(RAM_ADDRS if op == 0x17 else MEM_ADDRS))]
    if not light:
        L += ["LI %d" % a]
    if op in (0x28, 0x29):
        lt = g.label()
        L += ["%s %s" % ("PI" if op == 0x28 else "JMP", lt), lt + ":"]
    elif 0x80 <= op <= 0x87 or op == 0x8F or 0x90 <= op <= 0x9F:
        lt = g.label()
        L += ["%s %s" % (_branch_text(op), lt), "LIS 5", lt + ":"]
    elif op == 0x2A:
        L += ["DCI %d" % (imm if imm is not None else rnd.choice(MEM_ADDRS + [rnd.randrange(0x10000)]))]
    elif op in IMM8.values():
        v = imm if imm is not None else (rnd.choice(E8) if rnd.random() < 0.5 else rnd.randrange(256))
        if op in (0x26, 0x27):
            v = imm if imm is not None else rnd.choice(PORTS)
        L += [byte(op, v)]
    else:
        L += [byte(op)]
    return L


def _branch_text(op):
    if op == 0x8F:
        return "BR7"
    if op < 0x88:
        return "BT %d," % (op & 7)
    return "BF %d," % (op & 15) if op != 0x90 else "BR"


def ops_probe():
    g = Gen("ops", 0xF8001)
    for op in range(256):
        for _ in range(12):
            g.add(one(g, op))
    return g.result()


def isar_probe():
    g = Gen("isar", 0xF8002)
    groups = [0x3C, 0x4C, 0x5C, 0xCC, 0xDC, 0xEC, 0xFC]
    for base in groups:
        for mode in range(3):
            for v in range(64):
                g.add(one(g, base + mode, is_=v))
    for op in range(0x60, 0x70):
        for v in range(0, 64, 3):
            g.add(one(g, op, is_=v))
    for v in range(256):
        g.add(one(g, 0x0B, a=v))                    # LR IS,A masks to 6 bits
    for v in range(0, 64):
        g.add(one(g, 0x0A, is_=v))
    return g.result()


def alu_probe():
    g = Gen("alu", 0xF8003)
    for op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25):
        for a in E8:
            for i in E8:
                g.add(one(g, op, a=a, imm=i))
    for op in (0x12, 0x13, 0x14, 0x15, 0x18, 0x19, 0x1F, 0x70):
        for a in E8 + [0x55, 0xAA, 0x3C]:
            for w in (0, 2, 3, 0x1f, 0x10):
                g.add(one(g, op, a=a, w=w))
    return g.result()


def branch_probe():
    g = Gen("branch", 0xF8004)
    for t in range(8):
        for w in range(32):
            g.add(one(g, 0x80 + t, w=w))
    for t in range(16):
        for w in range(32):
            g.add(one(g, 0x90 + t, w=w))
    for v in range(64):
        g.add(one(g, 0x8F, is_=v))
    # backward branches: the taken path jumps back to a LIS 9 that rejoins
    for op in list(range(0x80, 0x88)) + [0x8F] + list(range(0x90, 0xA0)):
        for _ in range(6):
            _, w, v = rstate(g.rnd)
            back, fwd, cont = g.label(), g.label(), g.label()
            g.add(["JMP " + fwd, back + ":", "LIS 9", "JMP " + cont, fwd + ":"] + setw(w) + setis(v) +
                  ["%s %s" % (_branch_text(op), back), "LIS 5", cont + ":"])
    return g.result()


def sweep_probe(name, seed, fn):
    """A full sweep of a two-operand add: r1 = A counts up, the operand r3 takes all 256
    values. fn gives the loop body."""
    g = Gen(name, seed)
    L1, L2 = g.label(), g.label()
    g.add(["LIS 0", "LR 1,A", "LR 3,A"] + fn(g, L1, L2))
    return g.result()


def as_probe():
    def body(g, L1, L2):
        return [L1 + ":", "LI 0", "LR 3,A", L2 + ":", "LR A,1", "AS 3", "DS 3", "BNZ " + L2,
                "LR A,1", "INC", "LR 1,A", "BNZ " + L1]
    return sweep_probe("as", 0xF8005, body)


def asd_probe():
    def body(g, L1, L2):
        return [L1 + ":", "LI 0", "LR 3,A", L2 + ":", "LR A,1", "ASD 3", "DS 3", "BNZ " + L2,
                "LR A,1", "INC", "LR 1,A", "BNZ " + L1]
    return sweep_probe("asd", 0xF8006, body)


def amd_probe():
    """AMD against every byte of a 256-byte identity table, for every A"""
    g = Gen("amd", 0xF8007)
    L1, L2 = g.label(), g.label()
    g.add(["LIS 0", "LR 1,A", "LI idtab>>8", "LR HU,A", "LI idtab", "LR HL,A",
           L1 + ":", "LR DC,H", "LI 0", "LR 3,A", L2 + ":", "LR A,1", "AMD", "DS 3", "BNZ " + L2,
           "LR A,1", "INC", "LR 1,A", "BNZ " + L1])
    g.src += ["JMP done_amd", ".org (_ + 255) & ~255", "idtab:", byte(*range(256)), "done_amd:"]
    return g.result()


def fuzz_probe(n, seed):
    g = Gen("fuzz%d" % n, seed)
    r = g.rnd
    for _ in range(2400):
        op = r.randrange(256)
        g.add(one(g, op, light=r.random() < 0.6))
    return g.result()


def mem_probe():
    g = Gen("mem", 0xF8008)
    L = g.label
    f1, f2, f3, f4, f5, f6, f7, f8 = [L() for _ in range(8)]
    g.add(["; RAM filled and read back through ST and LM",
           "LIS 0", "LR 4,A", "DCI 0x2800",
           f1 + ":", "LR A,4", "COM", "ST", "DS 4", "BNZ " + f1,
           "DCI 0x2800", "LIS 0", "LR 5,A", "LR 4,A",
           f2 + ":", "LM", "AS 5", "LR 5,A", "DS 4", "BNZ " + f2])
    for name in ("AM", "AMD", "NM", "OM", "XM", "CM"):
        lp = L()
        g.add(["DCI 0x2800", "LI 0x5A", "LR 4,A", "LI 64", "LR 4,A", "LI 0x93",
               lp + ":", name, "DS 4", "BNZ " + lp])
    # the ROM's last bytes, the RAM, the open bus above it, a page edge, the top of memory
    for base in (0x27FE, 0x2FFE, 0x08FE, 0x0FFF, 0xFFFE, 0x3FFF):
        n = 2 if base == 0xFFFE else 4      # a read past $FFFF would be the BIOS at $0000
        g.add(["DCI %d" % base] + ["LM", "LR 7,A"] * n)
        g.add(["DCI %d" % base, "LI 0x3C"] + ["AM"] * (n - 1) + ["CM"])
    # ST past the end of RAM, into ROM and the open bus; read back
    for base in (0x28FE, 0x2FFE, 0x27FE, 0x0900, 0x3000):
        g.add(["DCI %d" % base, "LI 0xA5", "ST", "LI 0x5A", "ST", "LI 0x33", "ST",
               "DCI %d" % base, "LM", "LM", "LM"])
    # ADC: signed offsets onto the data counter
    for base, a in ((0x28F0, 0x20), (0x2900, 0xF0), (0x2800, 0x80), (0x2800, 0x7F), (0x2810, 0xFF),
                    (0x2800, 0x00), (0x0900, 0xE0), (0x2FF0, 0x7F), (0xFFF0, 0x20), (0x0801, 0xFF)):
        g.add(["DCI %d" % base, "LI %d" % a, "ADC", "LM", "LM"]
              if ((base + (a - 256 if a >= 128 else a)) & 0xFFFF) in range(0x800, 0xFFFE)
              else ["DCI %d" % base, "LI %d" % a, "ADC"])
    # the second data counter: XDC, LR DC,Q / Q,DC / DC,H / H,DC, DCI over a swapped counter
    g.add(["DCI 0x2800", "XDC", "DCI 0x2900", "LI 0x11", "ST", "XDC", "LI 0x22", "ST", "XDC", "LM", "XDC", "LM",
           "LR Q,DC", "LR H,DC", "XDC", "LR DC,Q", "LM", "LR DC,H", "LM", "XDC", "LR H,DC", "LR Q,DC",
           "DCI 0x28FF", "LR Q,DC", "LI 0x66", "ST", "ST", "LR DC,Q", "LM", "LM"])
    return g.result()


def io_probe():
    g = Gen("io", 0xF8009)
    vals = [0x00, 0xFF, 0x40, 0xBF, 0x55, 0xAA, 0x80, 0x01, 0x20, 0xDF, 0x7F]
    for p in (0, 1, 4, 5):
        for v in vals:
            for gate in (0x00, 0x40):
                g.add(["LI %d" % gate, "OUT 0", "LI %d" % v, "OUT %d" % p,
                       "LIS 3", "IN 0", "LIS 3", "IN 1", "LIS 3", "IN 4", "LIS 3", "IN 5"])
        for v in vals:
            for gate in (0x00, 0x40):
                g.add(["LI %d" % gate, "OUTS 0", "LI %d" % v, byte(0xB0 + p if p < 2 else 0xB0 + p),
                       "LIS 3", "INS 0", "LIS 3", "INS 1", "LIS 3", byte(0xA4), "LIS 3", byte(0xA5)])
    for p in range(256):                             # every port: unmapped ones read 0 and swallow writes
        g.add(["LI 0x40", "OUT 0", "LI 0x6B", "OUT %d" % p if p not in (0, 1, 4, 5) else "OUT 5", "LIS 9", "IN %d" % p])
    for n in range(4, 16):
        g.add(["LI 0x40", "OUT 0", "LI 0x35", byte(0xB0 + n), "LIS 9", byte(0xA0 + n), "LI 0", "OUT 0"])
    return g.result()


def flow_probe():
    """control flow at the places an implementation gets wrong: page crossings, the
    subroutine idioms (PI / POP, PK, the K stack) and the registers they move"""
    g = Gen("flow", 0xF8010)
    g.src += """
        JMP t1
        ; branches across a 256-byte boundary, forward, with the offset byte on the next page, backward
        .org 0x0AFD
t1:     BR t1x
        .org 0x0B10
t1x:    LIS 1
        JMP t2
        .org 0x0AFF
t2:     BR t2x
        .org 0x0B18
t2x:    LIS 2
        JMP t3
        .org 0x0AF0
t3g:    LIS 3
        JMP t4
        .org 0x0B02
t3:     BR t3g
        ; three-byte instructions with their operands on the next page
        .org 0x0CFE
t4:     PI t4x
        .org 0x0D20
t4x:    LI 0x55
        JMP t5
        .org 0x0DFF
t5:     JMP t5x
        .org 0x0E30
t5x:    LI 0x66
        JMP t6
        .org 0x0EFF
t6:     DCI 0x2812
        LM
        LR 5,A
        JMP t7
        ; a countdown whose backward branch and whose XI straddle pages
        .org 0x10F8
t7:     LIS 5
        LR 6,A
lp:     DS 6
        LR A,6
        SL 1
        AI 3
        XI 0x0F
        BNZ lp
        JMP t8
        ; PI, POP, the K stack (LR K,P / LR P,K), PK
        .org 0x1200
t8:     PI s1
r1:     LIS 8
        PI s3
r3:     JMP t9
s1:     LR K,P
        LI 0x77
        PI s2
r2:     LR P,K
        POP
s2:     INC
        POP
s3:     LI r3>>8
        LR KU,A
        LI r3
        LR KL,A
        PK
        ; PC1 and K round trips, LR P0,Q to another page
t9:     PI y1
y1:     LR K,P
        LR A,KU
        LR 1,A
        LR A,KL
        LR 2,A
        LR P,K
        LI 0x3C
        LR QU,A
        LI 0x41
        LR QL,A
        LR Q,DC
        LR H,DC
        LI t10>>8
        LR QU,A
        LI t10
        LR QL,A
        LR P0,Q
        .org 0x1500
t10:    LIS 10
        PI far
back:   PI far2
        JMP t11
        .org 0x1840
far:    LI 0xC3
        POP
        .org 0x1960
far2:   LI 0x00
        POP
        ; a 3-byte JMP / PI / DCI taking all of A's possible high bytes
t11:    JMP t12
        .org 0x1A00
t12:    JMP t13
        .org 0x1B00
t13:    PI t14
        JMP done_flow
        .org 0x1C00
t14:    LIS 14
        POP
        .org 0x1D00
done_flow:
""".splitlines()
    return g.result()


# ---------------------------------------------------------------- the registry
def _all():
    out = []
    for fn in (ops_probe, isar_probe, alu_probe, branch_probe, as_probe, asd_probe, amd_probe, mem_probe,
               io_probe, flow_probe, lambda: fuzz_probe(1, 0xF8101), lambda: fuzz_probe(2, 0xF8102),
               lambda: fuzz_probe(3, 0xF8103), lambda: fuzz_probe(4, 0xF8104)):
        out.extend(fn())
    return out


_CACHE = {}


def probes():
    """name -> (image, start, done), in a stable order"""
    if not _CACHE:
        for name, img, start, done in _all():
            assert name not in _CACHE, name
            _CACHE[name] = (img, start, done)
    return _CACHE


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "--list":
        for n, (img, st, dn) in probes().items():
            print("%-8s start %04X done %04X" % (n, st, dn))
    elif len(sys.argv) > 2 and sys.argv[1] == "--write":
        import os
        os.makedirs(sys.argv[2], exist_ok=True)
        for n, (img, st, dn) in probes().items():
            with open(os.path.join(sys.argv[2], n + ".bin"), "wb") as f:
                f.write(img)
            print("%-8s start %04X done %04X" % (n, st, dn))
    else:
        print("usage: f8_probes.py --list | --write DIR")
