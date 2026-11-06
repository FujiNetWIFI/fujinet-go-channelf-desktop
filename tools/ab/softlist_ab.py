#!/usr/bin/env python3
"""softlist_ab.py -- every Videocart type against MAME, through a software list.

    softlist_ab.py [--machine M] [--only NAME] [--report]

MAME picks a Videocart's PCB from the software list's `slot` feature, which a
plain `-cart` file never carries (it is always the `chess` type), so the 2102
RAM of Maze and Hangman, the 2K RAM of Schach, the bank latches of the two
multicarts and the plain ROM-only cart are reached here through a private
`-hashpath` holding synthetic entries (our own images, our own CRCs; nothing
from the real list) and a private `-rompath` holding the images. The probe
machine (tools/ab/probes/vm.asm) runs on each, working the type's own hardware
through port I/O and memory accesses and logging what it reads, and chf_headless
runs the same image with `-type`. Compared frame for frame, VRAM, latches and
every I/O write with its CPU clock (the reads are logged as writes to port $F0).

  std         reads past the ROM, where a Schach's RAM would be, and stores that go nowhere
  maze        the 2102 on ports $24/$25: a sweep of addresses written and read back
  hangman     the same on $20/$21
  chess       2K of RAM at $2800-$2FFF
  multi_old   8K banks selected by writes in $3000-$3FFF, RAM at $2800
  multi       the same with the half-bank bit
  *_reset     a console RESET in the middle (MAME's device_reset): the 2102's latches and a
              multicart's bank latch return to power-on, the RAM contents stay

Exit status 1 on any difference; 77 without the real BIOS, MAME or the assembler.
"""
import argparse
import hashlib
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chfab  # noqa: E402
from chfab import Script  # noqa: E402


def rnd(seed, n):
    """Deterministic filler (xorshift32), not ROM data."""
    x = seed or 1
    out = bytearray()
    for _ in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(x & 0xFF)
    return bytes(out)


def ram2102(s, pa, pb):
    """Write a sweep of cells through the 2102's two ports, then read them back."""
    cells = [((i * 37 + 11) & 0xFF, (i * 53 + 7) & 0xFF) for i in range(80)]
    for b, a in cells:
        s.out(pb, b).out(pa, a | 1)             # port A bit 0 = write; bit 3 = the data bit; bits 1, 2 = address bits
        s.inp(pa).inp(pb)
    for b, a in cells:
        s.out(pb, b).out(pa, a & 0xFE)          # read cycle
        s.inp(pa).inp(pb)
    for b in range(0, 256, 5):                  # addresses never written
        s.out(pb, b).out(pa, 0x00).inp(pa)
    s.inp(0x24 if pa == 0x20 else 0x20)         # the other cart's ports answer nothing
    return s


def std_script():
    s = Script()
    for a in (0x0900, 0x0F00, 0x0FFF, 0x1000, 0x17FF, 0x1800, 0x2000, 0x27FF, 0x2800, 0x2FFF, 0x3000, 0x4000,
              0x7FFF, 0x8000, 0xFFFF):
        s.load(a)
    s.store(0x0900, 0x5A).load(0x0900)          # ROM does not store
    s.store(0x2800, 0x12).load(0x2800)          # and there is no RAM at $2800
    s.store(0x3000, 0x01).load(0x0F00)          # nor a bank latch
    s.inp(0x24).inp(0x25).inp(0x20).inp(0x21).out(0x24, 0xFF).inp(0x24)
    return s


def maze_script():
    return ram2102(Script(), 0x24, 0x25)


def hangman_script():
    return ram2102(Script(), 0x20, 0x21)


def chess_script():
    s = Script()
    for off in range(0, 0x800, 0x41):
        s.store(0x2800 + off, (off * 3 + 1) & 0xFF)
    for off in range(0, 0x800, 0x41):
        s.load(0x2800 + off)
    for a in (0x27FF, 0x2800, 0x2FFF, 0x3000, 0x1000, 0x1FFF, 0x2000, 0x4FFF):
        s.load(a)
    s.store(0x27FF, 0x77).store(0x3000, 0x88).load(0x27FF).load(0x3000)
    s.pix(10, 10, 1)
    return s


def multi_script(final):
    s = Script()
    for b in (0, 1, 2, 3, 5, 9, 17, 30):
        s.store(0x3000, b).load(0x1000).load(0x1001).load(0x1700).load(0x1900).load(0x2000).load(0x27FF)
    s.store(0x3FFF, 4).load(0x1000).load(0x1800)         # anywhere in the latch window
    s.store(0x4000, 6).load(0x1000)                      # outside it
    s.store(0x2FFF, 0x61).load(0x2FFF).store(0x2800, 0x62).load(0x2800)
    for b in (0x20, 0x21, 0x22, 0x25, 0x3E):             # bit 5: the half bank (multi), ignored upward (multi_old)
        s.store(0x3000, b).load(0x1000).load(0x1001).load(0x1800)
    s.store(0x3000, 0x00).load(0x1000)
    s.pix(12, 12, 2)
    return s


def multi_image(script, work, final):
    """256K: the probe machine and script at the start of every 4K block (so the
    code survives any bank or half-bank), distinct filler after them."""
    vm = chfab.vm_image(work) + bytes(script.b) + b"\x00"
    assert len(vm) < 0x800, "the probe's script must fit under $1000 of every block"
    blocks = []
    for blk in range(64):
        body = vm + b"\xFF" * (0x800 - len(vm)) + bytes((blk,)) + rnd(0xC0DE + blk, 0x7FF)
        blocks.append(body)
    return b"".join(blocks)


def reset_script(kind):
    """What the script reads FIRST shows the state a RESET left (the probe restarts at its first op)."""
    s = Script()
    if kind == "maze":
        s.inp(0x24).inp(0x25).out(0x25, 0xA5).out(0x24, 0x0B).inp(0x24).inp(0x25)
        s.out(0x24, 0x00).inp(0x24)
    else:
        s.load(0x1000).load(0x1801).store(0x2800, 0x33).load(0x2800)
        s.store(0x3000, 0x25).load(0x1000).load(0x1801)
    return s


CARTS = {
    # name: (slot, builder, size, ram bytes, our -type)
    "std": ("std", std_script, 0x800, 0, "std"),
    "maze": ("maze", maze_script, 0x800, 1024, "maze"),
    "hangman": ("hangman", hangman_script, 0xC00, 1024, "hangman"),
    "chess": ("chess", chess_script, 0x1800, 2048, "chess"),
    "multi_old": ("multi_old", lambda: multi_script(False), 0x40000, 2048, "multi_old"),
    "multi": ("multi", lambda: multi_script(True), 0x40000, 2048, "multi"),
    "maze_reset": ("maze", lambda: reset_script("maze"), 0x800, 1024, "maze"),
    "multi_old_reset": ("multi_old", lambda: reset_script("multi"), 0x40000, 2048, "multi_old"),
    "multi_reset": ("multi", lambda: reset_script("multi"), 0x40000, 2048, "multi"),
}


def write_list(work, entries):
    """The private software list and its ROM tree. entries: [(name, slot, image, ram)]."""
    hashd = os.path.join(work, "sl-hash")
    romd = os.path.join(work, "sl-roms", "channelf")
    os.makedirs(hashd, exist_ok=True)
    out = ['<?xml version="1.0"?>', '<softwarelist name="channelf" description="private A/B list">']
    for name, slot, image, ram in entries:
        d = os.path.join(romd, name)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, name + ".bin"), "wb").write(image)
        out += ['<software name="%s"><description>%s</description><year>2026</year><publisher>-</publisher>' % (name, name),
                '<part name="cart" interface="channelf_cart"><feature name="slot" value="%s"/>' % slot,
                '<dataarea name="rom" size="%d"><rom name="%s.bin" size="%d" crc="%08x" sha1="%s"/></dataarea>' % (
                    len(image), name, len(image), zlib.crc32(image) & 0xFFFFFFFF, hashlib.sha1(image).hexdigest())]
        if ram:
            out.append('<dataarea name="ram" size="%d"></dataarea>' % ram)
        out.append("</part></software>")
    out.append("</softwarelist>")
    open(os.path.join(hashd, "channelf.xml"), "w").write("\n".join(out))
    return hashd, os.path.join(work, "sl-roms")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--machine", default="channelf", choices=sorted(chfab.MACHINES))
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    if not chfab.bios_dir() or not os.path.exists(chfab.MAME) or not chfab.have_asl():
        print("SKIP: needs the real BIOS (CHF_BIOS_DIR), MAME and the assembler")
        return 77
    work = chfab.work_dir()
    entries, files = [], {}
    for name, (slot, build, size, ram, typ) in CARTS.items():
        if args.only and name not in args.only:
            continue
        script = build()
        if size == 0x40000:
            image = multi_image(script, work, name == "multi")
        else:
            image = script.image(work, size)
            image += b"\xFF" * (size - len(image)) if len(image) < size else b""
            image = rnd(0x1234, 0) + image[:size] if len(image) <= size else image
        entries.append(("t" + name, slot, image, ram))
        files["t" + name] = (image, typ)
    hashd, romd = write_list(work, entries)
    bad = 0
    for sw, (image, typ) in files.items():
        path = os.path.join(work, sw + ".bin")
        open(path, "wb").write(image)
        mame_rom = "%s;%s" % (os.path.join(chfab.MAME_DIR, "roms"), romd)
        r = chfab.ab(args.machine, path, args.frames, work, iolog=True, softlist=sw, game_type=typ,
                     resets=(20, 25) if sw.endswith("_reset") else (),
                     mame_extra=["-hashpath", hashd, "-rompath", mame_rom])
        if not r.ok:
            bad += 1
        if args.report or not r.ok:
            print(chfab.report(sw[1:] + " (" + typ + ")", args.machine, r))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
