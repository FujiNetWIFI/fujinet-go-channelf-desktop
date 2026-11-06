"""biosmodel -- the behaviour of the four Channel F BIOS entry points as
executable models, written from observation only (see SPEC.md).

Every model maps a pre-state to the post-state a cartridge sees when the
routine returns (RESET: when control reaches $0802).  observe.py runs the
models over every observed case; a stand-in BIOS is correct when the same
suite passes against it.

A state is a dict:
    a w is_ dc0 dc1 pc1      A, W (5 bits), ISAR, DC0, DC1, PC1 (at entry: the return address)
    r                        64-entry scratchpad list
    p0 p1 p4 p5              port latches (ports 0, 1, 4, 5)
    vram                     list of 8192 cells (row*128+col), or None
field_tiers() says, per post-state field, whether FujiNet code may rely on it
(1), it is an incidental leftover of the real BIOS (2), or a corner case (3).
"""
import copy

# ---------------------------------------------------------------------------
# $0000 RESET entry, [$0800] == $55
# ---------------------------------------------------------------------------
RESET_A = 0x08
RESET_W_FLAGS = 0x07
RESET_IS = 0x3B
RESET_DC0 = 0x0801
RESET_STACK_PTR = 0x28


def model_reset(pre):
    post = copy.deepcopy(pre)
    post["a"] = RESET_A
    post["w"] = RESET_W_FLAGS | (pre["w"] & 0x10)
    post["is_"] = RESET_IS
    post["dc0"] = RESET_DC0
    post["r"] = [0] * 64
    post["r"][59] = RESET_STACK_PTR
    # dc1, pc1, latches and VRAM are untouched
    return post


# ---------------------------------------------------------------------------
# $0107 push K / $011E pop K
# ---------------------------------------------------------------------------


def model_push(pre):
    post = copy.deepcopy(pre)
    r = post["r"]
    p = r[59] & 0x3F
    r[7] = pre["is_"]                    # ISAR is parked in r7 ...
    r[p] = r[12]                         # KU, then
    r[(p + 1) & 63] = r[13]              # KL (read after the first store)
    r[59] = (p + 2) & 63
    post["a"] = r[7]                     # ... and reloaded from it afterwards
    post["is_"] = r[7] & 0x3F
    post["w"] = (pre["w"] & 0x10) | 0x01
    return post


def pop_flags(p_before):
    q = ((p_before & 0x3F) - 2) & 63
    return {63: 0x0, 0: 0x7}.get(q, 0x3)


def model_pop(pre):
    post = copy.deepcopy(pre)
    r = post["r"]
    p = r[59] & 0x3F
    q = (p - 2) & 63
    r[7] = pre["is_"]
    r[13] = r[(q + 1) & 63]              # KL first,
    r[12] = r[q]                         # then KU
    r[59] = q
    post["a"] = r[7]
    post["is_"] = r[7] & 0x3F
    post["w"] = (pre["w"] & 0x10) | pop_flags(p)
    return post


# ---------------------------------------------------------------------------
# $00D0 clear screen
# ---------------------------------------------------------------------------
# Per first-half BIOS version: the columns every row has written with 0 (vis),
# the seven columns whose content/write status depends on r3 and r31 (hid), the
# value r1 is left with and the last port-4 value.  Columns 111-121 are never written.
VERSIONS = {
    "sl31253": dict(vis=tuple(range(1, 111)), hid=(0, 122, 123, 124, 125, 126, 127), r1=0x73, p4=0x91),
    "sl90025": dict(vis=tuple(range(0, 110)), hid=(110, 122, 123, 124, 125, 126, 127), r1=0x72, p4=0x92),
}
DEFAULT_VERSION = "sl31253"
GAP_COLS = tuple(range(111, 122))

CLR_A = 0x1F
CLR_IS = 0x1F
CLR_W_FLAGS = 0x03
CLR_PC1 = 0x0107
CLR_DC0 = 0x07AD
CLR_CONST = {0: 0x0D, 2: 0x41, 5: 0, 6: 0, 7: 0x1F, 8: 0, 52: 0x1F}   # r1: per version
CLR_STACK_TAIL = (0x00, 0xE7)        # the 3rd/4th stack slot (incidental)
CLR_STACK_MIN, CLR_STACK_MAX = 14, 48   # r59 & 63 range in which the stack slots r59..r59+3 are safe


def clr_base(r3):
    """Key into the hidden-column table: bit 4 and the colour bits do not matter."""
    return r3 & 0x2F


def clr_stack(pre):
    """slot -> value written by the routine's two K pushes (returns the
    caller's return address first, then an internal one)."""
    p = pre["r"][59] & 0x3F
    ret = pre["pc1"]
    vals = (ret >> 8, ret & 0xFF, CLR_STACK_TAIL[0], CLR_STACK_TAIL[1])
    return {(p + i) & 63: v for i, v in enumerate(vals)}


def clr_mode(pre):
    """r31 bits 1-0 (as the routine sees r31, i.e. after its own stack pushes,
    which can land on r31 when r59 & 63 is 28..31) select the hidden-column
    pattern set: 0 -> 'A', else 'B'.  It only matters when r3 != 0."""
    r31 = clr_stack(pre).get(31, pre["r"][31])
    return "A" if (r31 & 3) == 0 else "B"


def model_clr(pre, hidden, ver=DEFAULT_VERSION):
    """hidden: {'A'|'B': {base: [64 strings of 7 chars from '0' 'c' 'x']}}
    ('c' = the colour r3 >> 6, 'x' = cell not written)."""
    post = copy.deepcopy(pre)
    r = post["r"]
    r3 = pre["r"][3]
    mode = clr_mode(pre)
    r3_post = r3 if mode == "A" else 0
    ret = pre["pc1"]                     # PC1 at entry is the return address
    p = r[59] & 0x3F
    colour = r3 >> 6
    for k, v in CLR_CONST.items():
        r[k] = v
    r[1] = VERSIONS[ver]["r1"]
    r[3] = r3_post
    r[53] = r3_post
    r[54] = pre["r"][4]
    r[12], r[13] = ret >> 8, ret & 0xFF
    for slot, v in clr_stack(pre).items():
        r[slot] = v
    r[59] = p
    post["a"] = CLR_A
    post["is_"] = CLR_IS
    post["w"] = (pre["w"] & 0x10) | CLR_W_FLAGS
    post["pc1"] = CLR_PC1
    post["dc0"] = CLR_DC0
    # ports: the sound bits of port 5 survive, the rest is the last cell written
    post["p0"], post["p1"], post["p4"] = 0x00, 0xFF, VERSIONS[ver]["p4"]
    post["p5"] = 0x3F | (pre["p5"] & 0xC0)
    if pre.get("vram") is not None:
        v = list(pre["vram"])
        tab = hidden[mode][clr_base(r3)]
        for row in range(64):
            o = row * 128
            for c in VERSIONS[ver]["vis"]:
                v[o + c] = 0
            line = tab[row]
            for k, c in enumerate(VERSIONS[ver]["hid"]):
                s = line[k]
                if s != "x":
                    v[o + c] = colour if s == "c" else 0
        post["vram"] = v
    return post


def clr_clobbered(pre):
    """Scratchpad bytes the real routine overwrites (tier 2 = real-BIOS
    leftovers; every other byte, and r59 itself, must survive)."""
    p = pre["r"][59] & 0x3F
    return {0, 1, 2, 3, 5, 6, 7, 8, 12, 13, 52, 53, 54} | {(p + k) & 63 for k in range(4)}


# ---------------------------------------------------------------------------
# tiers: 1 = FujiNet code may rely on it, 2 = real-BIOS leftover (reproduce if
# cheap), 3 = corner case / no promise.  Fields: r0..r63, w.I, w.flags, a, is_,
# dc0, dc1, pc1, p0, p1, p4, p5.sound (bits 7-6), p5.row (bits 5-0), vram.
# ---------------------------------------------------------------------------
def clr_domain1(pre):
    """r3 = 0 (any r31), or a colour-only r3 ($40/$80/$C0) with r31 & 3 = 0."""
    r3 = pre["r"][3]
    return (r3 & 0x3F) == 0 and (r3 == 0 or clr_mode(pre) == "A")


def field_tiers(routine, pre):
    t = {}
    p = pre["r"][59] & 63
    if routine == "reset":
        for k in range(64):
            t["r%d" % k] = 1
        t.update({"w.I": 1, "w.flags": 2, "a": 2, "is_": 2, "dc0": 2, "dc1": 2, "pc1": 2,
                  "p0": 1, "p1": 1, "p4": 1, "p5.sound": 1, "p5.row": 1, "vram": 1})
    elif routine == "clr":
        d1 = clr_domain1(pre)
        clob = clr_clobbered(pre)
        for k in range(64):
            t["r%d" % k] = (2 if d1 else 3) if (k in clob and k != 59) else (1 if d1 else 3)
        if pre["r"][59] > 63:               # the real routine leaves r59 & 63; nobody passes r59 >= 64
            t["r59"] = 3
        t.update({"w.I": 1, "w.flags": 2, "a": 2, "is_": 2, "dc0": 2, "dc1": 1, "pc1": 2,
                  "p0": 1, "p1": 2, "p4": 2, "p5.sound": 1, "p5.row": 2, "vram": 1 if d1 else 3})
    else:
        stored = {p, (p + 1) & 63} if routine == "push" else set()
        for k in range(64):
            t["r%d" % k] = 1 if (k != 7 or 7 in stored) else 2
        corner = routine == "push" and 7 in stored
        t.update({"w.I": 1, "w.flags": 2, "a": 3 if corner else 2, "is_": 3 if corner else 1,
                  "dc0": 1, "dc1": 1, "pc1": 1, "p0": 1, "p1": 1, "p4": 1,
                  "p5.sound": 1, "p5.row": 1, "vram": 1})
    return t
